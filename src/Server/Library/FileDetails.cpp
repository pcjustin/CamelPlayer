// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Library/FileDetails.h"

#include <windows.h>

#include <taglib/aifffile.h>
#include <taglib/apeitem.h>
#include <taglib/apetag.h>
#include <taglib/commentsframe.h>
#include <taglib/dsdiffdiintag.h>
#include <taglib/dsdifffile.h>
#include <taglib/dsffile.h>
#include <taglib/fileref.h>
#include <taglib/flacfile.h>
#include <taglib/id3v1tag.h>
#include <taglib/id3v2header.h>
#include <taglib/id3v2tag.h>
#include <taglib/infotag.h>
#include <taglib/mp4file.h>
#include <taglib/mp4item.h>
#include <taglib/mp4itemfactory.h>
#include <taglib/mp4tag.h>
#include <taglib/mpegfile.h>
#include <taglib/textidentificationframe.h>
#include <taglib/tpropertymap.h>
#include <taglib/urllinkframe.h>
#include <taglib/wavfile.h>
#include <taglib/xiphcomment.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <ranges>
#include <string_view>
#include <vector>

#include "Common/AudioFormat.h"
#include "Common/util/Text.h"
#include "Server/Library/LibraryScanner.h"
#include "Server/Library/LibraryViews.h"

