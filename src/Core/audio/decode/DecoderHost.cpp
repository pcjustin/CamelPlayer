#include "audio/decode/DecoderHost.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

#include "audio/decode/IDecoder.h"
#include "util/Text.h"

namespace camel::audio {
namespace {

struct DecoderHandle {
    std::unique_ptr<IDecoder> decoder;
};

int32_t mapError(const Error& error) {
    switch (error.code()) {
    case ErrorCode::FileOpen:
        return CAMEL_DECODER_HOST_E_IO;
    case ErrorCode::UnsupportedStream:
    case ErrorCode::FormatNotSupported:
        return CAMEL_DECODER_HOST_E_UNSUPPORTED;
    case ErrorCode::SeekFailed:
        return CAMEL_DECODER_HOST_E_SEEK;
    case ErrorCode::DecodeFailed:
        return CAMEL_DECODER_HOST_E_DECODE;
    default:
        return CAMEL_DECODER_HOST_E_FORMAT;
    }
}

DecoderKind decoderKind(uint32_t kind) {
    switch (kind) {
    case CAMEL_DECODER_FFMPEG:
        return DecoderKind::FFmpeg;
    case CAMEL_DECODER_SYMPHONIA:
        return DecoderKind::Symphonia;
    case CAMEL_DECODER_DR_FLAC:
        return DecoderKind::DrFlac;
    case CAMEL_DECODER_SNDFILE:
        return DecoderKind::Sndfile;
    default:
        throw Error(ErrorCode::UnsupportedStream, "unknown FLAC decoder selection");
    }
}

int32_t openDecoderHost(void*, uint32_t kind, const uint8_t* path, uint64_t pathLength, void** outDecoder,
                        CamelSourceInfoV1* info) noexcept {
    if (!path || !pathLength || pathLength > std::numeric_limits<size_t>::max() || !outDecoder || !info)
        return CAMEL_DECODER_HOST_E_FORMAT;
    *outDecoder = nullptr;
    try {
        const std::string_view utf8(reinterpret_cast<const char*>(path), static_cast<size_t>(pathLength));
        auto decoder = openDecoder(decoderKind(kind), std::filesystem::path(toWide(utf8)));
        const auto& source = decoder->info();
        auto handle = std::make_unique<DecoderHandle>();
        handle->decoder = std::move(decoder);
        CamelSourceInfoV1 result{};
        result.struct_size = sizeof(result);
        result.abi_version = CAMEL_ENGINE_ABI_VERSION;
        result.sample_rate = source.sampleRate;
        result.channel_count = source.channels;
        result.valid_bits = source.bitsPerSample;
        result.total_frames = source.totalFrames;
        result.has_md5 = source.md5.has_value() ? 1u : 0u;
        if (source.md5) std::copy(source.md5->begin(), source.md5->end(), result.md5);
        *info = result;
        *outDecoder = handle.release();
        return CAMEL_DECODER_HOST_OK;
    } catch (const Error& error) {
        return mapError(error);
    } catch (...) {
        return CAMEL_DECODER_HOST_E_FORMAT;
    }
}

int32_t readDecoderHost(void*, void* opaque, int32_t* output, uint64_t frames, uint64_t* framesRead) noexcept {
    if (!opaque || (frames && !output) || !framesRead || frames > std::numeric_limits<size_t>::max())
        return CAMEL_DECODER_HOST_E_FORMAT;
    *framesRead = 0;
    try {
        auto& handle = *static_cast<DecoderHandle*>(opaque);
        const auto read = handle.decoder->read(output, static_cast<size_t>(frames));
        *framesRead = read;
        return CAMEL_DECODER_HOST_OK;
    } catch (const Error& error) {
        return mapError(error);
    } catch (...) {
        return CAMEL_DECODER_HOST_E_DECODE;
    }
}

int32_t seekDecoderHost(void*, void* opaque, uint64_t frame) noexcept {
    if (!opaque) return CAMEL_DECODER_HOST_E_FORMAT;
    try {
        static_cast<DecoderHandle*>(opaque)->decoder->seek(frame);
        return CAMEL_DECODER_HOST_OK;
    } catch (const Error& error) {
        return mapError(error);
    } catch (...) {
        return CAMEL_DECODER_HOST_E_SEEK;
    }
}

void closeDecoderHost(void*, void* opaque) noexcept {
    delete static_cast<DecoderHandle*>(opaque);
}

} // namespace

CamelDecoderHostV1 makeDecoderHostV1() {
    return {sizeof(CamelDecoderHostV1),
            CAMEL_ENGINE_ABI_VERSION,
            nullptr,
            openDecoderHost,
            readDecoderHost,
            seekDecoderHost,
            closeDecoderHost};
}

} // namespace camel::audio
