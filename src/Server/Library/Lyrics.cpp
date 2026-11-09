// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Library/Lyrics.h"

#include <windows.h>

#include <taglib/aifffile.h>
#include <taglib/dsdifffile.h>
#include <taglib/dsffile.h>
#include <taglib/fileref.h>
#include <taglib/id3v2tag.h>
#include <taglib/mpegfile.h>
#include <taglib/synchronizedlyricsframe.h>
#include <taglib/tpropertymap.h>
#include <taglib/wavfile.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <vector>

#include "Common/util/Text.h"

namespace camel::server {

namespace {

std::string_view trimmed(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

std::optional<uint32_t> number(std::string_view digits) {
    if (digits.empty() || digits.size() > 6) return std::nullopt;
    uint32_t value = 0;
    for (const char c : digits) {
        if (c < '0' || c > '9') return std::nullopt;
        value = value * 10 + uint32_t(c - '0');
    }
    return value;
}

// "mm:ss", "mm:ss.xx", "mm:ss.xxx" or "mm:ss:xx", in milliseconds.
std::optional<uint32_t> stamp(std::string_view s) {
    const size_t colon = s.find(':');
    if (colon == std::string_view::npos) return std::nullopt;
    const auto minutes = number(s.substr(0, colon));
    std::string_view rest = s.substr(colon + 1);
    const size_t dot = rest.find_first_of(".:");
    const auto seconds = number(rest.substr(0, dot));
    if (!minutes || !seconds || *seconds >= 60) return std::nullopt;
    uint32_t ms = (*minutes * 60 + *seconds) * 1000;
    if (dot != std::string_view::npos) {
        const std::string_view fraction = rest.substr(dot + 1);
        const auto part = number(fraction);
        if (!part || fraction.size() > 3) return std::nullopt;
        ms += fraction.size() == 1 ? *part * 100 : fraction.size() == 2 ? *part * 10 : *part;
    }
    return ms;
}

bool isTag(std::string_view s) {
    const size_t colon = s.find(':');
    return colon != std::string_view::npos && colon > 0 &&
           std::all_of(s.begin(), s.begin() + std::ptrdiff_t(colon), [](char c) { return std::isalpha(uint8_t(c)) != 0; });
}

void add(std::vector<Lyrics>& found, Lyrics lyrics) {
    if (!lyrics.lines.empty()) found.push_back(std::move(lyrics));
}

// A .lrc in UTF-8 or UTF-16 with a byte order mark, else UTF-8, else the system's code page.
std::string readText(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (bytes.starts_with("\xEF\xBB\xBF")) return bytes.substr(3);
    if (bytes.size() >= 2 && uint8_t(bytes[0]) == 0xFF && uint8_t(bytes[1]) == 0xFE) {
        std::wstring wide((bytes.size() - 2) / 2, L'\0');
        memcpy(wide.data(), bytes.data() + 2, wide.size() * 2);
        return toUtf8(wide);
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), int(bytes.size()), nullptr, 0) > 0 || bytes.empty())
        return bytes;
    const int length = MultiByteToWideChar(CP_ACP, 0, bytes.data(), int(bytes.size()), nullptr, 0);
    std::wstring wide(size_t(std::max(length, 0)), L'\0');
    MultiByteToWideChar(CP_ACP, 0, bytes.data(), int(bytes.size()), wide.data(), length);
    return toUtf8(wide);
}

TagLib::ID3v2::Tag* id3Of(TagLib::File* file) {
    if (auto* mpeg = dynamic_cast<TagLib::MPEG::File*>(file)) return mpeg->ID3v2Tag();
    if (auto* wav = dynamic_cast<TagLib::RIFF::WAV::File*>(file)) return wav->ID3v2Tag();
    if (auto* aiff = dynamic_cast<TagLib::RIFF::AIFF::File*>(file)) return aiff->tag();
    if (auto* dsf = dynamic_cast<TagLib::DSF::File*>(file)) return dsf->tag();
    if (auto* dff = dynamic_cast<TagLib::DSDIFF::File*>(file)) return dff->ID3v2Tag();
    return nullptr;
}

// Synced lyrics in an ID3 SYLT frame, when timed in milliseconds.
Lyrics synchronizedFrame(const TagLib::ID3v2::SynchronizedLyricsFrame& frame) {
    Lyrics lyrics;
    if (frame.timestampFormat() != TagLib::ID3v2::SynchronizedLyricsFrame::AbsoluteMilliseconds) return lyrics;
    lyrics.synced = true;
    for (const auto& text : frame.synchedText()) {
        std::string line(trimmed(text.text.to8Bit(true)));
        if (!line.empty()) lyrics.lines.push_back({text.time, std::move(line)});
    }
    std::ranges::stable_sort(lyrics.lines, {}, [](const LyricLine& l) { return *l.ms; });
    return lyrics;
}

std::vector<Lyrics> embedded(const std::filesystem::path& file) {
    std::vector<Lyrics> found;
    TagLib::FileRef ref(file.c_str(), false);
    if (ref.isNull()) return found;
    const auto properties = ref.file()->properties();
    for (const char* key : {"LYRICS", "UNSYNCEDLYRICS"})
        if (const auto values = properties.find(key); values != properties.end())
            for (const auto& value : values->second) add(found, parseLrc(value.to8Bit(true)));
    if (auto* id3 = id3Of(ref.file()); id3 && id3->frameListMap().contains("SYLT"))
        for (auto* frame : id3->frameListMap()["SYLT"])
            if (const auto* sylt = dynamic_cast<TagLib::ID3v2::SynchronizedLyricsFrame*>(frame))
                add(found, synchronizedFrame(*sylt));
    return found;
}

} // namespace