namespace camel::server {

namespace {

using Strings = std::vector<std::string>;

std::string utf8(const TagLib::String& text) {
    return text.to8Bit(true);
}

Strings utf8(const TagLib::StringList& list) {
    Strings out;
    out.reserve(list.size());
    for (const auto& text : list) out.push_back(utf8(text));
    return out;
}

std::string upper(std::string text) {
    std::ranges::transform(text, text.begin(), [](char c) { return c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : c; });
    return text;
}

// Pictures and lyrics are not shown.
bool leftOut(const std::string& name) {
    static constexpr std::array<std::string_view, 6> kLeftOut{"LYRICS",   "UNSYNCEDLYRICS", "METADATA_BLOCK_PICTURE",
                                                              "COVERART", "COVERARTMIME",   "PICTURE"};
    const auto key = upper(name);
    return key.starts_with("COVER ART") || std::ranges::find(kLeftOut, key) != kLeftOut.end();
}

void add(FileTagSection& section, std::string key, std::string field, Strings values) {
    if (std::ranges::all_of(values, &std::string::empty) || leftOut(key) || leftOut(field)) return;
    section.tags.push_back({std::move(key), std::move(field), std::move(values), std::nullopt});
}

FileTagSection id3v2Section(const TagLib::ID3v2::Tag& tag) {
    FileTagSection section{"ID3v2." + std::to_string(tag.header()->majorVersion()), {}};
    for (const auto* frame : tag.frameList()) {
        const auto id = frame->frameID();
        std::string key(id.data(), id.size());
        // Text, URL and comment frames; the others hold pictures, lyrics, ratings or binary data.
        TagLib::PropertyMap properties;
        const auto* text = dynamic_cast<const TagLib::ID3v2::TextIdentificationFrame*>(frame);
        if (text) {
            if (const auto* user = dynamic_cast<const TagLib::ID3v2::UserTextIdentificationFrame*>(text))
                key += ":" + utf8(user->description());
            properties = text->asProperties();
        } else if (const auto* url = dynamic_cast<const TagLib::ID3v2::UrlLinkFrame*>(frame)) {
            if (const auto* user = dynamic_cast<const TagLib::ID3v2::UserUrlLinkFrame*>(url))
                key += ":" + utf8(user->description());
            properties = url->asProperties();
        } else if (const auto* comment = dynamic_cast<const TagLib::ID3v2::CommentsFrame*>(frame)) {
            if (!comment->description().isEmpty()) key += ":" + utf8(comment->description());
            properties = comment->asProperties();
        } else {
            continue;
        }
        for (const auto& [field, values] : properties) add(section, key, utf8(field), utf8(values));
        if (properties.isEmpty() && text) add(section, key, {}, utf8(text->fieldList()));
    }
    return section;
}

FileTagSection id3v1Section(const TagLib::ID3v1::Tag& tag) {
    FileTagSection section{"ID3v1", {}};
    for (const auto& [field, values] : tag.properties()) add(section, utf8(field), utf8(field), utf8(values));
    return section;
}

FileTagSection xiphSection(const TagLib::Ogg::XiphComment& comment) {
    FileTagSection section{"Vorbis comment", {}};
    for (const auto& [key, values] : comment.fieldListMap()) add(section, utf8(key), utf8(key), utf8(values));
    return section;
}

// TagLib's property name for a key is found by asking a tag holding only that key.
FileTagSection apeSection(const TagLib::APE::Tag& tag) {
    FileTagSection section{"APEv2", {}};
    for (const auto& [name, item] : tag.itemListMap()) {
        if (item.type() == TagLib::APE::Item::Binary) continue;
        TagLib::APE::Tag single;
        single.setItem(name, item);
        const auto properties = single.properties();
        add(section, utf8(item.key()), properties.isEmpty() ? std::string{} : utf8(properties.begin()->first),
            utf8(item.values()));
    }
    return section;
}

FileTagSection infoSection(const TagLib::RIFF::Info::Tag& tag) {
    FileTagSection section{"RIFF INFO", {}};
    for (const auto& [id, value] : tag.fieldListMap()) {
        TagLib::RIFF::Info::Tag single;
        single.setFieldText(id, value);
        const auto properties = single.properties();
        add(section, std::string(id.data(), id.size()), properties.isEmpty() ? std::string{} : utf8(properties.begin()->first),
            {utf8(value)});
    }
    return section;
}

FileTagSection mp4Section(const TagLib::MP4::Tag& tag) {
    using Type = TagLib::MP4::Item::Type;
    FileTagSection section{"MP4", {}};
    for (const auto& [name, item] : tag.itemMap()) {
        Strings values;
        switch (item.type()) {
        case Type::StringList:
            values = utf8(item.toStringList());
            break;
        case Type::Bool:
            values = {item.toBool() ? "1" : "0"};
            break;
        case Type::Int:
            values = {std::to_string(item.toInt())};
            break;
        case Type::Byte:
            values = {std::to_string(item.toByte())};
            break;
        case Type::UInt:
            values = {std::to_string(item.toUInt())};
            break;
        case Type::LongLong:
            values = {std::to_string(item.toLongLong())};
            break;
        case Type::IntPair: {
            const auto pair = item.toIntPair();
            values = {pair.second ? std::to_string(pair.first) + "/" + std::to_string(pair.second) : std::to_string(pair.first)};
            break;
        }
        default: // pictures and binary data
            continue;
        }
        const auto field = TagLib::MP4::ItemFactory::instance()->propertyKeyForName(name.data(TagLib::String::Latin1));
        add(section, utf8(name), utf8(field), std::move(values));
    }
    return section;
}

FileTagSection diinSection(const TagLib::DSDIFF::DIIN::Tag& tag) {
    FileTagSection section{"DIIN", {}};
    add(section, "DITI", "TITLE", {utf8(tag.title())});
    add(section, "DIAR", "ARTIST", {utf8(tag.artist())});
    return section;
}

std::vector<FileTagSection> sectionsOf(TagLib::File* file) {
    std::vector<FileTagSection> out;
    const auto keep = [&](FileTagSection section) {
        if (!section.tags.empty()) out.push_back(std::move(section));
    };
    if (auto* flac = dynamic_cast<TagLib::FLAC::File*>(file)) {
        if (flac->hasXiphComment()) keep(xiphSection(*flac->xiphComment()));
        if (flac->hasID3v2Tag()) keep(id3v2Section(*flac->ID3v2Tag()));
        if (flac->hasID3v1Tag()) keep(id3v1Section(*flac->ID3v1Tag()));
    } else if (auto* mpeg = dynamic_cast<TagLib::MPEG::File*>(file)) {
        if (mpeg->hasID3v2Tag()) keep(id3v2Section(*mpeg->ID3v2Tag()));
        if (mpeg->hasAPETag()) keep(apeSection(*mpeg->APETag()));
        if (mpeg->hasID3v1Tag()) keep(id3v1Section(*mpeg->ID3v1Tag()));
    } else if (auto* mp4 = dynamic_cast<TagLib::MP4::File*>(file)) {
        if (mp4->hasMP4Tag()) keep(mp4Section(*mp4->tag()));
    } else if (auto* wav = dynamic_cast<TagLib::RIFF::WAV::File*>(file)) {
        if (wav->hasInfoTag()) keep(infoSection(*wav->InfoTag()));
        if (wav->hasID3v2Tag()) keep(id3v2Section(*wav->ID3v2Tag()));
    } else if (auto* aiff = dynamic_cast<TagLib::RIFF::AIFF::File*>(file)) {
        if (aiff->hasID3v2Tag()) keep(id3v2Section(*aiff->tag()));
    } else if (auto* dsf = dynamic_cast<TagLib::DSF::File*>(file)) {
        if (dsf->tag()) keep(id3v2Section(*dsf->tag()));
    } else if (auto* dff = dynamic_cast<TagLib::DSDIFF::File*>(file)) {
        if (dff->hasDIINTag()) keep(diinSection(*dff->DIINTag()));
        if (dff->hasID3v2Tag()) keep(id3v2Section(*dff->ID3v2Tag()));
    }
    return out;
}

// As the scanner reads the stream, by the same code.
void readStream(FileDetails& details, const std::filesystem::path& path) {
    const auto stream = readLibraryStream(path);
    const auto format = stream.format;
    const auto& info = stream.info;
    details.format = audio::formatName(format);
    details.sampleRate = info.sampleRate;
    details.bitsPerSample = audio::isLossy(format) ? 0 : info.bitsPerSample; // a lossy stream has no source depth
    details.channels = info.channels;
    details.frames = info.totalFrames;
    details.lengthMs = info.sampleRate ? info.totalFrames * 1000 / info.sampleRate : 0;
}

std::string systemMessage(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length =
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code,
                       0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text(buffer ? std::wstring_view(buffer, length) : std::wstring_view{});
    LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) text.pop_back();
    return text.empty() ? "error " + std::to_string(code) : toUtf8(text);
}

// What the library shows for a field, compared as the library reads it.
enum class Kind : uint8_t { Text, List, Number, Year, Gain, Peak };

struct LibraryValue {
    Kind kind = Kind::Text;
    Strings texts;
    int64_t number = 0;
    std::optional<double> real;
};

struct LibraryRow {
    int64_t file = 0;
    std::map<std::string, LibraryValue> fields; // by TagLib property name
    std::string format;
    uint32_t sampleRate = 0, bitsPerSample = 0, channels = 0;
    uint64_t frames = 0;
    std::optional<double> analysis[4];
};

std::optional<double> realOf(const sqlite::Statement& row, int column) {
    return row.isNull(column) ? std::nullopt : std::optional(row.real(column));
}

std::optional<int64_t> libraryFile(sqlite::Database& db, const std::string& pathUtf8) {
    auto find = db.prepare("SELECT id FROM files WHERE path=? AND kind='audio' ORDER BY available DESC LIMIT 1");
    const std::filesystem::path given(toWide(pathUtf8));
    if (find.bind(1, toUtf8(spelledLibraryPath(given).wstring())).step()) return find.integer(0);
    try {
        find.reset();
        if (find.bind(1, toUtf8(canonicalLibraryPath(given).wstring())).step()) return find.integer(0);
    } catch (const std::exception&) { // the file is not there to resolve
    }
    return std::nullopt;
}

std::optional<LibraryRow> libraryRow(sqlite::Database& db, int64_t file) {
    auto row = db.prepare("SELECT id,title,album,album_artist,title_sort_tag,album_sort_tag,artist_sort_tag,year,"
                          "track_number,track_total,disc_number,disc_total,rg_track_gain,rg_track_peak,rg_album_gain,"
                          "rg_album_peak,rg_analysis_gain,rg_analysis_peak,rg_analysis_album_gain,rg_analysis_album_peak,"
                          "coalesce(format,''),sample_rate,bits_per_sample,channels,total_frames FROM tracks WHERE file_id=?");
    if (!row.bind(1, file).step()) return std::nullopt;
    LibraryRow out;
    out.file = file;
    int column = 1;
    for (const char* field : {"TITLE", "ALBUM", "ALBUMARTIST", "TITLESORT", "ALBUMSORT", "ARTISTSORT"}) {
        out.fields[field] = {Kind::Text, {row.isNull(column) ? std::string{} : row.text(column)}, 0, {}};
        ++column;
    }
    out.fields["DATE"] = {Kind::Year, {}, row.integer(column++), {}};
    for (const char* field : {"TRACKNUMBER", "TRACKTOTAL", "DISCNUMBER", "DISCTOTAL"})
        out.fields[field] = {Kind::Number, {}, row.integer(column++), {}};
    for (const char* field :
         {"REPLAYGAIN_TRACK_GAIN", "REPLAYGAIN_TRACK_PEAK", "REPLAYGAIN_ALBUM_GAIN", "REPLAYGAIN_ALBUM_PEAK"}) {
        out.fields[field] = {std::string_view(field).ends_with("GAIN") ? Kind::Gain : Kind::Peak, {}, 0, realOf(row, column)};
        ++column;
    }
    for (auto& analysis : out.analysis) analysis = realOf(row, column++);
    out.format = row.text(column++);
    out.sampleRate = uint32_t(row.integer(column++));
    out.bitsPerSample = uint32_t(row.integer(column++));
    out.channels = uint32_t(row.integer(column++));
    out.frames = uint64_t(row.integer(column));
    const int64_t track = row.integer(0);
    out.fields["ARTIST"].kind = out.fields["COMPOSER"].kind = out.fields["GENRE"].kind = Kind::List;
    auto credits = db.prepare("SELECT upper(field),position,value FROM track_tags WHERE track_id=? UNION ALL "
                              "SELECT upper(field),position,value FROM track_values WHERE track_id=? AND field='genre' "
                              "ORDER BY 1,2");
    credits.bind(1, track).bind(2, track);
    while (credits.step()) out.fields[credits.text(0)].texts.push_back(credits.text(2));
    return out;
}

int64_t leadingNumber(const std::string& text) {
    int64_t value = 0;
    const auto start = text.find_first_not_of(" \t");
    if (start == std::string::npos) return 0;
    const auto parsed = std::from_chars(text.data() + start, text.data() + text.size(), value);
    return parsed.ec == std::errc{} ? std::max<int64_t>(0, value) : 0;
}

std::string shown(const LibraryValue& value) {
    char text[32];
    switch (value.kind) {
    case Kind::Text:
    case Kind::List: {
        std::string out;
        for (const auto& item : value.texts) out += (out.empty() ? "" : "; ") + item;
        return out;
    }
    case Kind::Number:
    case Kind::Year:
        return value.number ? std::to_string(value.number) : std::string{};
    case Kind::Gain:
        if (!value.real) return {};
        snprintf(text, sizeof(text), "%.2f dB", *value.real);
        return text;
    case Kind::Peak:
        if (!value.real) return {};
        snprintf(text, sizeof(text), "%.6f", *value.real);
        return text;
    }
    return {};
}

bool sameAs(const LibraryValue& library, const Strings& values) {
    const std::string& first = values.front();
    switch (library.kind) {
    case Kind::Text:
        return first == library.texts.front();
    case Kind::List: {
        Strings given;
        std::ranges::copy_if(values, std::back_inserter(given), [](const std::string& v) { return !v.empty(); });
        return given == library.texts;
    }
    case Kind::Number:
        return leadingNumber(first) == library.number;
    case Kind::Year: {
        int64_t year = leadingNumber(first);
        while (year > 9999) year /= 10;
        return year == library.number;
    }
    case Kind::Gain:
    case Kind::Peak: {
        char* end = nullptr;
        const double value = std::strtod(first.c_str(), &end);
        if (end == first.c_str()) return !library.real;
        return library.real && std::abs(value - *library.real) < (library.kind == Kind::Gain ? 0.005 : 0.0000005);
    }
    }
    return true;
}

void markDifferences(std::vector<FileTagSection>& sections, const LibraryRow& row) {
    for (auto& section : sections)
        for (auto& tag : section.tags)
            if (const auto library = row.fields.find(tag.field);
                library != row.fields.end() && !sameAs(library->second, tag.values))
                tag.library = shown(library->second);
}

std::vector<FileTag> libraryFields(const LibraryRow& row) {
    std::vector<FileTag> out;
    for (const char* field : {"TITLE", "ARTIST", "ALBUM", "ALBUMARTIST", "COMPOSER", "GENRE", "DATE", "TRACKNUMBER", "TRACKTOTAL",
                              "DISCNUMBER", "DISCTOTAL"}) {
        const auto& value = row.fields.at(field);
        Strings values = value.kind == Kind::List ? value.texts : Strings{shown(value)};
        std::erase_if(values, [](const std::string& v) { return v.empty(); });
        if (!values.empty()) out.push_back({field, field, std::move(values), std::nullopt});
    }
    return out;
}

} // namespace

