// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Library/LibraryScanner.h"

#include <windows.h>
#include <wil/resource.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <future>
#include <fstream>
#include <map>
#include <set>
#include <stop_token>
#include <thread>

#include <taglib/aifffile.h>
#include <taglib/dsdifffile.h>
#include <taglib/dsffile.h>
#include <taglib/flacfile.h>
#include <taglib/mp4file.h>
#include <taglib/mpegfile.h>
#include <taglib/tpropertymap.h>
#include <taglib/tfilestream.h>
#include <taglib/wavfile.h>

#include "Common/util/Log.h"
#include "Common/util/Text.h"
#include "Engine/decode/FormatProbe.h"
#include "Engine/decode/IDecoder.h"

namespace camel::server {
namespace {

using Path = std::filesystem::path;
using Clock = std::chrono::steady_clock;

int64_t now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::wstring lower(std::wstring s) {
    if (!s.empty() && !LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.data(), int(s.size()), s.data(), int(s.size()),
                                     nullptr, nullptr, 0))
        throw std::runtime_error("cannot normalize library path");
    return s;
}

wil::unique_handle openPath(const Path& path, DWORD access = FILE_READ_ATTRIBUTES, DWORD flags = FILE_FLAG_BACKUP_SEMANTICS) {
    return wil::unique_handle(CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                          OPEN_EXISTING, flags, nullptr));
}

Path canonicalRoot(const Path& path) {
    auto handle = openPath(path);
    if (!handle) throw std::runtime_error("cannot access library path: " + toUtf8(path.wstring()));
    const DWORD count = GetFinalPathNameByHandleW(handle.get(), nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!count) throw std::runtime_error("cannot resolve library path");
    std::wstring text(count, L'\0');
    const DWORD size = GetFinalPathNameByHandleW(handle.get(), text.data(), count, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!size || size >= count) throw std::runtime_error("cannot resolve library path");
    text.resize(size);
    Path result(lower(text));
    while (result != result.root_path() && result.filename().empty()) result = result.parent_path();
    return result;
}

std::string key(const Path& path) {
    return toUtf8(lower(path.wstring()));
}

bool contains(const Path& parent, const Path& child) {
    auto p = parent.begin(), c = child.begin();
    for (; p != parent.end(); ++p, ++c)
        if (c == child.end() || *p != *c) return false;
    return true;
}

struct Version {
    int64_t size = 0, mtime = 0, volume = 0;
    std::vector<uint8_t> index;
    bool operator==(const Version&) const = default;
};

