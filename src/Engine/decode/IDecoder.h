// Decoder interface (docs/Plan.md §16.3). The final player keeps this interface for every format.
#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

#include "Common/AudioTypes.h"
#include "Common/DecoderKind.h"

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
};

// Opens `path` with the chosen decoder. Throws Error (FileOpen / UnsupportedStream / DecodeFailed).
std::unique_ptr<IDecoder> openDecoder(DecoderKind kind, const std::filesystem::path& path);

} // namespace camel::audio