FileDetails readFileDetails(const std::string& pathUtf8, sqlite::Database* db) {
    FileDetails details;
    const std::filesystem::path path(toWide(pathUtf8));
    std::optional<LibraryRow> row;
    if (db)
        if (const auto file = libraryFile(*db, pathUtf8)) {
            row = libraryRow(*db, *file);
            details.inLibrary = row.has_value();
            details.path = libraryStoredPath(*db, *file).value_or("");
        }
    if (details.path.empty()) details.path = libraryDisplayPath(pathUtf8);
    std::error_code error;
    details.folderPresent = std::filesystem::is_directory(path.parent_path(), error);
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) {
        const DWORD code = GetLastError();
        const bool gone = code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
        details.problem = gone ? FileDetails::Problem::Missing : FileDetails::Problem::Unreadable;
        details.reason = gone ? std::string{} : systemMessage(code);
    } else {
        details.size = (uint64_t(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
        const auto written =
            (uint64_t(attributes.ftLastWriteTime.dwHighDateTime) << 32) | attributes.ftLastWriteTime.dwLowDateTime;
        details.modified = int64_t(written / 10'000'000) - 11'644'473'600LL; // from 1601 in 100 ns steps
        try {
            readStream(details, path);
            TagLib::FileRef ref(path.c_str(), true);
            if (!ref.isNull()) {
                details.sections = sectionsOf(ref.file());
                const auto tags = ref.file()->properties();
                details.trackGain = libraryGain(tags, "REPLAYGAIN_TRACK_GAIN");
                details.trackPeak = libraryGain(tags, "REPLAYGAIN_TRACK_PEAK");
                details.albumGain = libraryGain(tags, "REPLAYGAIN_ALBUM_GAIN");
                details.albumPeak = libraryGain(tags, "REPLAYGAIN_ALBUM_PEAK");
                if (const auto* audio = ref.audioProperties()) details.bitrateKbps = uint32_t(std::max(0, audio->bitrate()));
            }
            if (!details.bitrateKbps && details.lengthMs) details.bitrateKbps = uint32_t(*details.size * 8 / details.lengthMs);
        } catch (const std::exception& e) {
            details.problem = FileDetails::Problem::Unreadable;
            details.reason = e.what();
            details.sections.clear();
        }
    }
    if (!row) return details;
    details.analysisTrackGain = row->analysis[0];
    details.analysisTrackPeak = row->analysis[1];
    details.analysisAlbumGain = row->analysis[2];
    details.analysisAlbumPeak = row->analysis[3];
    if (details.problem == FileDetails::Problem::None) {
        markDifferences(details.sections, *row);
        return details;
    }
    details.library = libraryFields(*row);
    details.sampleRate = row->sampleRate;
    details.bitsPerSample = row->bitsPerSample;
    if (audio::AudioFormat format{}; audio::formatFromId(row->format, format)) {
        details.format = audio::formatName(format);
        if (audio::isLossy(format)) details.bitsPerSample = 0;
    }
    details.channels = row->channels;
    details.frames = row->frames;
    details.lengthMs = row->sampleRate ? row->frames * 1000 / row->sampleRate : 0;
    details.trackGain = row->fields.at("REPLAYGAIN_TRACK_GAIN").real;
    details.trackPeak = row->fields.at("REPLAYGAIN_TRACK_PEAK").real;
    details.albumGain = row->fields.at("REPLAYGAIN_ALBUM_GAIN").real;
    details.albumPeak = row->fields.at("REPLAYGAIN_ALBUM_PEAK").real;
    return details;
}

} // namespace camel::server
