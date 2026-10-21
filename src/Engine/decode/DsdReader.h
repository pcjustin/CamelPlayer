// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Raw DSD from DSF and DSDIFF files through DSD Nexus libdsf/libdsdiff, with DST decompressed by libdst
// (docs/Plan.md §5.8). Not yet a playback decoder: DSD output arrives with DoP/native packing (M2-R06).
#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace camel::audio {

struct DsdStreamInfo {
    uint32_t channels = 0;
    uint32_t sampleRate = 0; // DSD bits per second per channel, e.g. 2822400 for DSD64
    uint64_t frames = 0;     // one byte per channel per frame (8 DSD samples); whole valid bytes only
    bool dst = false;
};

class DsdReader {
public:
    virtual ~DsdReader() = default;

    virtual const DsdStreamInfo& info() const = 0;

    // Reads up to `frames` frames into `dst` (frames * channels bytes): byte-interleaved by channel, the oldest sample
    // in the most significant bit. Returns the number of frames read; 0 means end of stream.
    // Throws Error (DecodeFailed) on truncated or corrupt data.
    virtual size_t read(uint8_t* dst, size_t frames) = 0;

    // Positions the stream so the next read() starts exactly at `frame`. Throws Error (SeekFailed).
    virtual void seek(uint64_t frame) = 0;
};

// DST is decompressed only with `allowDst`; support stays gated until DST has an independent raw reference.
// Throws Error (FileOpen / UnsupportedStream / DecodeFailed).
std::unique_ptr<DsdReader> openDsdReader(const std::filesystem::path& path, bool allowDst = false);

} // namespace camel::audio
