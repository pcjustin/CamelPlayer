// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Common/util/Zip.h"

#include <zlib.h>

#include <algorithm>
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

constexpr uint64_t kMaxDeflateRatio = 1032;    // the most deflate packs into one byte
constexpr uint32_t kMaxEntryBytes = 1u << 30;  // far above an impulse response or a release's largest file
constexpr size_t kFirstOutputBytes = 1u << 16; // the output grows from this as it inflates

// The entry's data, inflated: never more than `size`, which it must reach exactly.
std::string inflateEntry(const char* data, uint32_t compressed, uint32_t size, const std::string& name) {
    const auto fail = [&] { return std::runtime_error("zip: cannot inflate " + name); };
    z_stream z{};
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) throw fail();
    std::string out(std::max<size_t>(1, std::min<size_t>(size, kFirstOutputBytes)), '\0');
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data));
    z.avail_in = compressed;
    z.next_out = reinterpret_cast<Bytef*>(out.data());
    z.avail_out = uInt(out.size());
    int result = Z_OK;
    for (;;) {
        result = inflate(&z, Z_NO_FLUSH);
        if (result == Z_STREAM_END || (result != Z_OK && result != Z_BUF_ERROR)) break;
        if (z.avail_out > 0 || out.size() >= size) break; // the data ended early, or holds more than it says
        const size_t done = out.size();
        out.resize(std::min<size_t>(size, done * 2));
        z.next_out = reinterpret_cast<Bytef*>(out.data() + done);
        z.avail_out = uInt(out.size() - done);
    }
    const auto produced = z.total_out;
    inflateEnd(&z);
    if (result != Z_STREAM_END || produced != size) throw fail();
    out.resize(size);
    return out;
}

// Each entry in the central directory's order, checked as far as its data is not read, handed to `each`.
template <class Each> void walk(const std::string& data, Each&& each) {
    if (data.size() < 22) throw std::runtime_error("zip: not a zip file");
    size_t end = data.size() - 22;
    const size_t lowest = data.size() > 22 + 65535 ? data.size() - 22 - 65535 : 0;
    while (read32(data, end) != 0x06054b50) {
        if (end == lowest) throw std::runtime_error("zip: no central directory");
        --end;
    }
    const uint32_t count = read16(data, end + 10);
    size_t at = read32(data, end + 16);
    for (uint32_t i = 0; i < count; ++i) {
        if (read32(data, at) != 0x02014b50) throw std::runtime_error("zip: damaged central directory");
        const uint32_t flags = read16(data, at + 8);
        ZipItem item;
        item.method = read16(data, at + 10);
        item.crc = read32(data, at + 16);
        item.compressed = read32(data, at + 20);
        item.size = read32(data, at + 24);
        const uint32_t nameLength = read16(data, at + 28), offset = read32(data, at + 42);
        if (item.compressed == 0xffffffff || item.size == 0xffffffff || offset == 0xffffffff)
            throw std::runtime_error("zip: zip64 entries are not supported");
        if (at + 46 + nameLength > data.size()) throw std::runtime_error("zip: truncated");
        item.name = data.substr(at + 46, nameLength);
        item.utf8 = (flags & (1 << 11)) != 0;
        at += 46 + nameLength + read16(data, at + 30) + read16(data, at + 32);
        if (item.name.ends_with('/') || item.name.ends_with('\\')) {
            item.directory = true;
            each(std::move(item));
            continue;
        }
        if (read32(data, offset) != 0x04034b50) throw std::runtime_error("zip: damaged entry " + item.name);
        item.start = offset + 30 + read16(data, offset + 26) + read16(data, offset + 28);
        if (item.start + item.compressed > data.size()) throw std::runtime_error("zip: truncated entry " + item.name);
        if (item.size > kMaxEntryBytes || (item.method == 8 && item.size > item.compressed * kMaxDeflateRatio))
            throw std::runtime_error("zip: " + item.name + " says it holds more than it can");
        if (item.method != 0 && item.method != 8) throw std::runtime_error("zip: unsupported compression in " + item.name);
        each(std::move(item));
    }
}

} // namespace

std::vector<ZipItem> listZip(const std::string& data) {
    std::vector<ZipItem> items;
    walk(data, [&](ZipItem&& item) { items.push_back(std::move(item)); });
    return items;
}

std::string zipData(const std::string& data, const ZipItem& item) {
    if (item.directory) return {};
    std::string out = item.method == 0 ? data.substr(item.start, item.compressed)
                                       : inflateEntry(data.data() + item.start, item.compressed, item.size, item.name);
    if (out.size() != item.size || crc32(0, reinterpret_cast<const Bytef*>(out.data()), uInt(out.size())) != item.crc)
        throw std::runtime_error("zip: checksum mismatch in " + item.name);
    return out;
}

std::vector<ZipEntry> readZip(const std::string& data) {
    std::vector<ZipEntry> entries;
    walk(data, [&](ZipItem&& item) {
        ZipEntry entry;
        entry.data = zipData(data, item);
        entry.name = std::move(item.name);
        entry.utf8 = item.utf8;
        entry.directory = item.directory;
        entries.push_back(std::move(entry));
    });
    return entries;
}

} // namespace camel
