// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Decoder interface (docs/Plan.md §16.3). The final player keeps this interface for every format.
#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

#include "Common/AudioTypes.h"
#include "Common/DecoderKind.h"
#include "Common/settings/Settings.h"

namespace camel::audio {

class IDecoder {
public:
    virtual ~IDecoder() = default;

    virtual const SourceInfo& info() const = 0;

    // Reads up to `frames` frames into `dst` (frames * channels values), interleaved and right-aligned
    // to info().bitsPerSample. Returns the number of frames read; 0 means end of stream.
    // Throws camel::audio::Error (DecodeFailed) on corrupt data.
    virtual size_t read(int32_t* dst, size_t frames) = 0;

    // Positions the stream so the next read() starts exactly at `frame`. Throws Error (SeekFailed).
    virtual void seek(uint64_t frame) = 0;

    // Lossy decoders also offer their float output (full scale +-1), which the Engine quantizes once with dither;
    // read() returns the same samples rounded to info().bitsPerSample. Mixing the two reads is allowed.
    virtual bool readsFloat() const { return false; }
    virtual size_t readFloat(float* dst, size_t frames);
};

// Opens `path` with the chosen decoder. Lossy formats decode to the depth `pcmBits` sets for them (24 by default).
// Throws Error (FileOpen / UnsupportedStream / DecodeFailed).
std::unique_ptr<IDecoder> openDecoder(DecoderKind kind, const std::filesystem::path& path, const PcmBitsByFormat& pcmBits = {});
// The same for any decoder in decoderRegistry() by its ID (docs/Extension-points.md #1).
std::unique_ptr<IDecoder> openDecoder(std::string_view decoder, const std::filesystem::path& path,
                                      const PcmBitsByFormat& pcmBits = {});

} // namespace camel::audio