Version version(const Path& path) {
    auto h = openPath(path);
    BY_HANDLE_FILE_INFORMATION info{};
    if (!h || !GetFileInformationByHandle(h.get(), &info)) throw std::runtime_error("cannot read file identity: " + key(path));
    const uint64_t index = (uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    Version out{int64_t((uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow),
                int64_t((uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime),
                info.dwVolumeSerialNumber,
                {}};
    out.index.resize(sizeof(index));
    memcpy(out.index.data(), &index, sizeof(index));
    return out;
}

std::string kind(const Path& path) {
    const auto ext = lower(path.extension().wstring());
    if (ext == L".cue") return "cue";
    if (ext == L".lrc") return "lrc";
    if (ext == L".jpg" || ext == L".jpeg" || ext == L".png" || ext == L".webp") return "image";
    for (const auto* e :
         {L".flac", L".m4a", L".mp4", L".wav", L".wave", L".aif", L".aiff", L".aifc", L".aac", L".mp3", L".dsf", L".dff"})
        if (ext == e) return "audio";
    return {};
}

struct Metadata {
    std::string format;
    audio::SourceInfo info;
    TagLib::PropertyMap tags;
    bool cover = false;
};

Metadata metadata(const Path& path) {
    const auto format = audio::probeFormat(path);
    // Decoder opening enforces the same codec/profile/depth scope as playback without decoding audio.
    const auto decoder = audio::openDecoder(audio::defaultDecoder(format), path);
    Metadata out{audio::formatId(format), decoder->info(), {}, false};
    if (format == audio::AudioFormat::Dsd) {
        out.info.sampleRate *= 8;
        out.info.totalFrames *= 8;
    }
    char header[8]{};
    std::ifstream(path, std::ios::binary).read(header, sizeof(header));
    TagLib::FileStream stream(path.c_str(), true);
    std::unique_ptr<TagLib::File> file;
    switch (format) {
    case audio::AudioFormat::Flac:
        file = std::make_unique<TagLib::FLAC::File>(&stream);
        break;
    case audio::AudioFormat::Wav:
        file = std::make_unique<TagLib::RIFF::WAV::File>(&stream);
        break;
    case audio::AudioFormat::Aiff:
        file = std::make_unique<TagLib::RIFF::AIFF::File>(&stream);
        break;
    case audio::AudioFormat::Alac:
        file = std::make_unique<TagLib::MP4::File>(&stream);
        break;
    case audio::AudioFormat::Mp3:
        file = std::make_unique<TagLib::MPEG::File>(&stream);
        break;
    case audio::AudioFormat::Aac:
        if (memcmp(header + 4, "ftyp", 4) == 0) file = std::make_unique<TagLib::MP4::File>(&stream);
        break;
    case audio::AudioFormat::Dsd:
        if (memcmp(header, "DSD ", 4) == 0)
            file = std::make_unique<TagLib::DSF::File>(&stream);
        else
            file = std::make_unique<TagLib::DSDIFF::File>(&stream);
        break;
    }
    if (file) {
        if (!file->isValid()) throw std::runtime_error("TagLib could not read metadata");
        out.tags = file->properties();
        out.cover = file->complexPropertyKeys().contains("PICTURE");
    }
    return out;
}

std::string tag(const Metadata& m, const char* name) {
    const auto values = m.tags.value(name);
    return values.isEmpty() ? std::string{} : values.front().to8Bit(true);
}

int number(const Metadata& m, const char* name, bool total = false) {
    auto text = tag(m, name);
    if (total) {
        const auto slash = text.find('/');
        if (slash == std::string::npos) return 0;
        text.erase(0, slash + 1);
    }
    int value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} ? std::max(0, value) : 0;
}

void writeMetadata(sqlite::Database& db, int64_t file, const Metadata& m) {
    auto s = db.prepare("INSERT INTO tracks(file_id,title,album,album_artist,artist,composer,genre,year,track_number,track_total,"
                        "disc_number,disc_total,format,sample_rate,bits_per_sample,channels,total_frames,embedded_cover,lyrics) "
                        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(file_id,cue_index) DO UPDATE SET "
                        "title=excluded.title,album=excluded.album,album_artist=excluded.album_artist,artist=excluded.artist,"
                        "composer=excluded.composer,genre=excluded.genre,year=excluded.year,track_number=excluded.track_number,"
                        "track_total=excluded.track_total,disc_number=excluded.disc_number,disc_total=excluded.disc_total,"
                        "format=excluded.format,sample_rate=excluded.sample_rate,bits_per_sample=excluded.bits_per_sample,"
                        "channels=excluded.channels,total_frames=excluded.total_frames,embedded_cover=excluded.embedded_cover,"
                        "lyrics=excluded.lyrics");
    s.bind(1, file);
    int i = 2;
    for (const auto* field : {"TITLE", "ALBUM", "ALBUMARTIST", "ARTIST", "COMPOSER", "GENRE"}) s.bind(i++, tag(m, field));
    s.bind(8, int64_t(number(m, "DATE")))
        .bind(9, int64_t(number(m, "TRACKNUMBER")))
        .bind(10, int64_t(number(m, "TRACKTOTAL") ? number(m, "TRACKTOTAL") : number(m, "TRACKNUMBER", true)))
        .bind(11, int64_t(number(m, "DISCNUMBER")))
        .bind(12, int64_t(number(m, "DISCTOTAL") ? number(m, "DISCTOTAL") : number(m, "DISCNUMBER", true)))
        .bind(13, m.format)
        .bind(14, int64_t(m.info.sampleRate))
        .bind(15, int64_t(m.info.bitsPerSample))
        .bind(16, int64_t(m.info.channels))
        .bind(17, int64_t(m.info.totalFrames))
        .bind(18, int64_t(m.cover))
        .bind(19, tag(m, "LYRICS").empty() ? "none" : "embedded")
        .run();
    auto row = db.prepare("SELECT id FROM tracks WHERE file_id=? AND cue_index=0");
    row.bind(1, file).step();
    const int64_t track = row.integer(0);
    db.prepare("DELETE FROM track_values WHERE track_id=?").bind(1, track).run();
    for (const auto* field : {"ARTIST", "COMPOSER", "GENRE"}) {
        int64_t position = 0;
        for (const auto& value : m.tags.value(field))
            db.prepare("INSERT INTO track_values(track_id,field,position,value) VALUES(?,?,?,?)")
                .bind(1, track)
                .bind(2, toUtf8(lower(toWide(field))))
                .bind(3, position++)
                .bind(4, value.to8Bit(true))
                .run();
    }
    i = 0;
    for (const auto* field :
         {"REPLAYGAIN_TRACK_GAIN", "REPLAYGAIN_TRACK_PEAK", "REPLAYGAIN_ALBUM_GAIN", "REPLAYGAIN_ALBUM_PEAK"}) {
        const char* columns[] = {"rg_track_gain", "rg_track_peak", "rg_album_gain", "rg_album_peak"};
        auto update = db.prepare(std::string("UPDATE tracks SET ") + columns[i++] + "=? WHERE id=?");
        const auto value = tag(m, field);
        char* end = nullptr;
        const double parsed = std::strtod(value.c_str(), &end);
        if (end != value.c_str() && std::isfinite(parsed))
            update.bind(1, parsed);
        else
            update.bindNull(1);
        update.bind(2, track).run();
    }
}

struct Entry {
    Path path;
    std::string kind;
    Version version;
    std::optional<Metadata> metadata;
    std::string error;
    bool rejected = false;
};

struct Listing {
    // shortcut: metadata is buffered per directory; stream batches if large flat folders miss scan latency targets.
    Path path;
    std::vector<Path> children;
    std::vector<Entry> files;
    bool complete = true;
    std::string error;
};

Listing list(const Path& path, const std::map<std::string, Version>& known, const CancellationToken& token) {
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    Listing out{path};
    try {
        if (canonicalRoot(path) != Path(lower(path.wstring())))
            throw std::runtime_error("directory now resolves through an alias: " + key(path));
    } catch (const std::exception& e) {
        out.complete = false;
        out.error = e.what();
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
        return out;
    }
    WIN32_FIND_DATAW data{};
    wil::unique_hfind find(FindFirstFileExW((path / L"*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr,
                                            FIND_FIRST_EX_LARGE_FETCH));
    if (!find) {
        // An empty, still-readable directory is a complete listing.
        const DWORD error = GetLastError();
        out.complete = error == ERROR_FILE_NOT_FOUND && bool(openPath(path));
        if (!out.complete) out.error = key(path) + ": directory listing failed (" + std::to_string(error) + ")";
    } else {
        do {
            if (token.cancelled()) {
                out.complete = false;
                break;
            }
            if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
            // Never traverse reparse points: this also prevents cycles and escapes from a registered root.
            if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            const Path child = path / data.cFileName;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                out.children.push_back(child);
                continue;
            }
            const auto type = kind(child);
            if (type.empty()) continue;
            Entry entry{child, type};
            try {
                entry.version = version(child);
                const auto old = known.find(key(child));
                if (old == known.end() || old->second != entry.version) {
                    if (type == "audio") entry.metadata = metadata(child);
                    if (version(child) != entry.version) throw std::runtime_error("file changed during metadata read");
                }
            } catch (const audio::Error& e) {
                entry.error = e.what();
                entry.rejected = e.code() == audio::ErrorCode::UnsupportedStream;
                if (!entry.rejected) out.complete = false;
            } catch (const std::exception& e) {
                entry.error = e.what();
                out.complete = false;
            }
            out.files.push_back(std::move(entry));
        } while (FindNextFileW(find.get(), &data));
        if (!token.cancelled() && GetLastError() != ERROR_NO_MORE_FILES) {
            out.complete = false;
            out.error = key(path) + ": directory enumeration interrupted";
        }
    }
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    return out;
}

} // namespace

int64_t LibraryScanner::addRoot(const Path& path) {
    std::lock_guard lock(store_.scanMutex_);
    const Path resolved = canonicalRoot(path);
    const DWORD attr = GetFileAttributesW(resolved.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
        throw std::invalid_argument("root must be a directory");
    int64_t id = 0;
    store_
        .write([&](sqlite::Database& db) {
            auto roots = db.prepare("SELECT id,path FROM roots WHERE removed IS NULL");
            std::vector<int64_t> children;
            while (roots.step()) {
                const Path existing(toWide(roots.text(1)));
                if (contains(existing, resolved)) {
                    id = roots.integer(0);
                    return;
                }
                if (contains(resolved, existing)) children.push_back(roots.integer(0));
            }
            db.prepare("INSERT INTO roots(path,added) VALUES(?,?) ON CONFLICT(path) DO UPDATE SET removed=NULL")
                .bind(1, key(resolved))
                .bind(2, now())
                .run();
            auto root = db.prepare("SELECT id FROM roots WHERE path=?");
            root.bind(1, key(resolved)).step();
            id = root.integer(0);
            for (const auto child : children) {
                for (const auto* table : {"folders", "files"})
                    db.prepare(std::string("UPDATE ") + table + " SET root_id=? WHERE root_id=?")
                        .bind(1, id)
                        .bind(2, child)
                        .run();
                db.prepare("UPDATE roots SET removed=? WHERE id=?").bind(1, now()).bind(2, child).run();
            }
        })
        .get();
    return id;
}

ScanReport LibraryScanner::scan(int64_t root, const CancellationToken& token) {
    std::lock_guard lock(store_.scanMutex_);
    ScanReport report;
    auto reader = store_.openReader();
    auto row = reader.prepare("SELECT path FROM roots WHERE id=? AND removed IS NULL");
    if (!row.bind(1, root).step()) throw std::invalid_argument("unknown or removed library root");
    const Path path(toWide(row.text(0)));
    if (token.cancelled()) return report;
    if (!openPath(path, FILE_LIST_DIRECTORY)) {
        report.rootError = GetLastError();
        report.online = false;
        report.diagnostics.push_back("root unavailable (" + std::to_string(report.rootError) + "): " + key(path));
        store_.write([root](sqlite::Database& db) { db.prepare("UPDATE roots SET online=0 WHERE id=?").bind(1, root).run(); })
            .get();
        return report;
    }
    std::map<std::string, Version> known;
    auto files = reader.prepare("SELECT path,size,mtime,volume_serial,file_index FROM files WHERE root_id=? AND available=1");
    files.bind(1, root);
    while (files.step())
        known.emplace(files.text(0), Version{files.integer(1), files.integer(2), files.integer(3), files.blob(4)});
    const int64_t generation = reader.scalar("SELECT COALESCE(MAX(last_seen),0)+1 FROM files").value();
    std::deque<Path> pending{path};
    std::deque<std::future<Listing>> active;
    bool complete = true;
    std::vector<Entry> batch;
    auto flush = [&] {
        if (batch.empty()) return;
        store_
            .write([&](sqlite::Database& db) {
                for (const auto& entry : batch) {
                    const auto p = key(entry.path);
                    auto old = db.prepare("SELECT id,volume_serial,file_index FROM files WHERE path=? AND available=1");
                    old.bind(1, p);
                    int64_t file = old.step() ? old.integer(0) : 0;
                    if (!entry.error.empty() && !entry.rejected) {
                        if (file) db.prepare("UPDATE files SET last_seen=? WHERE id=?").bind(1, generation).bind(2, file).run();
                        continue;
                    }
                    if (file && (old.integer(1) != entry.version.volume || old.blob(2) != entry.version.index)) {
                        db.prepare("UPDATE files SET available=0 WHERE id=?").bind(1, file).run();
                        file = 0;
                    }
                    const auto folder = key(entry.path.parent_path());
                    auto f = db.prepare("SELECT id FROM folders WHERE path=?");
                    f.bind(1, folder).step();
                    const int64_t folderId = f.integer(0);
                    if (!file) {
                        db.prepare("INSERT INTO files(folder_id,root_id,path,kind,size,mtime,volume_serial,file_index,last_seen) "
                                   "VALUES(?,?,?,?,?,?,?,?,?)")
                            .bind(1, folderId)
                            .bind(2, root)
                            .bind(3, p)
                            .bind(4, entry.kind)
                            .bind(5, entry.version.size)
                            .bind(6, entry.version.mtime)
                            .bind(7, entry.version.volume)
                            .bind(8, entry.version.index)
                            .bind(9, generation)
                            .run();
                        file = db.lastInsertId();
                    } else {
                        db.prepare("UPDATE files SET size=?,mtime=?,last_seen=?,folder_id=?,root_id=? WHERE id=?")
                            .bind(1, entry.version.size)
                            .bind(2, entry.version.mtime)
                            .bind(3, generation)
                            .bind(4, folderId)
                            .bind(5, root)
                            .bind(6, file)
                            .run();
                    }
                    if (entry.rejected) db.prepare("DELETE FROM tracks WHERE file_id=? AND cue_index=0").bind(1, file).run();
                    if (entry.metadata) {
                        writeMetadata(db, file, *entry.metadata);
                        report.updated++;
                    }
                }
            })
            .get();
        batch.clear();
    };
    while ((!pending.empty() || !active.empty()) && !token.cancelled()) {
        while (!pending.empty() && active.size() < 4) {
            const auto dir = pending.front();
            pending.pop_front();
            active.push_back(std::async(std::launch::async, [&, dir] { return list(dir, known, token); }));
        }
        const auto ready = std::find_if(active.begin(), active.end(), [](auto& job) {
            return job.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
        });
        if (ready == active.end()) {
            active.front().wait_for(std::chrono::milliseconds(10));
            continue;
        }
        auto listing = ready->get();
        active.erase(ready);
        complete = complete && listing.complete;
        if (!listing.error.empty()) report.diagnostics.push_back(listing.error);
        store_
            .write([&](sqlite::Database& db) {
                db.prepare("INSERT INTO folders(root_id,path,parent_id) VALUES(?,?,(SELECT id FROM folders WHERE path=?)) "
                           "ON CONFLICT(path) DO UPDATE SET root_id=excluded.root_id,parent_id=excluded.parent_id")
                    .bind(1, root)
                    .bind(2, key(listing.path))
                    .bind(3, listing.path == path ? std::string{} : key(listing.path.parent_path()))
                    .run();
            })
            .get();
        for (auto& child : listing.children) pending.push_back(std::move(child));
        for (auto& entry : listing.files) {
            report.seen++;
            if (!entry.error.empty()) report.diagnostics.push_back(key(entry.path) + ": " + entry.error);
            batch.push_back(std::move(entry));
            if (batch.size() == 500) flush();
        }
    }
    for (auto& job : active) job.get();
    flush();
    report.online = bool(openPath(path, FILE_LIST_DIRECTORY));
    if (!report.online) {
        report.rootError = GetLastError();
        report.diagnostics.push_back("root became unavailable (" + std::to_string(report.rootError) + "): " + key(path));
    }
    report.complete = complete && !token.cancelled() && report.online;
    store_
        .write([&](sqlite::Database& db) {
            db.prepare("UPDATE roots SET online=?,last_scan=CASE WHEN ? THEN ? ELSE last_scan END WHERE id=?")
                .bind(1, int64_t(report.online))
                .bind(2, int64_t(report.complete))
                .bind(3, now())
                .bind(4, root)
                .run();
            if (report.complete)
                db.prepare("UPDATE files SET available=0 WHERE root_id=? AND available=1 AND (last_seen IS NULL OR last_seen<>?)")
                    .bind(1, root)
                    .bind(2, generation)
                    .run();
        })
        .get();
    return report;
}

class LibraryMonitor::Impl {
public:
    Impl(LibraryStore& store, Options options) : store_(store), scanner_(store), options_(options) {
        if (options.offlineInterval.count() <= 0 || options.coalesce.count() < 0 || options.pollInterval.count() < 0)
            throw std::invalid_argument("invalid library monitor interval");
        worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
    }
    ~Impl() {
        worker_.request_stop();
        token_.cancel();
        worker_.join();
    }

private:
    struct Watch {
        wil::unique_handle directory, event;
        OVERLAPPED io{};
        std::array<uint8_t, 32768> buffer{};
        Clock::time_point due{}, lastPoll{};
        bool pending = false;
        ~Watch() {
            if (pending) {
                CancelIoEx(directory.get(), &io);
                DWORD bytes = 0;
                GetOverlappedResult(directory.get(), &io, &bytes, TRUE);
            }
        }
        bool arm() {
            ResetEvent(event.get());
            io = {};
            io.hEvent = event.get();
            pending = bool(ReadDirectoryChangesW(directory.get(), buffer.data(), DWORD(buffer.size()), TRUE,
                                                 FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                                     FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
                                                     FILE_NOTIFY_CHANGE_LAST_WRITE,
                                                 nullptr, &io, nullptr));
            return pending;
        }
    };

    void run(std::stop_token stop) {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        std::map<int64_t, std::unique_ptr<Watch>> watches;
        auto check = Clock::time_point{};
        while (!stop.stop_requested()) {
            try {
                const auto time = Clock::now();
                if (time >= check) {
                    check = time + options_.offlineInterval;
                    std::set<int64_t> live;
                    auto db = store_.openReader();
                    auto roots = db.prepare("SELECT id,path,online FROM roots WHERE removed IS NULL");
                    while (roots.step() && !stop.stop_requested()) {
                        const int64_t id = roots.integer(0);
                        live.insert(id);
                        const Path path(toWide(roots.text(1)));
                        const bool online = bool(openPath(path, FILE_LIST_DIRECTORY));
                        if (!online) {
                            watches.erase(id);
                            store_
                                .write([id](sqlite::Database& w) {
                                    w.prepare("UPDATE roots SET online=0 WHERE id=?").bind(1, id).run();
                                })
                                .get();
                            continue;
                        }
                        if (!watches.contains(id) || !watches.at(id)->pending) {
                            watches.erase(id);
                            auto watch = std::make_unique<Watch>();
                            watch->directory =
                                openPath(path, FILE_LIST_DIRECTORY, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED);
                            watch->event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
                            if (watch->directory && watch->event) watch->arm();
                            watch->due = time;
                            watch->lastPoll = time;
                            watches.emplace(id, std::move(watch));
                        }
                        auto& watch = *watches.at(id);
                        const auto interval = options_.pollInterval.count() || !isNetworkPath(path) ? options_.pollInterval
                                                                                                    : std::chrono::minutes(30);
                        if (interval.count() && time - watch.lastPoll >= interval) {
                            watch.due = time;
                            watch.lastPoll = time;
                        }
                    }
                    std::erase_if(watches, [&](const auto& pair) { return !live.contains(pair.first); });
                }
                for (auto& [id, ptr] : watches) {
                    auto& watch = *ptr;
                    if (watch.pending && WaitForSingleObject(watch.event.get(), 0) == WAIT_OBJECT_0) {
                        DWORD bytes = 0;
                        const bool ok = bool(GetOverlappedResult(watch.directory.get(), &watch.io, &bytes, FALSE));
                        watch.pending = false;
                        // Empty/overflowed buffers and failed notifications both require a full incremental scan.
                        if (watch.due == Clock::time_point{} || (ok && bytes)) watch.due = Clock::now() + options_.coalesce;
                        watch.arm();
                    }
                    if (watch.due != Clock::time_point{} && Clock::now() >= watch.due && !stop.stop_requested()) {
                        watch.due = {};
                        const auto report = scanner_.scan(id, token_);
                        for (const auto& diagnostic : report.diagnostics) log::warn("library: %s", diagnostic.c_str());
                    }
                }
            } catch (const std::exception& e) {
                log::warn("library monitor: %s", e.what());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    }

    LibraryStore& store_;
    LibraryScanner scanner_;
    Options options_;
    CancellationToken token_;
    std::jthread worker_;
};

LibraryMonitor::LibraryMonitor(LibraryStore& store) : LibraryMonitor(store, Options{}) {}
LibraryMonitor::LibraryMonitor(LibraryStore& store, Options options) : impl_(std::make_unique<Impl>(store, options)) {}
LibraryMonitor::~LibraryMonitor() = default;

} // namespace camel::server
