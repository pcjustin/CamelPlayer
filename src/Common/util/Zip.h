// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Reading .zip archives held in memory: stored and deflated entries, each checked against its CRC-32; no zip64.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace camel {

struct ZipEntry {
    std::string name;  // as stored, folders separated by '/'
    bool utf8 = false; // the name is UTF-8, else code page 437
    bool directory = false;
    std::string data;
};

// An entry of an archive, its data not read yet.
struct ZipItem {
    std::string name;
    bool utf8 = false;
    bool directory = false;
    uint32_t method = 0, crc = 0, compressed = 0, size = 0;
    size_t start = 0; // where its data begins in the archive
};

// Every entry of the archive `data`; throws std::runtime_error naming what is wrong, also for an entry that says it
// holds more than its data can, or more than 1 GB.
std::vector<ZipEntry> readZip(const std::string& data);

// The entries of the archive `data` with the same checks, short of reading their data, which zipData() reads one
// entry at a time.
std::vector<ZipItem> listZip(const std::string& data);
std::string zipData(const std::string& data, const ZipItem& item);

} // namespace camel
