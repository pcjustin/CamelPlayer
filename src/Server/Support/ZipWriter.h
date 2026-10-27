// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Writes a deflate-compressed zip file (no zip64: every entry and the whole file stay under 4 GB).
#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace camel::server {

class ZipWriter {
public:
    explicit ZipWriter(const std::filesystem::path& file); // throws std::runtime_error
    // `name` is a UTF-8 path inside the zip with forward slashes.
    void add(std::string_view name, std::string_view data);
    void finish(); // writes the central directory and closes the file

private:
    struct Entry {
        std::string name;
        uint32_t crc = 0, compressed = 0, size = 0, offset = 0;
    };
    std::ofstream out_;
    std::vector<Entry> entries_;
    uint16_t time_ = 0, date_ = 0;
};

} // namespace camel::server
