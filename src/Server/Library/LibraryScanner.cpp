// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Library/LibraryScanner.h"

#include <windows.h>
#include <wil/resource.h>

#include <algorithm>
#include <charconv>
#include <cwctype>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <future>
#include <fstream>
#include <map>
#include <set>
#include <stop_token>
#include <thread>
#include <tuple>

#include <taglib/aifffile.h>
#include <taglib/dsdifffile.h>
#include <taglib/dsffile.h>
#include <taglib/flacfile.h>
#include <taglib/mp4file.h>
#include <taglib/mpegfile.h>
#include <taglib/tpropertymap.h>
#include <taglib/tfilestream.h>
#include <taglib/wavfile.h>

#include "Server/Library/LibraryIdentity.h"
#include "Server/Library/LibraryText.h"
#include "Server/Library/CueSheet.h"
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

using Version = library::FileVersion;
using library::fileVersion;
using TrustedIdentity = std::tuple<std::string, std::vector<uint8_t>, int64_t>;

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
    db.prepare("UPDATE tracks SET title_sort_tag=?,album_sort_tag=?,artist_sort_tag=? WHERE id=?")
        .bind(1, tag(m, "TITLESORT")).bind(2, tag(m, "ALBUMSORT")).bind(3, tag(m, "ARTISTSORT")).bind(4, track).run();
    db.prepare("DELETE FROM track_values WHERE track_id=?").bind(1, track).run();
    for (const auto* field : {"ARTIST", "COMPOSER", "GENRE"}) {
        int64_t position = 0;
        const auto sorts = m.tags.value((std::string(field) + "SORT").c_str());
        auto sort = sorts.begin();
        for (const auto& value : m.tags.value(field))
            db.prepare("INSERT INTO track_values(track_id,field,position,value,sort_source) VALUES(?,?,?,?,?)")
                .bind(1, track)
                .bind(2, toUtf8(lower(toWide(field))))
                .bind(3, position++)
                .bind(4, value.to8Bit(true))
                .bind(5, sort == sorts.end() ? std::string{} : (sort++)->to8Bit(true))
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
    updateLibraryTrackText(db, track);
}

struct Entry {
    Path path;
    std::string kind;
    Version version;
    std::optional<Metadata> metadata;
    std::string error;
    bool rejected = false;
    std::optional<std::vector<uint8_t>> digest;
};

struct Listing {
    // shortcut: metadata is buffered per directory; stream batches if large flat folders miss scan latency targets.
    Path path;
    std::vector<Path> children;
    std::vector<Entry> files;
    bool complete = true;
    bool deferred = false;
    std::string error;
};

struct CueProjection {
    int64_t file = 0;
    int64_t cueFile = 0;
    int index = 0;
    int64_t start = 0;
    int64_t end = 0;
    int64_t pregap75 = 0;
    int64_t postgap75 = 0;
    int64_t year = 0;
    int64_t trackTotal = 0;
    std::string title;
    std::string album;
    std::string albumArtist;
    std::string artist;
};

struct CuePlan {
    std::set<int64_t> validCueFiles;
    std::set<int64_t> replaceEmbeddedFiles;
    std::set<std::pair<int64_t, int>> tracks;
    std::vector<CueProjection> projections;
};

std::optional<uint32_t> cueEncoding(sqlite::Database& db, int64_t fileId) {
    auto row = db.prepare("SELECT value FROM app_state WHERE key=?");
    row.bind(1, "cue_encoding:" + std::to_string(fileId));
    if (!row.step()) return std::nullopt;
    uint32_t value = 0;
    const auto text = row.text(0);
    if (std::from_chars(text.data(), text.data() + text.size(), value).ec != std::errc{} || !value) return std::nullopt;
    return value;
}

uint64_t cueSamples(uint64_t frames75, uint64_t sampleRate) {
    if (!sampleRate || frames75 > UINT64_MAX / sampleRate || (frames75 * sampleRate) % 75)
        throw std::runtime_error("CUE offset cannot be represented at this sample rate");
    return frames75 * sampleRate / 75;
}

