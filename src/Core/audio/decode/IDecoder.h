// Decoder interface (docs/計劃書.md §16.3). The final player keeps this interface for every format.
#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

#include "audio/AudioTypes.h"

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

enum class DecoderKind : uint8_t { FFmpeg, Symphonia, DrFlac, Sndfile };
constexpr DecoderKind kAllDecoders[] = {DecoderKind::FFmpeg, DecoderKind::Symphonia, DecoderKind::DrFlac, DecoderKind::Sndfile};

const char* decoderName(DecoderKind k);         // display name, e.g. "dr_flac"
const char* decoderId(DecoderKind k);           // settings / CLI id, e.g. "drflac"
bool decoderFromId(std::string_view id, DecoderKind& out);

// Opens `path` with the chosen decoder. Throws Error (FileOpen / UnsupportedStream / DecodeFailed).
std::unique_ptr<IDecoder> openDecoder(DecoderKind kind, const std::filesystem::path& path);

}  // namespace camel::audio
