// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace camel::server::library {

struct CueTrack {
    int number = 0;
    std::string title;
    std::string performer;
    std::optional<uint64_t> index00;
    std::optional<uint64_t> index01;
    uint64_t pregap75 = 0;
    uint64_t postgap75 = 0;
};

struct CueFile {
    std::string path;
    std::vector<CueTrack> tracks;
};

struct CueSheet {
    std::string title;
    std::string performer;
    int year = 0;
    std::vector<CueFile> files;
    bool offsetsAreSamples = false;
};

CueSheet parseCue(const std::filesystem::path& path, std::optional<uint32_t> codePage = std::nullopt);
CueSheet parseEmbeddedFlacCue(const std::filesystem::path& path);

} // namespace camel::server::library