CuePlan makeCuePlan(sqlite::Database& db, int64_t root, const Path& rootPath, const std::vector<std::string>& changedAudio,
                    std::vector<std::string>& diagnostics) {
    struct Audio {
        int64_t id;
        std::string path;
        uint64_t rate;
        uint64_t frames;
        std::string title, album, albumArtist, artist;
        int year, trackTotal;
    };
    struct Source {
        int64_t id;
        std::string path;
        library::CueSheet sheet;
        bool valid = false;
        std::vector<CueProjection> tracks;
    };
    CuePlan plan;
    std::map<std::string, Audio> audioByPath;
    std::map<int64_t, Audio> audioById;
    auto audioRows =
        db.prepare("SELECT f.id,f.path,t.sample_rate,t.total_frames,t.title,t.album,t.album_artist,t.artist,t.year,t.track_total "
                   "FROM files f JOIN tracks t ON t.file_id=f.id AND t.cue_index=0 "
                   "WHERE f.root_id=? AND f.available=1 AND f.kind='audio'");
    audioRows.bind(1, root);
    while (audioRows.step()) {
        Audio a{audioRows.integer(0),
                audioRows.text(1),
                uint64_t(audioRows.integer(2)),
                uint64_t(audioRows.integer(3)),
                audioRows.text(4),
                audioRows.text(5),
                audioRows.text(6),
                audioRows.text(7),
                int(audioRows.integer(8)),
                int(audioRows.integer(9))};
        audioByPath.emplace(a.path, a);
        audioById.emplace(a.id, std::move(a));
    }
    std::vector<Source> sources;
    auto cueRows = db.prepare("SELECT id,path FROM files WHERE root_id=? AND available=1 AND kind='cue'");
    cueRows.bind(1, root);
    while (cueRows.step()) {
        Source source{cueRows.integer(0), cueRows.text(1)};
        try {
            const Path cuePath(toWide(source.path));
            const auto before = fileVersion(cuePath);
            source.sheet = library::parseCue(cuePath, cueEncoding(db, source.id));
            if (fileVersion(cuePath) != before) throw std::runtime_error("CUE changed during read");
            std::set<std::string> usedPaths;
            for (const auto& cueFile : source.sheet.files) {
                const Path relative(toWide(cueFile.path));
                if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory())
                    throw std::runtime_error("CUE FILE path must be relative");
                const Path resolved = (cuePath.parent_path() / relative).lexically_normal();
                if (canonicalRoot(resolved) != Path(lower(resolved.wstring())) || !contains(rootPath, resolved))
                    throw std::runtime_error("CUE FILE resolves outside the registered root or through an alias");
                const std::string audioPath = key(resolved);
                if (!usedPaths.insert(audioPath).second) throw std::runtime_error("CUE repeats a FILE path");
                const auto found = audioByPath.find(audioPath);
                if (found == audioByPath.end()) throw std::runtime_error("CUE FILE is not an available audio file in this root");
                const auto& audio = found->second;
                if (!audio.rate || !audio.frames) throw std::runtime_error("CUE source has no known sample range");
                std::vector<uint64_t> starts;
                for (const auto& track : cueFile.tracks)
                    starts.push_back(source.sheet.offsetsAreSamples ? *track.index01 : cueSamples(*track.index01, audio.rate));
                size_t totalTracks = 0;
                for (const auto& file : source.sheet.files) totalTracks += file.tracks.size();
                if (starts.front() > 0 && &cueFile == &source.sheet.files.front()) {
                    source.tracks.push_back({audio.id, source.id, -1, 0, int64_t(starts.front()), 0, 0, source.sheet.year,
                                             int64_t(source.sheet.files.front().tracks.size()), "", source.sheet.title,
                                             source.sheet.performer.empty() ? audio.albumArtist : source.sheet.performer,
                                             source.sheet.performer.empty() ? audio.artist : source.sheet.performer});
                }
                for (size_t i = 0; i < cueFile.tracks.size(); ++i) {
                    const auto& track = cueFile.tracks[i];
                    const uint64_t end = i + 1 < starts.size() ? starts[i + 1] : audio.frames;
                    if (starts[i] >= end || end > audio.frames)
                        throw std::runtime_error("CUE track range is empty, overlapping or beyond the source");
                    if (i + 1 < cueFile.tracks.size() && cueFile.tracks[i + 1].index00) {
                        const auto pregap = source.sheet.offsetsAreSamples
                                                ? *cueFile.tracks[i + 1].index00
                                                : cueSamples(*cueFile.tracks[i + 1].index00, audio.rate);
                        if (pregap < starts[i]) throw std::runtime_error("CUE INDEX 00 overlaps the preceding track");
                    }
                    source.tracks.push_back(
                        {audio.id, source.id, track.number, int64_t(starts[i]), int64_t(end), int64_t(track.pregap75),
                         int64_t(track.postgap75), source.sheet.year ? source.sheet.year : audio.year, int64_t(totalTracks),
                         track.title.empty() ? "Track " + std::to_string(track.number) : track.title,
                         source.sheet.title.empty() ? audio.album : source.sheet.title,
                         source.sheet.performer.empty() ? audio.albumArtist : source.sheet.performer,
                         track.performer.empty() ? (source.sheet.performer.empty() ? audio.artist : source.sheet.performer)
                                                 : track.performer});
                }
            }
            source.valid = true;
        } catch (const std::exception& e) {
            diagnostics.push_back(source.path + ": " + e.what());
        }
        sources.push_back(std::move(source));
    }

    std::map<int64_t, std::set<int64_t>> sourcesByAudio;
    for (const auto& source : sources)
        if (source.valid)
            for (const auto& track : source.tracks)
                if (track.index > 0) sourcesByAudio[track.file].insert(source.id);
    std::set<int64_t> ambiguous;
    for (const auto& [audioId, sourceIds] : sourcesByAudio) {
        if (sourceIds.size() > 1) {
            ambiguous.insert(sourceIds.begin(), sourceIds.end());
            diagnostics.push_back("ambiguous external CUE references for audio file " + std::to_string(audioId));
        }
    }
    for (auto& source : sources) {
        if (!source.valid || ambiguous.contains(source.id)) continue;
        plan.validCueFiles.insert(source.id);
        for (const auto& track : source.tracks) {
            plan.replaceEmbeddedFiles.insert(track.file);
            plan.tracks.emplace(track.file, track.index);
            plan.projections.push_back(track);
        }
    }

    std::set<std::string> changedPaths(changedAudio.begin(), changedAudio.end());
    std::set<int64_t> embeddedCandidates;
    for (const auto& [id, audio] : audioById)
        if (changedPaths.contains(audio.path)) embeddedCandidates.insert(id);
    auto oldCueTracks = db.prepare("SELECT DISTINCT t.file_id,f.path,cf.id,cf.available FROM tracks t "
                                   "JOIN files f ON f.id=t.file_id LEFT JOIN files cf ON cf.id=t.cue_file_id "
                                   "WHERE f.root_id=? AND f.available=1 AND f.kind='audio' AND t.cue_index<>0 "
                                   "AND t.cue_file_id IS NOT NULL");
    oldCueTracks.bind(1, root);
    while (oldCueTracks.step()) {
        const int64_t file = oldCueTracks.integer(0);
        const int64_t cue = oldCueTracks.integer(2);
        if (!oldCueTracks.integer(3) || !plan.validCueFiles.contains(cue)) embeddedCandidates.insert(file);
    }
    for (const auto id : embeddedCandidates) {
        const auto& audio = audioById.at(id);
        if (plan.replaceEmbeddedFiles.contains(id)) continue;
        plan.replaceEmbeddedFiles.insert(id);
        try {
            auto embedded = library::parseEmbeddedFlacCue(Path(toWide(audio.path)));
            if (embedded.files.empty()) continue;
            std::vector<uint64_t> starts;
            for (const auto& track : embedded.files.front().tracks) starts.push_back(*track.index01);
            if (!starts.empty() && starts.front() > 0) {
                CueProjection leadIn{audio.id,
                                     0,
                                     -1,
                                     0,
                                     int64_t(starts.front()),
                                     0,
                                     0,
                                     audio.year,
                                     int64_t(starts.size()),
                                     "",
                                     audio.album,
                                     audio.albumArtist,
                                     audio.artist};
                plan.tracks.emplace(leadIn.file, leadIn.index);
                plan.projections.push_back(std::move(leadIn));
            }
            for (size_t i = 0; i < starts.size(); ++i) {
                const uint64_t end = i + 1 < starts.size() ? starts[i + 1] : audio.frames;
                if (starts[i] >= end || end > audio.frames) throw std::runtime_error("embedded CUESHEET range is invalid");
                CueProjection p{audio.id,
                                0,
                                embedded.files.front().tracks[i].number,
                                int64_t(starts[i]),
                                int64_t(end),
                                0,
                                0,
                                audio.year,
                                int64_t(starts.size()),
                                "Track " + std::to_string(embedded.files.front().tracks[i].number),
                                audio.album,
                                audio.albumArtist,
                                audio.artist};
                plan.tracks.emplace(p.file, p.index);
                plan.projections.push_back(std::move(p));
            }
            plan.replaceEmbeddedFiles.insert(id);
        } catch (const std::exception& e) {
            diagnostics.push_back(audio.path + ": embedded CUESHEET: " + e.what());
        }
    }
    return plan;
}

