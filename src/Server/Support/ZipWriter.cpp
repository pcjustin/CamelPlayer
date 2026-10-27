// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Support/ZipWriter.h"

#include <windows.h>
#include <zlib.h>

#include <limits>
#include <stdexcept>

namespace camel::server {

namespace {

void put16(std::string& out, uint32_t v) {
    out += char(v & 0xff);
    out += char((v >> 8) & 0xff);
}

void put32(std::string& out, uint32_t v) {
    put16(out, v & 0xffff);
    put16(out, v >> 16);
}

std::string deflateRaw(std::string_view data) {
    z_stream z{};
    if (deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        throw std::runtime_error("zip: deflate init failed");
    std::string out(deflateBound(&z, uLong(data.size())), '\0');
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    z.avail_in = uInt(data.size());
    z.next_out = reinterpret_cast<Bytef*>(out.data());
    z.avail_out = uInt(out.size());
    const int result = deflate(&z, Z_FINISH);
    out.resize(z.total_out);
    deflateEnd(&z);
    if (result != Z_STREAM_END) throw std::runtime_error("zip: deflate failed");
    return out;
}

} // namespace

ZipWriter::ZipWriter(const std::filesystem::path& file) : out_(file, std::ios::binary | std::ios::trunc) {
    if (!out_) throw std::runtime_error("zip: cannot create the file");
    SYSTEMTIME t;
    GetLocalTime(&t);
    time_ = uint16_t((t.wHour << 11) | (t.wMinute << 5) | (t.wSecond / 2));
    date_ = uint16_t(((t.wYear - 1980) << 9) | (t.wMonth << 5) | t.wDay);
}

void ZipWriter::add(std::string_view name, std::string_view data) {
    constexpr auto kLimit = std::numeric_limits<uint32_t>::max();
    const auto offset = uint64_t(out_.tellp());
    if (data.size() >= kLimit || offset >= kLimit) throw std::runtime_error("zip: entry too large");
    const std::string compressed = deflateRaw(data);
    Entry entry{std::string(name), uint32_t(crc32(0, reinterpret_cast<const Bytef*>(data.data()), uInt(data.size()))),
                uint32_t(compressed.size()), uint32_t(data.size()), uint32_t(offset)};
    std::string header;
    put32(header, 0x04034b50);
    put16(header, 20);      // version needed: deflate
    put16(header, 1 << 11); // UTF-8 names
    put16(header, 8);       // deflate
    put16(header, time_);
    put16(header, date_);
    put32(header, entry.crc);
    put32(header, entry.compressed);
    put32(header, entry.size);
    put16(header, uint32_t(entry.name.size()));
    put16(header, 0);
    header += entry.name;
    out_.write(header.data(), std::streamsize(header.size()));
    out_.write(compressed.data(), std::streamsize(compressed.size()));
    if (!out_) throw std::runtime_error("zip: writing failed");
    entries_.push_back(std::move(entry));
}

void ZipWriter::finish() {
    const auto start = uint32_t(out_.tellp());
    std::string directory;
    for (const auto& entry : entries_) {
        put32(directory, 0x02014b50);
        put16(directory, 20); // made by: MS-DOS, 2.0
        put16(directory, 20);
        put16(directory, 1 << 11);
        put16(directory, 8);
        put16(directory, time_);
        put16(directory, date_);
        put32(directory, entry.crc);
        put32(directory, entry.compressed);
        put32(directory, entry.size);
        put16(directory, uint32_t(entry.name.size()));
        put16(directory, 0); // extra
        put16(directory, 0); // comment
        put16(directory, 0); // disk
        put16(directory, 0); // internal attributes
        put32(directory, 0); // external attributes
        put32(directory, entry.offset);
        directory += entry.name;
    }
    const auto size = uint32_t(directory.size());
    put32(directory, 0x06054b50);
    put16(directory, 0);
    put16(directory, 0);
    put16(directory, uint32_t(entries_.size()));
    put16(directory, uint32_t(entries_.size()));
    put32(directory, size);
    put32(directory, start);
    put16(directory, 0);
    out_.write(directory.data(), std::streamsize(directory.size()));
    out_.close();
    if (!out_) throw std::runtime_error("zip: writing failed");
}

} // namespace camel::server
