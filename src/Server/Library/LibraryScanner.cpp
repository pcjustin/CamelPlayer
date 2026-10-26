// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Library/LibraryScanner.h"
#include "Server/Library/LibraryViews.h"

#include <windows.h>
#include <wil/resource.h>

#include <algorithm>
#include <array>
#include <atomic>
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
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>

#include <taglib/aifffile.h>
#include <taglib/dsdifffile.h>
#include <taglib/dsffile.h>
#include <taglib/flacfile.h>
#include <taglib/mp4file.h>
#include <taglib/mpegfile.h>
#include <taglib/tpropertymap.h>
#include <taglib/tfilestream.h>
#include <taglib/wavfile.h>
#include <taglib/tbytevectorstream.h>

#include "Server/Library/LibraryIdentity.h"
#include "Server/Library/LibraryText.h"
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

// A file's creation time on disk (a FILETIME) in Unix seconds; now when it reports none or one in the future.
int64_t addedTime(int64_t created) {
    constexpr int64_t kUnixEpoch = 116'444'736'000'000'000;
    return created > kUnixEpoch ? std::min(now(), (created - kUnixEpoch) / 10'000'000) : now();
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

Path finalPath(HANDLE handle) {
    std::wstring text(32768, L'\0');
    const DWORD size = GetFinalPathNameByHandleW(handle, text.data(), DWORD(text.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!size || size >= text.size()) throw std::runtime_error("cannot resolve library path");
    text.resize(size);
    Path result(lower(text));
    while (result != result.root_path() && result.filename().empty()) result = result.parent_path();
    return result;
}

Path canonicalRoot(const Path& path) {
    auto handle = openPath(path);
    if (!handle) throw std::runtime_error("cannot access library path: " + toUtf8(path.wstring()));
    return finalPath(handle.get());
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
    bool lengthEstimated = false;
};

Metadata metadata(const Path& path, int64_t fileSize) {
    std::array<char, 64 * 1024> buffer;
    std::ifstream input;
    input.rdbuf()->pubsetbuf(buffer.data(), buffer.size());
    input.open(path, std::ios::binary);
    char signature[4]{};
    input.read(signature, sizeof(signature));
    if (memcmp(signature, "fLaC", 4) == 0) {
        // Skip artwork payloads and let TagLib parse only STREAMINFO and Vorbis comments in memory.
        const auto size = uint64_t(fileSize);
        uint64_t position = 4;
        TagLib::ByteVector blocks("fLaC", 4);
        unsigned int lastHeader = 4;
        bool last = false, cover = false;
        do {
            char header[4]{};
            input.read(header, sizeof(header));
            position += sizeof(header);
            const auto type = uint8_t(header[0]) & 0x7f;
            const uint32_t length = (uint32_t(uint8_t(header[1])) << 16) |
                                    (uint32_t(uint8_t(header[2])) << 8) | uint8_t(header[3]);
            if (!input || position > size || length > size - position ||
                (blocks.size() == 4 && (type != 0 || length != 34)))
                throw audio::Error(audio::ErrorCode::UnsupportedStream, "invalid FLAC metadata block");
            last = (uint8_t(header[0]) & 0x80) != 0;
            if (type == 0 || type == 4) {
                lastHeader = blocks.size();
                header[0] = char(type);
                blocks.append(TagLib::ByteVector(header, 4));
                std::string data(length, '\0');
                input.read(data.data(), length);
                if (!input) throw audio::Error(audio::ErrorCode::UnsupportedStream, "truncated FLAC metadata");
                blocks.append(TagLib::ByteVector(data.data(), length));
            } else {
                cover = cover || type == 6;
                input.seekg(length, std::ios::cur);
            }
            position += length;
        } while (!last);
        blocks[lastHeader] = char(uint8_t(blocks[lastHeader]) | 0x80);
        TagLib::ByteVectorStream stream(blocks);
        TagLib::FLAC::File file(&stream);
        const auto* properties = file.audioProperties();
        if (!file.isValid() || !properties)
            throw audio::Error(audio::ErrorCode::UnsupportedStream, "invalid FLAC metadata");
        const auto bits = properties->bitsPerSample();
        if ((bits != 16 && bits != 24) || properties->sampleRate() <= 0 || properties->channels() <= 0)
            throw audio::Error(audio::ErrorCode::UnsupportedStream, "unsupported FLAC bit depth");
        Metadata out;
        out.format = "flac";
        out.info.sampleRate = properties->sampleRate();
        out.info.channels = properties->channels();
        out.info.bitsPerSample = bits;
        out.info.totalFrames = properties->sampleFrames();
        out.tags = file.properties();
        out.cover = cover;
        return out;
    }
    const auto format = audio::probeFormat(path);
    audio::SourceInfo info;
    bool estimated = false;
    if (format == audio::AudioFormat::Mp3) info = audio::probeMp3Info(path, estimated);
    else if (format == audio::AudioFormat::Aac) info = audio::probeAacInfo(path);
    else info = audio::openDecoder(audio::defaultDecoder(format), path)->info();
    Metadata out{audio::formatId(format), info, {}, false, estimated};
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

// Without surrounding spaces, tabs, line breaks or ideographic spaces, which some taggers leave in names.
std::string trimmedTag(std::string text) {
    static constexpr std::string_view kIdeographicSpace = "\xE3\x80\x80", kBlank = " \t\r\n";
    for (bool changed = true; changed;) {
        changed = false;
        while (!text.empty() && kBlank.find(text.back()) != std::string_view::npos) text.pop_back(), changed = true;
        while (!text.empty() && kBlank.find(text.front()) != std::string_view::npos) text.erase(0, 1), changed = true;
        if (text.ends_with(kIdeographicSpace)) text.resize(text.size() - kIdeographicSpace.size()), changed = true;
        if (text.starts_with(kIdeographicSpace)) text.erase(0, kIdeographicSpace.size()), changed = true;
    }
    return text;
}

std::string tag(const Metadata& m, const char* name) {
    const auto values = m.tags.value(name);
    return values.isEmpty() ? std::string{} : trimmedTag(values.front().to8Bit(true));
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

// DATE holds a year, an ISO date or a compact YYYYMMDD date.
int year(const Metadata& m) {
    int value = number(m, "DATE");
    while (value > 9999) value /= 10;
    return value;
}

// One upsert carries the derived search and sort columns, so the row is written and FTS-indexed once.
void writeMetadata(sqlite::Database& db, int64_t file, const Metadata& m, std::wstring_view locale) {
    auto s = db.prepare("INSERT INTO tracks(file_id,title,album,album_artist,artist,composer,genre,year,track_number,track_total,"
                        "disc_number,disc_total,format,sample_rate,bits_per_sample,channels,total_frames,embedded_cover,lyrics,"
                        "length_estimated,title_sort_tag,album_sort_tag,artist_sort_tag,rg_track_gain,rg_track_peak,"
                        "rg_album_gain,rg_album_peak,title_norm,album_norm,artist_norm,title_sort,album_sort,artist_sort) "
                        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
                        "ON CONFLICT(file_id) DO UPDATE SET "
                        "title=excluded.title,album=excluded.album,album_artist=excluded.album_artist,artist=excluded.artist,"
                        "composer=excluded.composer,genre=excluded.genre,year=excluded.year,track_number=excluded.track_number,"
                        "track_total=excluded.track_total,disc_number=excluded.disc_number,disc_total=excluded.disc_total,"
                        "format=excluded.format,sample_rate=excluded.sample_rate,bits_per_sample=excluded.bits_per_sample,"
                        "channels=excluded.channels,total_frames=excluded.total_frames,embedded_cover=excluded.embedded_cover,"
                        "lyrics=excluded.lyrics,length_estimated=excluded.length_estimated,"
                        "title_sort_tag=excluded.title_sort_tag,album_sort_tag=excluded.album_sort_tag,"
                        "artist_sort_tag=excluded.artist_sort_tag,rg_track_gain=excluded.rg_track_gain,"
                        "rg_track_peak=excluded.rg_track_peak,rg_album_gain=excluded.rg_album_gain,"
                        "rg_album_peak=excluded.rg_album_peak,title_norm=excluded.title_norm,album_norm=excluded.album_norm,"
                        "artist_norm=excluded.artist_norm,title_sort=excluded.title_sort,album_sort=excluded.album_sort,"
                        "artist_sort=excluded.artist_sort");
    s.bind(1, file);
    int i = 2;
    for (const auto* field : {"TITLE", "ALBUM", "ALBUMARTIST", "ARTIST", "COMPOSER", "GENRE"}) s.bind(i++, tag(m, field));
    s.bind(8, int64_t(year(m)))
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
        .bind(20, int64_t(m.lengthEstimated));
    i = 21;
    for (const auto* field : {"TITLESORT", "ALBUMSORT", "ARTISTSORT"}) s.bind(i++, tag(m, field));
    for (const auto* field :
         {"REPLAYGAIN_TRACK_GAIN", "REPLAYGAIN_TRACK_PEAK", "REPLAYGAIN_ALBUM_GAIN", "REPLAYGAIN_ALBUM_PEAK"}) {
        const auto value = tag(m, field);
        char* end = nullptr;
        const double parsed = std::strtod(value.c_str(), &end);
        if (end != value.c_str() && std::isfinite(parsed))
            s.bind(i++, parsed);
        else
            s.bindNull(i++);
    }
    for (const auto* field : {"TITLE", "ALBUM", "ARTIST"}) s.bind(i++, normalizeLibrarySearch(tag(m, field)));
    for (const auto& [field, sortField] : {std::pair{"TITLE", "TITLESORT"}, {"ALBUM", "ALBUMSORT"}, {"ARTIST", "ARTISTSORT"}}) {
        const auto sortTag = tag(m, sortField);
        s.bind(i++, librarySortKey(sortTag.empty() ? tag(m, field) : sortTag, locale));
    }
    s.run();
    auto row = db.prepare("SELECT id FROM tracks WHERE file_id=?");
    row.bind(1, file).step();
    const int64_t track = row.integer(0);
    db.prepare("DELETE FROM track_values WHERE track_id=?").bind(1, track).run();
    auto value = db.prepare("INSERT INTO track_values(track_id,field,position,value,sort_source,value_norm,sort_key) "
                            "VALUES(?,?,?,?,?,?,?)");
    for (const auto* field : {"ARTIST", "COMPOSER", "GENRE"}) {
        int64_t position = 0;
        const auto sorts = m.tags.value((std::string(field) + "SORT").c_str());
        auto sort = sorts.begin();
        for (const auto& item : m.tags.value(field)) {
            const auto text = trimmedTag(item.to8Bit(true));
            const auto source = sort == sorts.end() ? std::string{} : trimmedTag((sort++)->to8Bit(true));
            if (text.empty()) continue;
            value.bind(1, track)
                .bind(2, toUtf8(lower(toWide(field))))
                .bind(3, position++)
                .bind(4, text)
                .bind(5, source)
                .bind(6, normalizeLibrarySearch(text))
                .bind(7, librarySortKey(source.empty() ? text : source, locale))
                .run();
            value.reset();
        }
    }
}

struct Entry {
    Path path;
    std::string kind;
    Version version;
    std::optional<Metadata> metadata;
    std::string error;
    bool rejected = false;
    std::optional<std::vector<uint8_t>> digest;
    bool listedVersion = false;
};

enum class ListedIds { Unknown, Accepted, Rejected };

struct Listing {
    Path path;
    std::vector<Path> children;
    std::vector<Entry> files;
    bool complete = true;
    bool deferred = false;
    std::string error;
    bool fileWork = false;
};

// Without `fileFolders` every album of the root is rebuilt; with them, only the albums of those folders, so a long
// first scan fills the album wall batch by batch.
void rebuildAlbums(sqlite::Database& db, int64_t root, const std::optional<std::set<int64_t>>& fileFolders = std::nullopt) {
    std::set<int64_t> albumFolders;
    std::string scope;
    if (fileFolders) {
        auto folder = db.prepare("SELECT parent_id,path FROM folders WHERE id=?");
        for (const auto id : *fileFolders) {
            folder.reset();
            if (!folder.bind(1, id).step()) continue;
            const bool disc = !folder.isNull(0) && isDiscFolderName(Path(toWide(folder.text(1))).filename().wstring());
            albumFolders.insert(disc ? folder.integer(0) : id);
        }
        if (albumFolders.empty()) return;
        std::string list;
        for (const auto id : albumFolders) list += (list.empty() ? "" : ",") + std::to_string(id);
        scope = " AND (f.folder_id IN (" + list + ") OR d.parent_id IN (" + list + "))";
    }
    struct Album {
        int64_t folder = 0;
        int64_t firstFile = 0;
        bool firstHasCover = false;
        std::string title, artist;
        int64_t year = 0, discs = 0;
        std::set<std::string> artists;
        std::vector<int64_t> tracks;
    };
    std::map<std::string, Album> albums;
    auto rows = db.prepare("SELECT t.id,t.album,t.album_artist,t.artist,t.year,f.folder_id,COALESCE(d.parent_id,0),d.path,"
                           "t.file_id,t.embedded_cover,t.disc_number,t.disc_total "
                           "FROM tracks t JOIN files f ON f.id=t.file_id JOIN folders d ON d.id=f.folder_id "
                           "WHERE f.root_id=? AND f.available=1" + scope + " ORDER BY t.disc_number,t.track_number,t.id");
    rows.bind(1, root);
    while (rows.step()) {
        const auto track = rows.integer(0);
        const auto discFolder = rows.integer(5);
        const auto parentFolder = rows.integer(6);
        Path folderPath(toWide(rows.text(7)));
        const bool discSubfolder = parentFolder && isDiscFolderName(folderPath.filename().wstring());
        const auto folder = discSubfolder ? parentFolder : discFolder;
        if (fileFolders && !albumFolders.contains(folder)) continue;
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
            if (inferred)
                db.prepare("UPDATE tracks SET disc_number=?1 WHERE id=?2 AND disc_number IS NOT ?1")
                    .bind(1, int64_t(inferred))
                    .bind(2, track)
                    .run();
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
        // Rewriting an unchanged album_id would count as a library change for every track of a rescanned root.
        for (auto track : album.tracks)
            db.prepare("UPDATE tracks SET album_id=?1 WHERE id=?2 AND album_id IS NOT ?1").bind(1, albumId).bind(2, track).run();
        updateLibraryAlbumText(db, albumId);

        // The scan indexed the folder's images, so the cover is chosen without listing the folder again (slow on a NAS).
        int64_t coverFile = 0;
        std::string source;
        std::string mime;
        std::optional<std::pair<size_t, const char*>> best;
        int64_t bestSize = 0;
        auto images = db.prepare("SELECT id,path,size FROM files WHERE folder_id=? AND kind='image' AND available=1");
        images.bind(1, album.folder);
        while (images.step())
            if (const auto rank = folderArtworkRank(Path(toWide(images.text(1)))); rank && (!best || rank->first < best->first)) {
                best = rank;
                bestSize = images.integer(2);
                coverFile = images.integer(0);
            }
        if (best && bestSize > 0 && uintmax_t(bestSize) <= kMaxArtworkBytes) {
            source = "folder";
            mime = best->second;
        } else {
            coverFile = 0;
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
            "DELETE FROM covers WHERE NOT EXISTS(SELECT 1 FROM albums WHERE albums.cover_id=covers.id);"
            // Album rows carry no revision trigger; one step covers titles and covers this rebuild changed.
            "UPDATE library_revision SET value=value+1 WHERE id=1");
}

Listing readEntries(Listing out, const std::map<std::string, Version>& known,
                    const std::map<int64_t, library::Fingerprint>& hashSizes,
                    const std::set<std::string>& pendingIdentity, const std::set<std::string>& knownHashes,
                    const std::set<TrustedIdentity>& trusted, bool reread,
                    const CancellationToken& token, const std::function<bool()>& playbackActive) {
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    auto restore = wil::scope_exit([] { SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END); });
    // Entries settled by the listing need no file access, so the alias check runs before the first file is opened.
    std::optional<std::string> aliasError;
    bool aliasChecked = false;
    const auto directoryValid = [&] {
        if (!std::exchange(aliasChecked, true)) {
            try {
                if (canonicalRoot(out.path) != Path(lower(out.path.wstring())))
                    throw std::runtime_error("directory now resolves through an alias: " + key(out.path));
            } catch (const std::exception& e) {
                aliasError = e.what();
            }
        }
        return !aliasError;
    };
    for (auto& entry : out.files) {
        if (token.cancelled()) {
            out.complete = false;
            break;
        }
        const auto& child = entry.path;
        const auto& type = entry.kind;
        try {
            const auto fileKey = key(child);
            const auto old = known.find(fileKey);
            if (!entry.listedVersion || (old != known.end() && !old->second.created && old->second.index.size() == 8)) {
                if (!directoryValid()) break;
                entry.version = fileVersion(child);
            }
            if (reread || old == known.end() || old->second != entry.version) {
                if (!directoryValid()) break;
                if (type == "audio") entry.metadata = metadata(child, entry.version.size);
                // Fingerprinted now, while the start of the file the tags came from is still in the storage's cache: a
                // later pass read each file from a NAS disk again, the slowest part of building a library. The
                // fingerprint also confirms the version; during playback it is left to that pass. Images get none: a
                // cover is chosen again by every scan, so following its moves keeps nothing.
                if (type != "image" && (!playbackActive || !playbackActive())) {
                    const auto kind = hashSizes.contains(entry.version.size) ? hashSizes.at(entry.version.size)
                                                                              : library::Fingerprint::Sampled;
                    entry.digest = library::fileDigest(child, entry.version, token, playbackActive, kind);
                }
                if (type == "audio" && !entry.digest && fileVersion(child) != entry.version)
                    throw std::runtime_error("file changed during metadata read");
            }
            const bool hashed = old != known.end() && old->second == entry.version && knownHashes.contains(fileKey);
            if (!hashed && !entry.digest && type != "image" && (old == known.end() || !entry.version.sameFile(old->second) || pendingIdentity.contains(fileKey)) &&
                hashSizes.contains(entry.version.size) &&
                !trusted.contains({entry.version.volumeKey, entry.version.index, entry.version.created})) {
                if (!directoryValid()) break;
                entry.digest =
                    library::fileDigest(child, entry.version, token, playbackActive, hashSizes.at(entry.version.size));
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
    }
    if (aliasError) {
        out.complete = false;
        out.error = *aliasError;
        out.files.clear();
    }
    return out;
}

Listing list(const Path& path, const CancellationToken& token, bool network, std::atomic<ListedIds>& listedIds) {
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    Listing out{path};
    wil::unique_handle directory;
    try {
        directory = openPath(path, FILE_LIST_DIRECTORY);
        if (!directory) throw std::runtime_error("cannot access library path: " + toUtf8(path.wstring()));
        if (finalPath(directory.get()) != Path(lower(path.wstring())))
            throw std::runtime_error("directory now resolves through an alias: " + key(path));
    } catch (const std::exception& e) {
        out.complete = false;
        out.error = e.what();
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
        return out;
    }
    if (network && listedIds.load() != ListedIds::Rejected) {
        if (auto entries = library::directoryEntries(directory.get(), token)) {
            for (auto& item : *entries) {
                if (token.cancelled()) {
                    out.complete = false;
                    break;
                }
                if (item.name == L"." || item.name == L"..") continue;
                if (item.attributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                const Path child = path / item.name;
                if (item.attributes & FILE_ATTRIBUTE_DIRECTORY) {
                    out.children.push_back(child);
                    continue;
                }
                const auto type = kind(child);
                if (type.empty()) continue;
                Entry entry{child, type};
                entry.version = std::move(item.version);
                entry.listedVersion = true;
                out.files.push_back(std::move(entry));
            }
            // One direct query per scan confirms the server reports the same IDs as a file handle.
            if (listedIds.load() == ListedIds::Unknown && !out.files.empty()) {
                bool matches = false;
                try {
                    matches = library::fileVersion(out.files.front().path) == out.files.front().version;
                } catch (const std::exception&) {
                }
                auto expected = ListedIds::Unknown;
                if (!matches) listedIds = ListedIds::Rejected;
                else listedIds.compare_exchange_strong(expected, ListedIds::Accepted);
            }
            if (listedIds.load() == ListedIds::Rejected)
                for (auto& entry : out.files) entry.listedVersion = false;
            SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
            return out;
        }
        if (!token.cancelled()) listedIds = ListedIds::Rejected;
    }
    directory.reset();
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
            out.files.push_back(Entry{child, type});
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
    const Path resolved = canonicalRoot(path);
    const DWORD attr = GetFileAttributesW(resolved.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
        throw std::invalid_argument("root must be a directory");
    // A root beside the others is registered at once, even while another root's scan runs (a first NAS scan takes
    // hours); one that absorbs existing roots first stops their scans, which would otherwise write their old root ids.
    std::vector<int64_t> absorbed;
    {
        auto db = store_.openReader();
        auto roots = db.prepare("SELECT id,path FROM roots WHERE removed IS NULL");
        while (roots.step()) {
            const Path existing(toWide(roots.text(1)));
            if (contains(resolved, existing) && !contains(existing, resolved)) absorbed.push_back(roots.integer(0));
        }
    }
    std::vector<std::unique_lock<std::mutex>> stopped;
    std::unique_lock<std::mutex> crossRoot;
    if (!absorbed.empty()) {
        {
            std::lock_guard jobsLock(store_.jobsMutex_);
            for (const auto child : absorbed) {
                const auto [first, last] = store_.jobs_.equal_range(child);
                for (auto job = first; job != last; ++job) job->second.cancel();
            }
        }
        for (const auto child : absorbed) stopped.emplace_back(rootLock(child));
        crossRoot = std::unique_lock(store_.scanMutex_);
    }
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

std::mutex& LibraryScanner::rootLock(int64_t root) {
    std::lock_guard lock(store_.jobsMutex_);
    auto& mutex = store_.rootLocks_[root];
    if (!mutex) mutex = std::make_unique<std::mutex>();
    return *mutex;
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

ScanReport LibraryScanner::scan(int64_t root, const CancellationToken& request, const std::vector<Path>& folders) {
    ++store_.activeScans_;
    auto finishScan = wil::scope_exit([this] { --store_.activeScans_; });
    std::lock_guard lock(rootLock(root));
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
    int64_t found = 0, checked = 0;
    bool finishing = false;
    ++store_.scanning_;
    auto idle = wil::scope_exit([&] {
        --store_.scanning_;
        store_.scanFound_ -= found;
        store_.scanChecked_ -= checked;
        if (finishing) --store_.scanFinishing_;
    });
    ScanReport report;
    auto reader = store_.openReader();
    auto row = reader.prepare("SELECT path FROM roots WHERE id=? AND removed IS NULL");
    if (!row.bind(1, root).step()) throw std::invalid_argument("unknown or removed library root");
    const Path path(toWide(row.text(0)));
    row.reset();
    // More in-flight metadata requests hide SMB latency; playback still limits the storage to one worker.
    const bool network = isNetworkPath(path);
    const unsigned idleWorkers = network ? 16u : 4u;
    std::atomic<ListedIds> listedIds = ListedIds::Unknown;
    const auto playbackActive = playbackGate(path);
    std::vector<Path> subtrees;
    for (const auto& folder : folders)
        if (contains(path, folder) && folder != path) subtrees.push_back(folder);
    const bool partial = !subtrees.empty();
    // How the root reads on disk, for the folder pages and Settings; the scan is reading this storage anyway.
    const auto display = partial ? std::string{} : libraryDisplayPath(key(path));
    const auto rootName = toUtf8(Path(toWide(display)).filename().wstring());
    if (!partial)
        store_
            .write([&](sqlite::Database& db) {
                db.prepare("UPDATE roots SET display=?,location=? WHERE id=?")
                    .bind(1, display)
                    .bind(2, libraryLocation(key(path)))
                    .bind(3, root)
                    .run();
            })
            .get();
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
    auto files = reader.prepare("SELECT path,size,mtime,volume_serial,file_index,created,volume_key,identity_state,content_hash IS NOT NULL "
                                "FROM files WHERE root_id=? AND available=1");
    files.bind(1, root);
    std::set<std::string> pendingIdentity;
    std::set<std::string> knownHashes;
    while (files.step()) {
        known.emplace(files.text(0), library::storedVersion(files, 1));
        if (files.integer(7)) pendingIdentity.insert(files.text(0));
        if (files.integer(8)) knownHashes.insert(files.text(0));
    }
    // A new file is fingerprinted like the recorded files of its size; sampled wins where both kinds exist.
    std::map<int64_t, library::Fingerprint> hashSizes;
    auto fingerprints =
        reader.prepare("SELECT size,MAX(length(content_hash)) FROM files WHERE content_hash IS NOT NULL GROUP BY size");
    while (fingerprints.step())
        hashSizes.emplace(fingerprints.integer(0),
                          fingerprints.integer(1) == 32 ? library::Fingerprint::Full : library::Fingerprint::Sampled);
    std::set<TrustedIdentity> trusted;
    auto identities = reader.prepare("SELECT volume_key,file_index,created FROM files WHERE volume_key<>'' AND identity_state=0");
    while (identities.step()) trusted.emplace(identities.text(0), identities.blob(1), identities.integer(2));
    const int64_t generation = reader.scalar("SELECT COALESCE(MAX(last_seen),0)+1 FROM files").value();
    std::deque<Listing> pending;
    if (!partial) pending.push_back(Listing{path});
    for (const auto& folder : subtrees)
        if (const DWORD attributes = GetFileAttributesW(folder.c_str());
            attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY))
            pending.push_back(Listing{folder});
    std::deque<std::future<Listing>> active;
    bool complete = true;
    std::vector<Entry> batch;
    const auto derivedKey = "library_derived_dirty:" + std::to_string(root);
    auto dirty = reader.prepare("SELECT 1 FROM app_state WHERE key=?");
    bool derivedChanged = dirty.bind(1, derivedKey).step();
    dirty.reset();
    const auto rereadKey = "library_reread:" + std::to_string(root);
    dirty.bind(1, rereadKey);
    const bool reread = dirty.step();
    dirty.reset();
    derivedChanged = derivedChanged || reread;
    // Writes run in submission order on the store's writer; the dispatcher waits only when several are outstanding.
    std::deque<std::future<void>> writes;
    auto drainWrites = wil::scope_exit([&] {
        for (auto& write : writes) write.wait();
    });
    const auto submit = [&](std::function<void(sqlite::Database&)> work) {
        writes.push_back(store_.write(std::move(work)));
        while (!writes.empty() &&
               (writes.size() > 4 || writes.front().wait_for(std::chrono::seconds(0)) == std::future_status::ready)) {
            writes.front().get();
            writes.pop_front();
        }
    };
    auto flush = [&] {
        if (batch.empty()) return;
        auto entries = std::make_shared<std::vector<Entry>>(std::move(batch));
        batch.clear();
        submit([&, entries](sqlite::Database& db) {
            if (token.cancelled()) return;
            auto seen = db.prepare("UPDATE files SET last_seen=?,name=coalesce(name,?) WHERE id=?");
            auto old = db.prepare("SELECT id,size,mtime,volume_serial,file_index,created,volume_key,root_id FROM files WHERE "
                                  "path=? AND available=1");
            auto folders = db.prepare("SELECT id FROM folders WHERE path=?");
            auto localeRow = db.prepare("SELECT value FROM app_state WHERE key='sort_locale'");
            const std::wstring locale = localeRow.step() ? toWide(localeRow.text(0)) : L"zh-TW";
            std::set<int64_t> trackFolders;
            auto ungrouped = db.prepare("SELECT f.folder_id FROM tracks t JOIN files f ON f.id=t.file_id "
                                        "WHERE t.file_id=? AND t.album_id IS NULL LIMIT 1");
            for (const auto& entry : *entries) {
                const auto p = key(entry.path);
                old.reset();
                old.bind(1, p);
                int64_t file = old.step() ? old.integer(0) : 0;
                if (!entry.error.empty() && !entry.rejected) {
                    if (file) db.prepare("UPDATE files SET last_seen=? WHERE id=?").bind(1, generation).bind(2, file).run();
                    continue;
                }
                if (file && old.integer(7) == root && library::storedVersion(old, 1) == entry.version &&
                    entry.error.empty() && !entry.digest && !entry.metadata) {
                    seen.bind(1, generation).bind(2, toUtf8(entry.path.filename().wstring())).bind(3, file).run();
                    seen.reset();
                    // Tracks indexed by an interrupted first scan have no album yet.
                    ungrouped.reset();
                    if (ungrouped.bind(1, file).step()) trackFolders.insert(ungrouped.integer(0));
                    continue;
                }
                if (!file || library::storedVersion(old, 1) != entry.version) derivedChanged = true;
                if (file && !entry.version.sameFile(library::storedVersion(old, 1))) {
                    db.prepare("UPDATE files SET available=0 WHERE id=?").bind(1, file).run();
                    file = 0;
                }
                folders.reset();
                folders.bind(1, key(entry.path.parent_path())).step();
                const int64_t folderId = folders.integer(0);
                if (!file) {
                    db.prepare("INSERT INTO "
                               "files(folder_id,root_id,path,kind,size,mtime,volume_serial,file_index,last_seen,created,"
                               "volume_key,identity_state,name,added) "
                               "VALUES(?,?,?,?,?,?,?,?,?,?,?,1,?,?)")
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
                        .bind(12, toUtf8(entry.path.filename().wstring()))
                        .bind(13, addedTime(entry.version.created))
                        .run();
                    file = db.lastInsertId();
                } else {
                    if (library::storedVersion(old, 1) != entry.version)
                        db.prepare("UPDATE files SET content_hash=NULL WHERE id=?").bind(1, file).run();
                    db.prepare("UPDATE files SET size=?,mtime=?,last_seen=?,folder_id=?,root_id=?,"
                               "volume_serial=?,file_index=?,created=?,volume_key=?,name=? WHERE id=?")
                        .bind(1, entry.version.size)
                        .bind(2, entry.version.mtime)
                        .bind(3, generation)
                        .bind(4, folderId)
                        .bind(5, root)
                        .bind(6, entry.version.volume)
                        .bind(7, entry.version.index)
                        .bind(8, entry.version.created)
                        .bind(9, entry.version.volumeKey)
                        .bind(10, toUtf8(entry.path.filename().wstring()))
                        .bind(11, file)
                        .run();
                }
                if (entry.digest)
                    db.prepare("UPDATE files SET content_hash=? WHERE id=?").bind(1, *entry.digest).bind(2, file).run();
                if (entry.rejected) db.prepare("DELETE FROM tracks WHERE file_id=?").bind(1, file).run();
                if (entry.rejected || entry.metadata) trackFolders.insert(folderId);
                if (entry.metadata) {
                    writeMetadata(db, file, *entry.metadata, locale);
                    report.updated++;
                }
            }
            if (!trackFolders.empty()) rebuildAlbums(db, root, trackFolders);
            if (derivedChanged)
                db.prepare("INSERT OR REPLACE INTO app_state(key,value) VALUES(?,1)").bind(1, derivedKey).run();
        });
    };
    while ((!pending.empty() || !active.empty()) && !token.cancelled()) {
        while (!pending.empty() && active.size() < (playbackActive() ? 1u : idleWorkers)) {
            auto work = std::move(pending.front());
            pending.pop_front();
            active.push_back(std::async(std::launch::async, [&, work = std::move(work)]() mutable {
                if (!work.fileWork) return list(work.path, token, network, listedIds);
                return readEntries(std::move(work), known, hashSizes, pendingIdentity, knownHashes, trusted, reread, token,
                                   playbackActive);
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
        if (!listing.fileWork) {
            submit([root, folder = key(listing.path),
                    parent = listing.path == path ? std::string{} : key(listing.path.parent_path()),
                    name = listing.path == path ? rootName : toUtf8(listing.path.filename().wstring())](sqlite::Database& db) {
                db.prepare("INSERT INTO folders(root_id,path,parent_id,name) VALUES(?,?,(SELECT id FROM folders WHERE path=?),?) "
                           "ON CONFLICT(path) DO UPDATE SET root_id=excluded.root_id,parent_id=excluded.parent_id,"
                           "name=excluded.name")
                    .bind(1, root)
                    .bind(2, folder)
                    .bind(3, parent)
                    .bind(4, name)
                    .run();
            });
            for (auto& child : listing.children) pending.push_back(Listing{std::move(child)});
            found += int64_t(listing.files.size());
            store_.scanFound_ += int64_t(listing.files.size());
            for (size_t end = listing.files.size(); end > 0;) {
                const size_t begin = end > 64 ? end - 64 : 0;
                Listing work{listing.path};
                work.fileWork = true;
                for (size_t i = begin; i < end; ++i) work.files.push_back(std::move(listing.files[i]));
                pending.push_front(std::move(work));
                end = begin;
            }
            continue;
        }
        for (auto& entry : listing.files) {
            report.seen++;
            ++checked;
            ++store_.scanChecked_;
            if (!entry.error.empty()) report.diagnostics.push_back(key(entry.path) + ": " + entry.error);
            batch.push_back(std::move(entry));
            if (batch.size() == 500) flush();
        }
    }
    for (auto& job : active) job.get();
    flush();
    for (; !writes.empty(); writes.pop_front()) writes.front().get();
    report.online = bool(openPath(path, FILE_LIST_DIRECTORY));
    if (!report.online) {
        report.rootError = GetLastError();
        report.diagnostics.push_back("root became unavailable (" + std::to_string(report.rootError) + "): " + key(path));
    }
    report.complete = complete && !token.cancelled() && report.online;
    std::lock_guard crossRoot(store_.scanMutex_);
    finishing = true;
    ++store_.scanFinishing_;
    store_
        .write([&](sqlite::Database& db) {
            if (token.cancelled()) return;
            db.prepare("UPDATE roots SET online=? WHERE id=? AND online<>?")
                .bind(1, int64_t(report.online))
                .bind(2, root)
                .bind(3, int64_t(report.online))
                .run();
            if (report.complete) {
                if (!partial) {
                    db.prepare("UPDATE roots SET last_scan=? WHERE id=?").bind(1, now()).bind(2, root).run();
                    db.prepare("DELETE FROM app_state WHERE key=?").bind(1, rereadKey).run();
                    db.prepare("UPDATE files SET available=0 WHERE root_id=? AND available=1 AND (last_seen IS NULL OR last_seen<>?)")
                        .bind(1, root)
                        .bind(2, generation)
                        .run();
                    derivedChanged = derivedChanged || db.scalar("SELECT changes()").value_or(0) != 0;
                }
                // A path range over the unique files.path index selects a subtree without a LIKE pattern.
                for (const auto& folder : subtrees) {
                    const auto below = key(folder);
                    db.prepare("UPDATE files SET available=0 WHERE root_id=? AND available=1 AND "
                               "(last_seen IS NULL OR last_seen<>?) AND path>?||'\\' AND path<?||']'")
                        .bind(1, root)
                        .bind(2, generation)
                        .bind(3, below)
                        .bind(4, below)
                        .run();
                    derivedChanged = derivedChanged || db.scalar("SELECT changes()").value_or(0) != 0;
                }
                std::vector<int64_t> added;
                auto pending = db.prepare("SELECT id FROM files WHERE root_id=? AND available=1 AND identity_state=1");
                pending.bind(1, root);
                while (pending.step()) added.push_back(pending.integer(0));
                derivedChanged = derivedChanged || !added.empty();
                report.moved = library::reconcileMoves(db, added);
                db.prepare("UPDATE files SET identity_state=0 WHERE root_id=? AND identity_state=1").bind(1, root).run();
                if (derivedChanged)
                    db.prepare("INSERT OR REPLACE INTO app_state(key,value) VALUES(?,1)").bind(1, derivedKey).run();
            }
        })
        .get();
    if (report.complete && derivedChanged)
        store_.write([&](sqlite::Database& db) {
            if (token.cancelled()) return;
            rebuildAlbums(db, root);
            db.prepare("DELETE FROM app_state WHERE key=?").bind(1, derivedKey).run();
        }).get();
    report.complete = report.complete && !token.cancelled();
    return report;
}

void LibraryScanner::recordExactLength(int64_t file, uint32_t sampleRate, uint64_t frames) {
    store_.write([file, sampleRate, frames](sqlite::Database& db) {
        db.prepare("UPDATE tracks SET total_frames=?,length_estimated=0 "
                   "WHERE file_id=? AND length_estimated=1 AND sample_rate=?")
            .bind(1, int64_t(frames))
            .bind(2, file)
            .bind(3, int64_t(sampleRate))
            .run();
    });
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
    const auto playing = playbackGate(Path(toWide(rootRow.text(0))));
    const bool network = isNetworkPath(Path(toWide(rootRow.text(0))));
    const auto backgroundBlocked = [this, playing] { return store_.activeScans_.load() != 0 || playing(); };
    rootRow.reset();
    if (token.cancelled() || backgroundBlocked()) return report;
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
                                   "AND (content_hash IS NULL OR length(content_hash)=32) AND identity_state<>2 "
                                   "AND files.kind<>'image' AND roots.online=1 AND roots.removed IS NULL");
        rows.bind(1, root);
        while (rows.step()) {
            if (token.cancelled() || backgroundBlocked()) return report;
            pending.push_back({rows.integer(0), rows.text(1), library::storedVersion(rows, 2)});
        }
    }
    // Fingerprints are saved a batch at a time: a write transaction per file made each one wait for a commit to disk,
    // about 7 files a second on a local disk. The files were already checked after hashing, so the write only compares
    // with the database: checking them again on the storage held every other write for one NAS round trip per file.
    std::vector<std::pair<const PendingHash*, std::vector<uint8_t>>> digests;
    const auto save = [&] {
        if (digests.empty()) return;
        store_
            .write([&](sqlite::Database& db) {
                if (token.cancelled() || backgroundBlocked()) return;
                auto current = db.prepare("SELECT path,size,mtime,volume_serial,file_index,created,volume_key FROM files "
                                          "WHERE id=? AND available=1 AND identity_state<>2 "
                                          "AND (content_hash IS NULL OR length(content_hash)=32)");
                auto update = db.prepare("UPDATE files SET content_hash=? WHERE id=?");
                for (const auto& [file, digest] : digests) {
                    try {
                        const bool unchanged = current.bind(1, file->id).step() && current.text(0) == file->path &&
                                               library::storedVersion(current, 1) == file->version;
                        current.reset();
                        if (!unchanged) continue;
                        update.bind(1, digest).bind(2, file->id).run();
                        update.reset();
                        report.hashed++;
                    } catch (const std::exception& e) {
                        current.reset();
                        report.diagnostics.push_back(file->path + ": " + e.what());
                    }
                }
            })
            .get();
        digests.clear();
    };
    // On a NAS each file mostly waits for network round trips (7.5 files a second one by one), so sixteen are hashed at
    // once there, as the scan reads them; four on a local disk.
    struct Result {
        std::optional<std::vector<uint8_t>> digest;
        std::string error;
    };
    bool stopped = false;
    constexpr size_t kBatch = 256;
    for (size_t first = 0; first < pending.size() && !stopped; first += kBatch) {
        if ((stopped = token.cancelled() || backgroundBlocked())) break;
        const size_t count = std::min(kBatch, pending.size() - first);
        std::vector<Result> results(count);
        std::atomic<size_t> next = 0;
        std::vector<std::future<void>> workers;
        for (int i = 0; i < (network ? 16 : 4); ++i)
            workers.push_back(std::async(std::launch::async, [&] {
                const bool background = SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != 0;
                for (size_t at; (at = next++) < count;) {
                    const auto& file = pending[first + at];
                    try {
                        results[at].digest = library::fileDigest(Path(toWide(file.path)), file.version, token, backgroundBlocked);
                    } catch (const std::exception& e) {
                        results[at].error = e.what();
                    }
                }
                if (background) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
            }));
        for (auto& worker : workers) worker.get();
        for (size_t at = 0; at < count; ++at) {
            if (!results[at].error.empty())
                report.diagnostics.push_back(pending[first + at].path + ": " + results[at].error);
            else if (results[at].digest)
                digests.emplace_back(&pending[first + at], std::move(*results[at].digest));
            else
                stopped = true; // cancelled, or playback or covers need the storage
        }
        save();
    }
    report.complete = !stopped && !token.cancelled() && !backgroundBlocked() && report.diagnostics.empty();
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

    void refreshRoots() { refresh_ = true; }
    void rescanAll() { rescan_ = true; }

private:
    struct Watch {
        wil::unique_handle directory, event;
        OVERLAPPED io{};
        std::array<uint8_t, 32768> buffer{};
        Clock::time_point due{}, lastPoll{};
        bool pending = false;
        bool deferred = false;
        std::function<bool()> playbackActive;
        Path root;
        // Folders the notifications named since the last scan; a full scan when they cannot say (overflow, failure,
        // a change at the root itself, startup, polling).
        std::set<Path> changed;
        bool full = true;

        // A folder that is there (new, moved in, or changed because its contents did) is listed itself; a file lists the
        // folder holding it. What went away may have been a folder, so its own path is reconciled too.
        void collect(DWORD bytes) {
            for (size_t offset = 0; offset < bytes;) {
                const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer.data() + offset);
                const Path item = root / std::wstring(info->FileName, info->FileNameLength / sizeof(wchar_t));
                const DWORD attributes = info->Action == FILE_ACTION_REMOVED || info->Action == FILE_ACTION_RENAMED_OLD_NAME
                                             ? INVALID_FILE_ATTRIBUTES
                                             : GetFileAttributesW(item.c_str());
                const bool gone = attributes == INVALID_FILE_ATTRIBUTES;
                const Path folder = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)
                                        ? item
                                        : item.parent_path();
                if (gone) changed.insert(item);
                if (folder != root) changed.insert(folder);
                else if (!gone || !kind(item).empty()) full = true;
                if (!info->NextEntryOffset) break;
                offset += info->NextEntryOffset;
            }
        }

        // The changed folders without those inside another; empty for a full scan.
        std::vector<Path> take() {
            std::vector<Path> folders;
            if (!full)
                for (const auto& folder : changed)
                    if (folders.empty() || !contains(folders.back(), folder)) folders.push_back(folder);
            changed.clear();
            full = false;
            return folders;
        }
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
        // Each root scans on its own thread, so a long NAS scan neither delays other roots nor these checks.
        std::map<int64_t, std::future<ScanReport>> scans;
        auto check = Clock::time_point{};
        while (!stop.stop_requested()) {
            try {
                const auto time = Clock::now();
                if (time >= check || refresh_.exchange(false)) {
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
                                    w.prepare("UPDATE roots SET online=0 WHERE id=? AND online<>0").bind(1, id).run();
                                })
                                .get();
                            continue;
                        }
                        // A first NAS scan can take hours or wait for playback; the root is browsable again meanwhile.
                        if (!roots.integer(2))
                            store_
                                .write([id](sqlite::Database& w) {
                                    w.prepare("UPDATE roots SET online=1 WHERE id=?").bind(1, id).run();
                                })
                                .get();
                        if (!watches.contains(id) || !watches.at(id)->pending) {
                            watches.erase(id);
                            auto watch = std::make_unique<Watch>();
                            watch->playbackActive = scanner_.playbackGate(path);
                            watch->directory =
                                openPath(path, FILE_LIST_DIRECTORY, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED);
                            watch->event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
                            if (watch->directory && watch->event) watch->arm();
                            watch->root = path;
                            watch->due = time;
                            watch->lastPoll = time;
                            watches.emplace(id, std::move(watch));
                        }
                        auto& watch = *watches.at(id);
                        // A share's change notifications are unreliable, so it is also walked periodically, like Roon's
                        // rescan interval (4 hours by default there).
                        const auto interval = isNetworkPath(path) ? options_.networkPollInterval : options_.pollInterval;
                        if (interval.count() && time - watch.lastPoll >= interval) {
                            watch.due = time;
                            watch.full = true;
                            watch.lastPoll = time;
                        }
                    }
                    std::erase_if(watches, [&](const auto& pair) { return !live.contains(pair.first); });
                }
                const bool rescan = rescan_.exchange(false);
                for (auto& [id, ptr] : watches) {
                    auto& watch = *ptr;
                    if (rescan) watch.due = Clock::now(), watch.full = true;
                    if (watch.deferred && !watch.playbackActive()) {
                        watch.deferred = false;
                        watch.due = Clock::now();
                        watch.full = true;
                    }
                    if (watch.pending && WaitForSingleObject(watch.event.get(), 0) == WAIT_OBJECT_0) {
                        DWORD bytes = 0;
                        const bool ok = bool(GetOverlappedResult(watch.directory.get(), &watch.io, &bytes, FALSE));
                        watch.pending = false;
                        // Empty/overflowed buffers and failed notifications both require a full incremental scan.
                        if (ok && bytes) watch.collect(bytes);
                        else watch.full = true;
                        if (watch.due == Clock::time_point{} || (ok && bytes)) watch.due = Clock::now() + options_.coalesce;
                        watch.arm();
                    }
                    if (watch.due != Clock::time_point{} && Clock::now() >= watch.due && !stop.stop_requested() &&
                        !scans.contains(id)) {
                        watch.due = {};
                        scans.emplace(id, std::async(std::launch::async, [this, id, folders = watch.take()] {
                            SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
                            auto normal = wil::scope_exit([] { SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END); });
                            return scanner_.scan(id, token_, folders);
                        }));
                    }
                }
                for (auto it = scans.begin(); it != scans.end();) {
                    if (it->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                        ++it;
                        continue;
                    }
                    const auto id = it->first;
                    auto job = std::move(it->second);
                    it = scans.erase(it);
                    try {
                        const auto report = job.get();
                        if (const auto watch = watches.find(id); watch != watches.end()) watch->second->deferred = report.deferred;
                        for (const auto& diagnostic : report.diagnostics) log::warn("library: %s", diagnostic.c_str());
                    } catch (const std::exception& e) {
                        log::warn("library scan: %s", e.what());
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
    std::atomic<bool> refresh_ = false;
    std::atomic<bool> rescan_ = false;
    std::jthread worker_;
    std::jthread fingerprints_;
};

LibraryMonitor::LibraryMonitor(LibraryStore& store) : LibraryMonitor(store, Options{}) {}
LibraryMonitor::LibraryMonitor(LibraryStore& store, Options options) : impl_(std::make_unique<Impl>(store, options)) {}
LibraryMonitor::~LibraryMonitor() = default;
void LibraryMonitor::refreshRoots() {
    impl_->refreshRoots();
}
void LibraryMonitor::rescanAll() {
    impl_->rescanAll();
}

void requestMetadataReread(LibraryStore& store) {
    store
        .write([](sqlite::Database& db) {
            db.exec("INSERT OR REPLACE INTO app_state(key,value) "
                    "SELECT 'library_reread:'||id,1 FROM roots WHERE removed IS NULL");
        })
        .get();
}

} // namespace camel::server