void applyCuePlan(sqlite::Database& db, const CuePlan& plan, int64_t root) {
    db.exec("DELETE FROM tracks WHERE cue_index<>0 AND cue_file_id IN "
            "(SELECT id FROM files WHERE root_id=" +
            std::to_string(root) + " AND kind='cue' AND available=0)");
    auto oldCues = db.prepare("SELECT id FROM files WHERE root_id=? AND kind='cue' AND available=1");
    oldCues.bind(1, root);
    while (oldCues.step()) {
        const auto id = oldCues.integer(0);
        if (!plan.validCueFiles.contains(id)) db.prepare("DELETE FROM tracks WHERE cue_file_id=?").bind(1, id).run();
    }
    for (const auto file : plan.replaceEmbeddedFiles) {
        auto existing = db.prepare("SELECT cue_index FROM tracks WHERE file_id=? AND cue_file_id IS NULL AND cue_index<>0");
        existing.bind(1, file);
        while (existing.step()) {
            const auto index = int(existing.integer(0));
            if (!plan.tracks.contains({file, index}))
                db.prepare("DELETE FROM tracks WHERE file_id=? AND cue_index=? AND cue_file_id IS NULL")
                    .bind(1, file)
                    .bind(2, int64_t(index))
                    .run();
        }
    }
    auto oldVirtual =
        db.prepare("SELECT file_id,cue_index,cue_file_id FROM tracks WHERE cue_index<>0 AND cue_file_id IS NOT NULL");
    while (oldVirtual.step()) {
        const int64_t file = oldVirtual.integer(0);
        const int index = int(oldVirtual.integer(1));
        const int64_t cueFile = oldVirtual.integer(2);
        if (plan.validCueFiles.contains(cueFile) && !plan.tracks.contains({file, index}))
            db.prepare("DELETE FROM tracks WHERE file_id=? AND cue_index=?").bind(1, file).bind(2, int64_t(index)).run();
    }
    for (const auto& p : plan.projections) {
        if (p.index == 0) continue;
        auto s = db.prepare("INSERT INTO tracks(file_id,cue_index,cue_file_id,start_frame,end_frame,cue_pregap_75ths,"
                            "cue_postgap_75ths,title,album,album_artist,artist,year,track_number,track_total,disc_number,"
                            "disc_total,format,sample_rate,bits_per_sample,channels,total_frames,rg_track_gain,rg_track_peak,"
                            "rg_album_gain,rg_album_peak,rg_analysis_gain,rg_analysis_peak,embedded_cover,lyrics) "
                            "SELECT file_id,?,?,?,?,?,?,?,?,?,?,?,?,?,disc_number,disc_total,format,sample_rate,bits_per_sample,"
                            "channels,total_frames,rg_track_gain,rg_track_peak,rg_album_gain,rg_album_peak,rg_analysis_gain,"
                            "rg_analysis_peak,embedded_cover,lyrics FROM tracks WHERE file_id=? AND cue_index=0 "
                            "ON CONFLICT(file_id,cue_index) DO UPDATE SET cue_file_id=excluded.cue_file_id,"
                            "start_frame=excluded.start_frame,end_frame=excluded.end_frame,"
                            "cue_pregap_75ths=excluded.cue_pregap_75ths,cue_postgap_75ths=excluded.cue_postgap_75ths,"
                            "title=excluded.title,album=excluded.album,album_artist=excluded.album_artist,artist=excluded.artist,"
                            "year=excluded.year,track_number=excluded.track_number,track_total=excluded.track_total");
        s.bind(1, int64_t(p.index));
        if (p.cueFile)
            s.bind(2, p.cueFile);
        else
            s.bindNull(2);
        s.bind(3, p.start)
            .bind(4, p.end)
            .bind(5, p.pregap75)
            .bind(6, p.postgap75)
            .bind(7, p.title)
            .bind(8, p.album)
            .bind(9, p.albumArtist)
            .bind(10, p.artist)
            .bind(11, p.year)
            .bind(12, int64_t(p.index < 0 ? 0 : p.index))
            .bind(13, p.trackTotal)
            .bind(14, p.file)
            .run();
        auto row = db.prepare("SELECT id FROM tracks WHERE file_id=? AND cue_index=?");
        row.bind(1, p.file).bind(2, int64_t(p.index)).step();
        const auto track = row.integer(0);
        db.prepare("DELETE FROM track_values WHERE track_id=?").bind(1, track).run();
        db.prepare("INSERT INTO track_values(track_id,field,position,value,sort_source) SELECT ?,field,position,value,sort_source FROM track_values "
                   "WHERE track_id=(SELECT id FROM tracks WHERE file_id=? AND cue_index=0) AND field<>'artist'")
            .bind(1, track)
            .bind(2, p.file)
            .run();
        if (!p.artist.empty())
            db.prepare("INSERT INTO track_values(track_id,field,position,value) VALUES(?,'artist',0,?)")
                .bind(1, track)
                .bind(2, p.artist)
                .run();
        updateLibraryTrackText(db, track);
    }
}

