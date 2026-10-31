// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Common/util/Zip.h"

#include <zlib.h>

#include <cstdint>
#include <stdexcept>

namespace camel {
namespace {

uint32_t read16(const std::string& s, size_t at) {
    if (at + 2 > s.size()) throw std::runtime_error("zip: truncated");
    return uint8_t(s[at]) | (uint32_t(uint8_t(s[at + 1])) << 8);
}

uint32_t read32(const std::string& s, size_t at) {
    return read16(s, at) | (read16(s, at + 2) << 16);
}

} // namespace

std::vector<ZipEntry> readZip(const std::string& data) {
    if (data.size() < 22) throw std::runtime_error("zip: not a zip file");
    size_t end = data.size() - 22;
    const size_t lowest = data.size() > 22 + 65535 ? data.size() - 22 - 65535 : 0;
    while (read32(data, end) != 0x06054b50) {
        if (end == lowest) throw std::runtime_error("zip: no central directory");
        --end;
    }
    const uint32_t count = read16(data, end + 10);
    size_t at = read32(data, end + 16);
    std::vector<ZipEntry> entries;
    for (uint32_t i = 0; i < count; ++i) {
        if (read32(data, at) != 0x02014b50) throw std::runtime_error("zip: damaged central directory");
        const uint32_t flags = read16(data, at + 8), method = read16(data, at + 10), crc = read32(data, at + 16);
        const uint32_t compressed = read32(data, at + 20), size = read32(data, at + 24);
        const uint32_t nameLength = read16(data, at + 28), offset = read32(data, at + 42);
        if (compressed == 0xffffffff || size == 0xffffffff || offset == 0xffffffff)
            throw std::runtime_error("zip: zip64 entries are not supported");
        if (at + 46 + nameLength > data.size()) throw std::runtime_error("zip: truncated");
        ZipEntry entry;
        entry.name = data.substr(at + 46, nameLength);
        entry.utf8 = (flags & (1 << 11)) != 0;
        at += 46 + nameLength + read16(data, at + 30) + read16(data, at + 32);
        if (entry.name.ends_with('/') || entry.name.ends_with('\\')) {
            entry.directory = true;
            entries.push_back(std::move(entry));
            continue;
        }
        if (read32(data, offset) != 0x04034b50) throw std::runtime_error("zip: damaged entry " + entry.name);
        const size_t start = offset + 30 + read16(data, offset + 26) + read16(data, offset + 28);
        if (start + compressed > data.size()) throw std::runtime_error("zip: truncated entry " + entry.name);
        if (method == 0) {
            entry.data = data.substr(start, compressed);
        } else if (method == 8) {
            entry.data.resize(size);
            z_stream z{};
            inflateInit2(&z, -MAX_WBITS);
            z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data() + start));
            z.avail_in = compressed;
            z.next_out = reinterpret_cast<Bytef*>(entry.data.data());
            z.avail_out = size;
            const int result = inflate(&z, Z_FINISH);
            inflateEnd(&z);
            if (result != Z_STREAM_END || z.total_out != size) throw std::runtime_error("zip: cannot inflate " + entry.name);
        } else {
            throw std::runtime_error("zip: unsupported compression in " + entry.name);
        }
        if (entry.data.size() != size ||
            crc32(0, reinterpret_cast<const Bytef*>(entry.data.data()), uInt(entry.data.size())) != crc)
            throw std::runtime_error("zip: checksum mismatch in " + entry.name);
        entries.push_back(std::move(entry));
    }
    return entries;
}

} // namespace camel