Lyrics parseLrc(std::string_view text) {
    Lyrics synced{true, {}}, plain;
    int32_t offset = 0;
    bool anyStamp = false;
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    while (!text.empty()) {
        const size_t end = text.find('\n');
        std::string_view line = trimmed(text.substr(0, end));
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        std::vector<uint32_t> stamps;
        bool tag = false;
        while (line.starts_with('[')) {
            const size_t close = line.find(']');
            if (close == std::string_view::npos) break;
            const std::string_view inside = line.substr(1, close - 1);
            if (const auto at = stamp(inside)) {
                stamps.push_back(*at);
            } else if (isTag(inside)) {
                tag = true;
                if (inside.starts_with("offset:")) {
                    std::string_view value = trimmed(inside.substr(7));
                    const bool negative = value.starts_with('-');
                    if (negative || value.starts_with('+')) value.remove_prefix(1);
                    if (const auto ms = number(value)) offset = negative ? -int32_t(*ms) : int32_t(*ms);
                }
            } else {
                break;
            }
            line = trimmed(line.substr(close + 1));
        }
        if (!stamps.empty()) {
            anyStamp = true;
            if (!line.empty())
                for (const uint32_t at : stamps) synced.lines.push_back({at, std::string(line)});
        } else if (!tag && !line.empty()) {
            plain.lines.push_back({std::nullopt, std::string(line)});
        }
    }
    if (!anyStamp) return plain;
    // A positive offset shows the lyrics sooner.
    for (auto& line : synced.lines) line.ms = uint32_t(std::max<int64_t>(0, int64_t(*line.ms) - offset));
    std::ranges::stable_sort(synced.lines, {}, [](const LyricLine& l) { return *l.ms; });
    return synced;
}

Lyrics readLyrics(const std::filesystem::path& file) {
    std::vector<Lyrics> found;
    try {
        auto lrc = file;
        lrc.replace_extension(L".lrc");
        std::error_code error;
        if (std::filesystem::is_regular_file(lrc, error)) add(found, parseLrc(readText(lrc)));
        const auto inFile = embedded(file);
        found.insert(found.end(), inFile.begin(), inFile.end());
    } catch (const std::exception&) {
        return {};
    }
    const auto synced = std::ranges::find_if(found, &Lyrics::synced);
    if (synced != found.end()) return *synced;
    return found.empty() ? Lyrics{} : found.front();
}

std::optional<size_t> lyricLineAt(const Lyrics& lyrics, uint32_t ms) {
    if (!lyrics.synced) return std::nullopt;
    const auto after = std::ranges::upper_bound(lyrics.lines, ms, {}, [](const LyricLine& l) { return *l.ms; });
    if (after == lyrics.lines.begin()) return std::nullopt;
    const uint32_t at = *std::prev(after)->ms;
    return size_t(std::ranges::lower_bound(lyrics.lines, at, {}, [](const LyricLine& l) { return *l.ms; }) -
                  lyrics.lines.begin());
}

} // namespace camel::server
