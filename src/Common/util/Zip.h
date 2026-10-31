// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Reading .zip archives held in memory: stored and deflated entries, each checked against its CRC-32; no zip64.
#pragma once
#include <string>
#include <vector>

namespace camel {

struct ZipEntry {
    std::string name;  // as stored, folders separated by '/'
    bool utf8 = false; // the name is UTF-8, else code page 437
    bool directory = false;
    std::string data;
};

// Every entry of the archive `data`; throws std::runtime_error naming what is wrong.
std::vector<ZipEntry> readZip(const std::string& data);

} // namespace camel
