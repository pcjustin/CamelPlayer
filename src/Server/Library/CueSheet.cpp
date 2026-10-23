// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Library/CueSheet.h"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

#include "Common/util/Text.h"

namespace camel::server::library {
namespace {

std::string upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    return value;
}

std::string decode(std::string bytes, std::optional<uint32_t> codePage) {
    UINT page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (bytes.size() >= 3 && uint8_t(bytes[0]) == 0xef && uint8_t(bytes[1]) == 0xbb && uint8_t(bytes[2]) == 0xbf)
        bytes.erase(0, 3);
    else if (bytes.size() >= 2 && uint8_t(bytes[0]) == 0xff && uint8_t(bytes[1]) == 0xfe) {
        bytes.erase(0, 2);
        if (bytes.size() % 2) throw std::runtime_error("invalid UTF-16LE CUE");
        const int count = MultiByteToWideChar(1200, 0, bytes.data(), int(bytes.size()), nullptr, 0);
        if (!count && !bytes.empty()) throw std::runtime_error("invalid UTF-16LE CUE");
        std::wstring wide(count, L'\0');
        if (count) MultiByteToWideChar(1200, 0, bytes.data(), int(bytes.size()), wide.data(), count);
        return toUtf8(wide);
    } else if (bytes.size() >= 2 && uint8_t(bytes[0]) == 0xfe && uint8_t(bytes[1]) == 0xff) {
        bytes.erase(0, 2);
        if (bytes.size() % 2) throw std::runtime_error("invalid UTF-16BE CUE");
        std::string little(bytes.size(), '\0');
        for (size_t i = 0; i < bytes.size(); i += 2) {
            little[i] = bytes[i + 1];
            little[i + 1] = bytes[i];
        }
        const int count = MultiByteToWideChar(1200, 0, little.data(), int(little.size()), nullptr, 0);
        if (!count && !little.empty()) throw std::runtime_error("invalid UTF-16BE CUE");
        std::wstring wide(count, L'\0');
        if (count) MultiByteToWideChar(1200, 0, little.data(), int(little.size()), wide.data(), count);
        return toUtf8(wide);
    } else if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), int(bytes.size()), nullptr, 0)) {
        if (!codePage || !*codePage) throw std::runtime_error("CUE is not valid UTF-8; set a legacy encoding override");
        page = *codePage;
        flags = 0;
    }
    const int count = MultiByteToWideChar(page, flags, bytes.data(), int(bytes.size()), nullptr, 0);
    if (!count && !bytes.empty()) throw std::runtime_error("cannot decode CUE text");
    std::wstring wide(count, L'\0');
    if (count && !MultiByteToWideChar(page, flags, bytes.data(), int(bytes.size()), wide.data(), count))
        throw std::runtime_error("cannot decode CUE text");
    return toUtf8(wide);
}

std::string quoted(std::istringstream& line) {
    std::string value;
    line >> std::quoted(value);
    if (!line || value.empty()) throw std::runtime_error("invalid quoted CUE value");
    return value;
}

uint64_t time75(const std::string& value) {
    unsigned minutes = 0, seconds = 0, frames = 0;
    char colon1 = 0, colon2 = 0, extra = 0;
    std::istringstream in(value);
    if (!(in >> minutes >> colon1 >> seconds >> colon2 >> frames) || in >> extra || colon1 != ':' || colon2 != ':' ||
        seconds >= 60 || frames >= 75 || minutes > UINT64_MAX / 60)
        throw std::runtime_error("invalid CUE time");
    const uint64_t secondsTotal = uint64_t(minutes) * 60 + seconds;
    if (secondsTotal > (UINT64_MAX - frames) / 75) throw std::runtime_error("CUE time overflows");
    return secondsTotal * 75 + frames;
}

uint64_t read64(const uint8_t* bytes) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value = (value << 8) | bytes[i];
    return value;
}

} // namespace