void rebuildAlbums(sqlite::Database& db, int64_t root) {
    struct Album {
        int64_t folder = 0;
        int64_t firstFile = 0;
        bool firstHasCover = false;
        Path path;
        std::string title, artist;
        int64_t year = 0, discs = 0;
        std::set<std::string> artists;
        std::vector<int64_t> tracks;
    };
    std::map<std::string, Album> albums;
    auto rows = db.prepare("SELECT t.id,t.album,t.album_artist,t.artist,t.year,f.folder_id,COALESCE(d.parent_id,0),d.path,"
                           "t.file_id,COALESCE((SELECT embedded_cover FROM tracks e WHERE e.file_id=t.file_id AND e.cue_index=0),"
                           "t.embedded_cover),t.disc_number,t.disc_total "
                           "FROM tracks t JOIN files f ON f.id=t.file_id JOIN folders d ON d.id=f.folder_id "
                           "WHERE f.root_id=? AND f.available=1 ORDER BY t.disc_number,t.track_number,t.id");
    rows.bind(1, root);
    while (rows.step()) {
        const auto track = rows.integer(0);
        const auto discFolder = rows.integer(5);
        const auto parentFolder = rows.integer(6);
        Path folderPath(toWide(rows.text(7)));
        const bool discSubfolder = parentFolder && isDiscFolderName(folderPath.filename().wstring());
        const auto folder = discSubfolder ? parentFolder : discFolder;
        if (discSubfolder) {
            auto parent = db.prepare("SELECT path FROM folders WHERE id=?");
            if (parent.bind(1, parentFolder).step()) folderPath = Path(toWide(parent.text(0)));
        }
        auto title = albumTitleWithoutDiscSuffix(toWide(rows.text(1)));
        if (!rows.integer(10)) {
            const auto discNumber = [](std::wstring_view value) {
                const auto folded = lower(std::wstring(value));
                for (size_t i = 0; i < folded.size(); ++i) {
                    if (i && ((folded[i - 1] >= L'a' && folded[i - 1] <= L'z') ||
                              (folded[i - 1] >= L'0' && folded[i - 1] <= L'9')))
                        continue;
                    size_t marker = 0;
                    const std::wstring_view words[] = {L"cd", L"disc", L"disk"};
                    for (const auto word : words)
                        if (std::wstring_view(folded).substr(i, word.size()) == word) marker = word.size();
                    if (!marker && folded[i] == L'碟') marker = 1;
                    if (!marker) continue;
                    size_t n = i + marker;
                    while (n < folded.size() && std::iswspace(folded[n])) ++n;
                    int number = 0;
                    while (n < folded.size() && folded[n] >= L'0' && folded[n] <= L'9') number = number * 10 + (folded[n++] - L'0');
                    if (number) return number;
                }
                return 0;
            };
            const auto originalTitle = toWide(rows.text(1));
            const auto suffixNumber = title != originalTitle ? discNumber(std::wstring_view(originalTitle).substr(title.size())) : 0;
            const int inferred = suffixNumber ? suffixNumber : discNumber(Path(toWide(rows.text(7))).filename().wstring());
            if (inferred) db.prepare("UPDATE tracks SET disc_number=? WHERE id=?").bind(1, int64_t(inferred)).bind(2, track).run();
        }
        if (title.empty()) title = folderPath.filename().wstring();
        const auto titleText = toUtf8(title);
        const auto explicitArtist = rows.text(2);
        const auto normalized = toUtf8(lower(title)) + "\n" + toUtf8(lower(toWide(explicitArtist)));
        const auto keyText = std::to_string(folder) + "\n" + normalized;
        auto& album = albums[keyText];
        album.folder = folder;
        album.title = titleText;
        album.year = rows.integer(4);
        album.discs = std::max({album.discs, rows.integer(10), rows.integer(11)});
        if (!album.firstFile) {
            album.firstFile = rows.integer(8);
            album.firstHasCover = rows.integer(9) != 0;
            album.path = folderPath;
        }
        if (!explicitArtist.empty()) album.artist = explicitArtist;
        else if (!rows.text(3).empty()) album.artists.insert(rows.text(3));
        album.tracks.push_back(track);
    }
    for (auto& [keyText, album] : albums) {
        if (album.artist.empty()) album.artist = album.artists.size() == 1 ? *album.artists.begin() : "Various Artists";
        db.prepare("INSERT INTO albums(folder_id,grouping_key,title,album_artist,year,disc_count,added,title_norm,artist_norm) "
                   "VALUES(?,?,?,?,?,?,?,?,?) ON CONFLICT(grouping_key) DO UPDATE SET folder_id=excluded.folder_id,"
                   "title=excluded.title,album_artist=excluded.album_artist,year=excluded.year,disc_count=excluded.disc_count,title_norm=excluded.title_norm,"
                   "artist_norm=excluded.artist_norm")
            .bind(1, album.folder)
            .bind(2, keyText)
            .bind(3, album.title)
            .bind(4, album.artist)
            .bind(5, album.year)
            .bind(6, album.discs)
            .bind(7, now())
            .bind(8, toUtf8(lower(toWide(album.title))))
            .bind(9, toUtf8(lower(toWide(album.artist))))
            .run();
        auto row = db.prepare("SELECT id FROM albums WHERE grouping_key=?");
        row.bind(1, keyText).step();
        const auto albumId = row.integer(0);
        for (auto track : album.tracks) db.prepare("UPDATE tracks SET album_id=? WHERE id=?").bind(1, albumId).bind(2, track).run();
        updateLibraryAlbumText(db, albumId);

        ArtworkQuery query;
        query.albumFolder = album.path;
        query.albumArtist = album.artist;
        query.album = album.title;
        auto candidates = makeFolderArtworkProvider()->find(query, {});
        std::optional<ArtworkCandidate> image;
        if (!candidates.empty()) image = std::move(candidates.front());
        int64_t coverFile = 0;
        std::string source;
        std::string mime;
        if (image) {
            auto cover = db.prepare("SELECT id FROM files WHERE path=? AND kind='image' AND available=1");
            cover.bind(1, key(image->file));
            if (cover.step()) {
                coverFile = cover.integer(0);
                source = image->source;
                mime = image->mime;
            }
        }
        if (!coverFile && album.firstHasCover) {
            coverFile = album.firstFile;
            source = "embedded";
        }
        db.prepare("UPDATE albums SET cover_id=NULL WHERE id=?").bind(1, albumId).run();
        if (coverFile) {
            auto cover = db.prepare("SELECT id FROM covers WHERE source=? AND file_id=?");
            cover.bind(1, source).bind(2, coverFile);
            int64_t coverId = 0;
            if (cover.step()) coverId = cover.integer(0);
            else {
                db.prepare("INSERT INTO covers(source,file_id,mime) VALUES(?,?,?)")
                    .bind(1, source)
                    .bind(2, coverFile)
                    .bind(3, mime)
                    .run();
                coverId = db.lastInsertId();
            }
            db.prepare("UPDATE albums SET cover_id=? WHERE id=?").bind(1, coverId).bind(2, albumId).run();
        }
    }
    db.exec("DELETE FROM albums WHERE NOT EXISTS(SELECT 1 FROM tracks WHERE tracks.album_id=albums.id);"
            "DELETE FROM covers WHERE NOT EXISTS(SELECT 1 FROM albums WHERE albums.cover_id=covers.id)");
}

