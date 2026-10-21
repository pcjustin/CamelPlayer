// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Engine/decode/DecoderHost.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/DecoderRegistry.h"
#include "Engine/decode/IDecoder.h"
#include "Common/util/Text.h"

namespace camel::audio {
namespace {

struct DecoderHandle {
    std::unique_ptr<IDecoder> decoder;
    std::vector<float> floats; // a lossy decoder's output, handed on as float32 bit patterns
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

constexpr std::pair<uint32_t, DecoderKind> kBuiltinNumbers[] = {
    {CAMEL_DECODER_FFMPEG, DecoderKind::FFmpeg},  {CAMEL_DECODER_SYMPHONIA, DecoderKind::Symphonia},
    {CAMEL_DECODER_DR_FLAC, DecoderKind::DrFlac}, {CAMEL_DECODER_SNDFILE, DecoderKind::Sndfile},
    {CAMEL_DECODER_DR_WAV, DecoderKind::DrWav},   {CAMEL_DECODER_MPG123, DecoderKind::Mpg123},
    {CAMEL_DECODER_DR_MP3, DecoderKind::DrMp3},   {CAMEL_DECODER_DSD_NEXUS, DecoderKind::DsdNexus}};

std::string decoderFor(uint32_t number) {
    for (const auto& [n, kind] : kBuiltinNumbers)
        if (n == number) return decoderId(kind);
    if (number >= CAMEL_DECODER_REGISTERED_BASE)
        if (const auto entry = decoderRegistry().at(number - CAMEL_DECODER_REGISTERED_BASE)) return entry->id;
    throw Error(ErrorCode::UnsupportedStream, "unknown decoder selection");
}

int32_t openDecoderHost(void* context, uint32_t kind, const uint8_t* path, uint64_t pathLength, void** outDecoder,
                        CamelSourceInfoV1* info) noexcept {
    if (!path || !pathLength || pathLength > std::numeric_limits<size_t>::max() || !outDecoder || !info)
        return CAMEL_DECODER_HOST_E_FORMAT;
    *outDecoder = nullptr;
    try {
        const std::string_view utf8(reinterpret_cast<const char*>(path), static_cast<size_t>(pathLength));
        const auto* pcmBits = static_cast<const PcmBitsByFormat*>(context);
        auto decoder = openDecoder(decoderFor(kind), std::filesystem::path(toWide(utf8)), pcmBits ? *pcmBits : PcmBitsByFormat{});
        const auto& source = decoder->info();
        auto handle = std::make_unique<DecoderHandle>();
        handle->decoder = std::move(decoder);
        CamelSourceInfoV1 result{};
        result.struct_size = sizeof(result);
        result.abi_version = CAMEL_ENGINE_ABI_VERSION;
        result.sample_rate = source.sampleRate;
        result.channel_count = source.channels;
        result.valid_bits = source.bitsPerSample;
        result.sample_encoding = source.bitsPerSample == 1       ? CAMEL_SAMPLE_ENCODING_DSD
                                 : handle->decoder->readsFloat() ? CAMEL_SAMPLE_ENCODING_FLOAT32
                                                                 : CAMEL_SAMPLE_ENCODING_INTEGER;
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
        size_t read = 0;
        if (handle.decoder->readsFloat()) {
            handle.floats.resize(static_cast<size_t>(frames) * handle.decoder->info().channels);
            read = handle.decoder->readFloat(handle.floats.data(), static_cast<size_t>(frames));
            std::memcpy(output, handle.floats.data(), read * handle.decoder->info().channels * sizeof(float));
        } else {
            read = handle.decoder->read(output, static_cast<size_t>(frames));
        }
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

CamelDecoderHostV1 makeDecoderHostV1(const PcmBitsByFormat* pcmBits) {
    return {sizeof(CamelDecoderHostV1),
            CAMEL_ENGINE_ABI_VERSION,
            const_cast<PcmBitsByFormat*>(pcmBits),
            openDecoderHost,
            readDecoderHost,
            seekDecoderHost,
            closeDecoderHost};
}

uint32_t abiDecoder(std::string_view decoder) {
    for (const auto& [number, kind] : kBuiltinNumbers)
        if (decoder == decoderId(kind)) return number;
    const auto index = decoderRegistry().indexOf(decoder);
    return index ? CAMEL_DECODER_REGISTERED_BASE + uint32_t(*index) : 0;
}

} // namespace camel::audio