CueSheet parseCue(const std::filesystem::path& path, std::optional<uint32_t> codePage) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open CUE file");
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    if (bytes.size() > 1024 * 1024) throw std::runtime_error("CUE file exceeds 1 MiB");
    const auto text = decode(std::move(bytes), codePage);
    CueSheet sheet;
    CueTrack* currentTrack = nullptr;
    CueFile* currentFile = nullptr;
    std::istringstream lines(text);
    std::string source;
    std::set<int> numbers;
    while (std::getline(lines, source)) {
        std::istringstream line(source);
        std::string command;
        if (!(line >> command)) continue;
        command = upper(command);
        if (command == "REM") {
            std::string key, value;
            if (!(line >> key)) continue;
            std::getline(line, value);
            const auto first = value.find_first_not_of(" \t\r");
            if (first == std::string::npos) continue;
            value.erase(0, first);
            if (value.front() == '"') {
                std::istringstream rest(value);
                value = quoted(rest);
            }
            key = upper(key);
            if (key == "DATE" && !currentTrack) {
                int year = 0;
                auto parsed = std::from_chars(value.data(), value.data() + value.size(), year);
                if (parsed.ec == std::errc{}) sheet.year = year;
            }
            continue;
        }
        if (command == "FILE") {
            CueFile file;
            file.path = quoted(line);
            sheet.files.push_back(std::move(file));
            currentFile = &sheet.files.back();
            currentTrack = nullptr;
        } else if (command == "TRACK") {
            int number = 0;
            std::string type;
            if (!currentFile || !(line >> number >> type) || number < 1 || number > 99 || upper(type) != "AUDIO" ||
                !numbers.insert(number).second)
                throw std::runtime_error("invalid or unsupported CUE track");
            currentFile->tracks.push_back({});
            currentTrack = &currentFile->tracks.back();
            currentTrack->number = number;
        } else if (command == "INDEX") {
            int index = -1;
            std::string timestamp;
            if (!currentTrack || !(line >> index >> timestamp) || index < 0 || index > 99)
                throw std::runtime_error("invalid CUE index");
            const auto frame = time75(timestamp);
            if (index == 0 || index == 1) {
                auto& target = index == 0 ? currentTrack->index00 : currentTrack->index01;
                if (target) throw std::runtime_error("duplicate CUE index");
                target = frame;
            }
        } else if (command == "PREGAP" || command == "POSTGAP") {
            std::string timestamp;
            if (!currentTrack || !(line >> timestamp)) throw std::runtime_error("invalid CUE gap");
            (command == "PREGAP" ? currentTrack->pregap75 : currentTrack->postgap75) = time75(timestamp);
        } else if (command == "TITLE" || command == "PERFORMER") {
            const auto value = quoted(line);
            if (currentTrack)
                (command == "TITLE" ? currentTrack->title : currentTrack->performer) = value;
            else if (command == "TITLE")
                sheet.title = value;
            else
                sheet.performer = value;
        }
    }
    if (sheet.files.empty() || numbers.empty()) throw std::runtime_error("CUE has no audio tracks");
    for (const auto& file : sheet.files) {
        if (file.tracks.empty()) throw std::runtime_error("CUE FILE has no audio tracks");
        for (const auto& track : file.tracks)
            if (!track.index01 || (track.index00 && *track.index00 > *track.index01))
                throw std::runtime_error("CUE track has an invalid INDEX 01 range");
    }
    return sheet;
}

CueSheet parseEmbeddedFlacCue(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    char magic[4]{};
    input.read(magic, 4);
    if (!input || std::string_view(magic, 4) != "fLaC") return {};
    bool last = false;
    while (!last) {
        uint8_t header[4]{};
        input.read(reinterpret_cast<char*>(header), 4);
        if (!input) throw std::runtime_error("truncated FLAC metadata header");
        last = (header[0] & 0x80) != 0;
        const auto type = header[0] & 0x7f;
        const uint32_t length = (uint32_t(header[1]) << 16) | (uint32_t(header[2]) << 8) | header[3];
        if (length > 16 * 1024 * 1024) throw std::runtime_error("oversized FLAC metadata block");
        if (type != 5) {
            input.seekg(length, std::ios::cur);
            if (!input) throw std::runtime_error("truncated FLAC metadata block");
            continue;
        }
        std::vector<uint8_t> block(length);
        input.read(reinterpret_cast<char*>(block.data()), length);
        if (!input || length < 396) throw std::runtime_error("invalid FLAC CUESHEET block");
        const uint8_t trackCount = block[395];
        size_t offset = 396;
        CueSheet sheet;
        sheet.offsetsAreSamples = true;
        sheet.files.push_back({path.filename().string(), {}});
        std::set<int> trackNumbers;
        for (uint8_t i = 0; i < trackCount; ++i) {
            if (offset + 36 > block.size()) throw std::runtime_error("truncated FLAC CUESHEET track");
            const uint64_t trackOffset = read64(block.data() + offset);
            const int number = block[offset + 8];
            const uint8_t indices = block[offset + 35];
            offset += 36;
            if (offset + size_t(indices) * 12 > block.size()) throw std::runtime_error("truncated FLAC CUESHEET index");
            uint64_t index01 = trackOffset;
            bool hasIndex01 = false;
            for (uint8_t j = 0; j < indices; ++j) {
                const uint64_t indexOffset = read64(block.data() + offset);
                const int indexNumber = block[offset + 8];
                if (indexNumber == 1) {
                    if (indexOffset > UINT64_MAX - trackOffset) throw std::runtime_error("FLAC CUESHEET offset overflow");
                    index01 = trackOffset + indexOffset;
                    hasIndex01 = true;
                }
                offset += 12;
            }
            if (number >= 1 && number <= 99) {
                if (!hasIndex01 || !trackNumbers.insert(number).second)
                    throw std::runtime_error("invalid FLAC CUESHEET track index");
                CueTrack track;
                track.number = number;
                track.index01 = index01;
                sheet.files.front().tracks.push_back(std::move(track));
            }
        }
        if (!sheet.files.front().tracks.empty()) return sheet;
        return {};
    }
    return {};
}

} // namespace camel::server::library