Listing list(const Path& path, const std::map<std::string, Version>& known, const std::set<int64_t>& hashSizes,
             const std::set<std::string>& pendingIdentity, const std::set<TrustedIdentity>& trusted,
             const CancellationToken& token, const std::function<bool()>& playbackActive) {
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
                entry.version = fileVersion(child);
                const auto old = known.find(key(child));
                if (old == known.end() || old->second != entry.version) {
                    if (type == "audio") entry.metadata = metadata(child);
                    if (fileVersion(child) != entry.version) throw std::runtime_error("file changed during metadata read");
                }
                if ((old == known.end() || !entry.version.sameFile(old->second) || pendingIdentity.contains(key(child))) &&
                    hashSizes.contains(entry.version.size) &&
                    !trusted.contains({entry.version.volumeKey, entry.version.index, entry.version.created})) {
                    entry.digest = library::fileDigest(child, entry.version, token, playbackActive);
                    if (!entry.digest) {
                        out.deferred = true;
                        throw std::runtime_error("fingerprint deferred while cancelled or playing");
                    }
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

Path canonicalLibraryPath(const Path& path) { return canonicalRoot(path); }

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

std::function<bool()> LibraryScanner::playbackGate(const Path& root) const {
    const auto storage = library::storageKey(root);
    return [this, storage] {
        if (playbackActive_ && playbackActive_()) return true;
        if (!playingSource_) return false;
        const auto source = playingSource_();
        if (source.empty()) return false;
        const auto playingStorage = library::storageKey(source);
        return storage.empty() || playingStorage.empty() || storage == playingStorage;
    };
}

void LibraryScanner::removeRoot(int64_t root) {
    std::lock_guard lock(store_.jobsMutex_);
    const auto [first, last] = store_.jobs_.equal_range(root);
    for (auto job = first; job != last; ++job) job->second.cancel();
    store_.write([root](sqlite::Database& db) {
        auto existing = db.prepare("SELECT 1 FROM roots WHERE id=?");
        if (!existing.bind(1, root).step()) throw std::invalid_argument("unknown library root");
        db.prepare("UPDATE roots SET removed=COALESCE(removed,?),online=0 WHERE id=?")
            .bind(1, now()).bind(2, root).run();
        db.prepare("UPDATE files SET available=0 WHERE root_id=?").bind(1, root).run();
    }).get();
}

ScanReport LibraryScanner::scan(int64_t root, const CancellationToken& request) {
    std::lock_guard lock(store_.scanMutex_);
    auto token = request.child();
    decltype(store_.jobs_)::iterator registration;
    {
        std::lock_guard jobsLock(store_.jobsMutex_);
        registration = store_.jobs_.emplace(root, token);
    }
    auto unregister = wil::scope_exit([&] {
        std::lock_guard jobsLock(store_.jobsMutex_);
        store_.jobs_.erase(registration);
    });
    ScanReport report;
    auto reader = store_.openReader();
    auto row = reader.prepare("SELECT path FROM roots WHERE id=? AND removed IS NULL");
    if (!row.bind(1, root).step()) throw std::invalid_argument("unknown or removed library root");
    const Path path(toWide(row.text(0)));
    const auto playbackActive = playbackGate(path);
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
    auto files = reader.prepare("SELECT path,size,mtime,volume_serial,file_index,created,volume_key,identity_state "
                                "FROM files WHERE root_id=? AND available=1");
    files.bind(1, root);
    std::set<std::string> pendingIdentity;
    while (files.step()) {
        known.emplace(files.text(0), library::storedVersion(files, 1));
        if (files.integer(7)) pendingIdentity.insert(files.text(0));
    }
    std::set<int64_t> hashSizes;
    auto fingerprints = reader.prepare("SELECT DISTINCT size FROM files WHERE length(content_hash)=32");
    while (fingerprints.step()) hashSizes.insert(fingerprints.integer(0));
    std::set<TrustedIdentity> trusted;
    auto identities = reader.prepare("SELECT volume_key,file_index,created FROM files WHERE volume_key<>'' AND identity_state=0");
    while (identities.step()) trusted.emplace(identities.text(0), identities.blob(1), identities.integer(2));
    const int64_t generation = reader.scalar("SELECT COALESCE(MAX(last_seen),0)+1 FROM files").value();
    std::deque<Path> pending{path};
    std::deque<std::future<Listing>> active;
    bool complete = true;
    std::vector<Entry> batch;
    std::vector<std::string> changedAudioPaths;
    auto flush = [&] {
        if (batch.empty()) return;
        store_
            .write([&](sqlite::Database& db) {
                if (token.cancelled()) return;
                for (const auto& entry : batch) {
                    const auto p = key(entry.path);
                    auto old = db.prepare("SELECT id,size,mtime,volume_serial,file_index,created,volume_key FROM files WHERE "
                                          "path=? AND available=1");
                    old.bind(1, p);
                    int64_t file = old.step() ? old.integer(0) : 0;
                    if (!entry.error.empty() && !entry.rejected) {
                        if (file) db.prepare("UPDATE files SET last_seen=? WHERE id=?").bind(1, generation).bind(2, file).run();
                        continue;
                    }
                    if (file && !entry.version.sameFile(library::storedVersion(old, 1))) {
                        db.prepare("UPDATE files SET available=0 WHERE id=?").bind(1, file).run();
                        file = 0;
                    }
                    const auto folder = key(entry.path.parent_path());
                    auto f = db.prepare("SELECT id FROM folders WHERE path=?");
                    f.bind(1, folder).step();
                    const int64_t folderId = f.integer(0);
                    if (!file) {
                        db.prepare("INSERT INTO "
                                   "files(folder_id,root_id,path,kind,size,mtime,volume_serial,file_index,last_seen,created,"
                                   "volume_key,identity_state) "
                                   "VALUES(?,?,?,?,?,?,?,?,?,?,?,1)")
                            .bind(1, folderId)
                            .bind(2, root)
                            .bind(3, p)
                            .bind(4, entry.kind)
                            .bind(5, entry.version.size)
                            .bind(6, entry.version.mtime)
                            .bind(7, entry.version.volume)
                            .bind(8, entry.version.index)
                            .bind(9, generation)
                            .bind(10, entry.version.created)
                            .bind(11, entry.version.volumeKey)
                            .run();
                        file = db.lastInsertId();
                    } else {
                        if (library::storedVersion(old, 1) != entry.version)
                            db.prepare("UPDATE files SET content_hash=NULL WHERE id=?").bind(1, file).run();
                        db.prepare("UPDATE files SET size=?,mtime=?,last_seen=?,folder_id=?,root_id=?,"
                                   "volume_serial=?,file_index=?,created=?,volume_key=? WHERE id=?")
                            .bind(1, entry.version.size)
                            .bind(2, entry.version.mtime)
                            .bind(3, generation)
                            .bind(4, folderId)
                            .bind(5, root)
                            .bind(6, entry.version.volume)
                            .bind(7, entry.version.index)
                            .bind(8, entry.version.created)
                            .bind(9, entry.version.volumeKey)
                            .bind(10, file)
                            .run();
                    }
                    if (entry.digest)
                        db.prepare("UPDATE files SET content_hash=? WHERE id=?").bind(1, *entry.digest).bind(2, file).run();
                    if (entry.rejected) db.prepare("DELETE FROM tracks WHERE file_id=? AND cue_index=0").bind(1, file).run();
                    if (entry.metadata) {
                        writeMetadata(db, file, *entry.metadata);
                        report.updated++;
                        changedAudioPaths.push_back(p);
                    }
                }
            })
            .get();
        batch.clear();
    };
    while ((!pending.empty() || !active.empty()) && !token.cancelled()) {
        while (!pending.empty() && active.size() < (playbackActive() ? 1u : 4u)) {
            const auto dir = pending.front();
            pending.pop_front();
            active.push_back(std::async(std::launch::async, [&, dir] {
                return list(dir, known, hashSizes, pendingIdentity, trusted, token, playbackActive);
            }));
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
        report.deferred = report.deferred || listing.deferred;
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
            if (token.cancelled()) return;
            db.prepare("UPDATE roots SET online=?,last_scan=CASE WHEN ? THEN ? ELSE last_scan END WHERE id=?")
                .bind(1, int64_t(report.online))
                .bind(2, int64_t(report.complete))
                .bind(3, now())
                .bind(4, root)
                .run();
            if (report.complete) {
                db.prepare("UPDATE files SET available=0 WHERE root_id=? AND available=1 AND (last_seen IS NULL OR last_seen<>?)")
                    .bind(1, root)
                    .bind(2, generation)
                    .run();
                std::vector<int64_t> added;
                auto pending = db.prepare("SELECT id FROM files WHERE root_id=? AND available=1 AND identity_state=1");
                pending.bind(1, root);
                while (pending.step()) added.push_back(pending.integer(0));
                report.moved = library::reconcileMoves(db, added);
                db.prepare("UPDATE files SET identity_state=0 WHERE root_id=? AND identity_state=1").bind(1, root).run();
            }
        })
        .get();
    if (report.complete) {
        auto cueReader = store_.openReader();
        CuePlan cuePlan = makeCuePlan(cueReader, root, path, changedAudioPaths, report.diagnostics);
        store_.write([&](sqlite::Database& db) {
            if (token.cancelled()) return;
            applyCuePlan(db, cuePlan, root);
            rebuildAlbums(db, root);
        }).get();
    }
    report.complete = report.complete && !token.cancelled();
    return report;
}

void LibraryScanner::setCueEncoding(const Path& cuePath, std::optional<uint32_t> codePage) {
    std::lock_guard lock(store_.scanMutex_);
    const auto resolved = canonicalRoot(cuePath);
    if (kind(resolved) != "cue") throw std::invalid_argument("encoding override requires a CUE file");
    if (codePage && (!*codePage || !IsValidCodePage(*codePage))) throw std::invalid_argument("invalid CUE code page");
    const auto cueKey = key(resolved);
    store_
        .write([&](sqlite::Database& db) {
            auto file = db.prepare("SELECT id FROM files WHERE path=? AND kind='cue' AND available=1");
            file.bind(1, cueKey);
            if (!file.step()) throw std::invalid_argument("CUE file is not indexed");
            const std::string stateKey = "cue_encoding:" + std::to_string(file.integer(0));
            if (codePage)
                db.prepare("INSERT INTO app_state(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value")
                    .bind(1, stateKey)
                    .bind(2, std::to_string(*codePage))
                    .run();
            else
                db.prepare("DELETE FROM app_state WHERE key=?").bind(1, stateKey).run();
        })
        .get();
}

FingerprintReport LibraryScanner::fingerprint(int64_t root, const CancellationToken& request) {
    auto token = request.child();
    decltype(store_.jobs_)::iterator registration;
    {
        std::lock_guard jobsLock(store_.jobsMutex_);
        registration = store_.jobs_.emplace(root, token);
    }
    auto unregister = wil::scope_exit([&] {
        std::lock_guard jobsLock(store_.jobsMutex_);
        store_.jobs_.erase(registration);
    });
    FingerprintReport report;
    auto db = store_.openReader();
    auto rootRow = db.prepare("SELECT path FROM roots WHERE id=? AND removed IS NULL");
    if (!rootRow.bind(1, root).step()) return report;
    const auto playbackActive = playbackGate(Path(toWide(rootRow.text(0))));
    if (token.cancelled() || playbackActive()) return report;
    const bool background = bool(SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN));
    auto restorePriority = wil::scope_exit([&] {
        if (background) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    });
    struct PendingHash {
        int64_t id;
        std::string path;
        Version version;
    };
    std::vector<PendingHash> pending;
    {
        auto reader = store_.openReader();
        auto rows = reader.prepare("SELECT files.id,files.path,size,mtime,volume_serial,file_index,created,volume_key "
                                   "FROM files JOIN roots ON roots.id=files.root_id WHERE root_id=? AND available=1 "
                                   "AND content_hash IS NULL AND identity_state=0 AND roots.online=1 AND roots.removed IS NULL");
        rows.bind(1, root);
        while (rows.step()) {
            if (token.cancelled()) return report;
            pending.push_back({rows.integer(0), rows.text(1), library::storedVersion(rows, 2)});
        }
    }
    for (const auto& [id, path, expected] : pending) {
        if (token.cancelled() || playbackActive()) return report;
        try {
            const auto digest = library::fileDigest(Path(toWide(path)), expected, token, playbackActive);
            if (!digest) return report;
            store_
                .write([&](sqlite::Database& db) {
                    if (token.cancelled() || playbackActive()) return;
                    auto current = db.prepare("SELECT path,size,mtime,volume_serial,file_index,created,volume_key FROM files "
                                              "WHERE id=? AND available=1 AND identity_state=0 AND content_hash IS NULL");
                    if (!current.bind(1, id).step() || current.text(0) != path ||
                        library::storedVersion(current, 1) != expected || library::fileVersion(Path(toWide(path))) != expected)
                        return;
                    db.prepare("UPDATE files SET content_hash=? WHERE id=?").bind(1, *digest).bind(2, id).run();
                    report.hashed++;
                })
                .get();
        } catch (const std::exception& e) {
            report.diagnostics.push_back(path + ": " + e.what());
        }
    }
    report.complete = !token.cancelled() && !playbackActive() && report.diagnostics.empty();
    return report;
}

class LibraryMonitor::Impl {
public:
    Impl(LibraryStore& store, Options options)
        : store_(store), scanner_(store, options.playbackActive, options.playingSource), options_(options) {
        if (options.offlineInterval.count() <= 0 || options.coalesce.count() < 0 || options.pollInterval.count() < 0)
            throw std::invalid_argument("invalid library monitor interval");
        worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
        fingerprints_ = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                try {
                    std::vector<int64_t> ids;
                    {
                        auto db = store_.openReader();
                        auto roots = db.prepare("SELECT id FROM roots WHERE removed IS NULL AND online=1");
                        while (roots.step()) ids.push_back(roots.integer(0));
                    }
                    for (const auto id : ids) {
                        if (stop.stop_requested()) break;
                        scanner_.fingerprint(id, token_);
                    }
                } catch (const std::exception& e) {
                    log::warn("library fingerprints: %s", e.what());
                }
                for (int i = 0; i < 20 && !stop.stop_requested(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });
    }
    ~Impl() {
        worker_.request_stop();
        fingerprints_.request_stop();
        token_.cancel();
        worker_.join();
        fingerprints_.join();
    }

private:
    struct Watch {
        wil::unique_handle directory, event;
        OVERLAPPED io{};
        std::array<uint8_t, 32768> buffer{};
        Clock::time_point due{}, lastPoll{};
        bool pending = false;
        bool deferred = false;
        std::function<bool()> playbackActive;
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
                            watch->playbackActive = scanner_.playbackGate(path);
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
                    if (watch.deferred && !watch.playbackActive()) {
                        watch.deferred = false;
                        watch.due = Clock::now();
                    }
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
                        watch.deferred = report.deferred;
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
    std::jthread fingerprints_;
};

LibraryMonitor::LibraryMonitor(LibraryStore& store) : LibraryMonitor(store, Options{}) {}
LibraryMonitor::LibraryMonitor(LibraryStore& store, Options options) : impl_(std::make_unique<Impl>(store, options)) {}
LibraryMonitor::~LibraryMonitor() = default;

} // namespace camel::server
