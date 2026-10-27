// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Engine/RustSession.h"

#include <algorithm>
#include <string>

#include "Engine/decode/DecoderHost.h"
#include "Common/util/Text.h"

namespace camel::audio {
namespace {

CamelAudioFormatV1 abiFormat(const DeviceFormat& f, DsdPath dsd) {
    CamelAudioFormatV1 format{};
    format.struct_size = sizeof(format);
    format.abi_version = CAMEL_ENGINE_ABI_VERSION;
    format.stream_kind = CAMEL_AUDIO_PCM;
    format.rate_hz = f.sampleRate;
    format.channel_count = f.channels;
    format.channel_mask = f.channels == 2 ? 3 : 0;
    if (dsd == DsdPath::Native) {
        format.stream_kind = CAMEL_AUDIO_DSD;
        format.sample_format = CAMEL_SAMPLE_DSD_U8;
        format.rate_hz = f.sampleRate * 8; // DSD samples per second
        format.valid_bits = 1;
        format.container_bits = 8;
        return format;
    }
    if (dsd == DsdPath::Dop) {
        format.stream_kind = CAMEL_AUDIO_DOP;
        format.sample_format = f.type == SampleType::Int24 ? CAMEL_SAMPLE_DOP_S24_LE : CAMEL_SAMPLE_DOP_S24_IN_32_LE;
        format.valid_bits = 24;
        format.container_bits = f.type == SampleType::Int24 ? 24 : 32;
        return format;
    }
    switch (f.type) {
    case SampleType::Int16:
        format.sample_format = CAMEL_SAMPLE_S16_LE;
        format.valid_bits = format.container_bits = 16;
        break;
    case SampleType::Int24:
        format.sample_format = CAMEL_SAMPLE_S24_LE;
        format.valid_bits = format.container_bits = 24;
        break;
    case SampleType::Int24In32:
        format.sample_format = CAMEL_SAMPLE_S24_IN_32_LE;
        format.valid_bits = 24;
        format.container_bits = 32;
        break;
    case SampleType::Int32:
        format.sample_format = CAMEL_SAMPLE_S32_LE;
        format.valid_bits = format.container_bits = 32;
        break;
    case SampleType::Float32:
        format.sample_format = CAMEL_SAMPLE_F32_LE;
        format.valid_bits = format.container_bits = 32;
        break;
    case SampleType::Dsd8:
        break; // native DSD returned above
    }
    return format;
}

Error engineError(const char* what, CamelEngineStatus status) {
    const std::string detail = std::string(what) + ": Rust engine status " + std::to_string(status);
    switch (status) {
    case CAMEL_ENGINE_E_SOURCE_IO:
        return Error(ErrorCode::FileOpen, detail);
    case CAMEL_ENGINE_E_DECODE:
        return Error(ErrorCode::DecodeFailed, detail);
    case CAMEL_ENGINE_E_SEEK:
        return Error(ErrorCode::SeekFailed, detail);
    case CAMEL_ENGINE_E_UNSUPPORTED_FORMAT:
        return Error(ErrorCode::UnsupportedStream, detail + " (the source cannot be played in this output format)");
    default:
        return Error(ErrorCode::Other, detail);
    }
}

// Borrows `utf8`, which must outlive the call that receives the source.
CamelTrackSourceV1 trackSource(const std::string& utf8) {
    CamelTrackSourceV1 source{};
    source.struct_size = sizeof(source);
    source.abi_version = CAMEL_ENGINE_ABI_VERSION;
    source.path_utf8 = reinterpret_cast<const uint8_t*>(utf8.data());
    source.path_length_bytes = utf8.size();
    return source;
}

void destroy(CamelEngine* engine) noexcept {
    camel_engine_quiesce_v1(engine);
    camel_engine_destroy_v1(engine);
}

} // namespace

RustSession::RustSession(const std::filesystem::path& path, DecoderKind decoder, const DeviceFormat& format, uint64_t startFrame,
                         const PcmBitsByFormat& pcmBits, Conversion conversion, DsdPath dsd, uint32_t predecodeSeconds)
    : pcmBits_(pcmBits), host_(makeDecoderHostV1(&pcmBits_)), raw_(dsd == DsdPath::Native || dsd == DsdPath::Dop),
      unitsPerFrame_(dsd == DsdPath::Native ? 8 : 1) {
    command_.struct_size = sizeof(command_);
    command_.abi_version = CAMEL_ENGINE_ABI_VERSION;
    command_.generation = 1;
    const uint32_t quality = conversion.quality == ResampleQuality::Quick      ? CAMEL_RESAMPLE_QUICK
                             : conversion.quality == ResampleQuality::VeryHigh ? CAMEL_RESAMPLE_VERY_HIGH
                                                                               : CAMEL_RESAMPLE_HIGH;
    const CamelEngineConfigV1 config{sizeof(CamelEngineConfigV1),   CAMEL_ENGINE_ABI_VERSION,   CAMEL_ENGINE_MAX_RENDER_UNITS,
                                     abiFormat(format, dsd),        conversion.allow ? 1u : 0u, quality,
                                     conversion.allowDsd ? 1u : 0u, predecodeSeconds * 1000};
    if (const auto status = camel_engine_create_v1(&config, &engine_); status != CAMEL_ENGINE_OK)
        throw engineError("create", status);
    const auto utf8 = toUtf8(path.wstring());
    const auto source = trackSource(utf8);
    auto command = nextCommand();
    auto status = camel_engine_load_flac_v1(engine_, &command, &source, abiDecoder(decoderId(decoder)), &host_);
    if (status == CAMEL_ENGINE_OK && startFrame) {
        command = nextCommand();
        status = camel_engine_seek_v1(engine_, &command, startFrame);
    }
    if (status != CAMEL_ENGINE_OK) {
        destroy(engine_);
        throw engineError(startFrame ? "load and seek" : "load", status);
    }
}

RustSession::~RustSession() {
    destroy(engine_);
}

CamelCommandStampV1 RustSession::nextCommand() {
    ++command_.command_id;
    return command_;
}

void RustSession::play() {
    const auto command = nextCommand();
    if (const auto status = camel_engine_play_v1(engine_, &command); status != CAMEL_ENGINE_OK) throw engineError("play", status);
}

void RustSession::pause() {
    const auto command = nextCommand();
    if (const auto status = camel_engine_pause_v1(engine_, &command); status != CAMEL_ENGINE_OK)
        throw engineError("pause", status);
}

RustSession::Prefill RustSession::prefill(uint64_t& frames) {
    frames = 0;
    const auto status = camel_engine_prefill_v1(engine_, CAMEL_ENGINE_MAX_RENDER_UNITS, &frames);
    switch (status) {
    case CAMEL_ENGINE_OK:
        return frames ? Prefill::Decoded : Prefill::Full;
    case CAMEL_ENGINE_END:
        return Prefill::End;
    case CAMEL_ENGINE_E_NOT_READY:
        return Prefill::Full;
    default:
        throw engineError("decode", status);
    }
}

RustSession::Rendered RustSession::render(uint8_t* destination, size_t frames, size_t bytesPerFrame) noexcept {
    Rendered rendered;
    const auto maxChunk = size_t(CAMEL_ENGINE_MAX_RENDER_UNITS / unitsPerFrame_);
    for (size_t done = 0; done < frames;) {
        const size_t chunk = std::min(frames - done, maxChunk);
        const CamelRenderRequestV1 request{sizeof(CamelRenderRequestV1), CAMEL_ENGINE_ABI_VERSION, chunk * unitsPerFrame_,
                                           destination + done * bytesPerFrame, chunk * bytesPerFrame};
        CamelRenderResultV1 result{sizeof(CamelRenderResultV1), CAMEL_ENGINE_ABI_VERSION, 0, 0, 0, 0};
        const auto status = camel_engine_render_v1(engine_, &request, &result);
        const auto got = size_t(result.units_rendered / unitsPerFrame_);
        // Audio ends within this request; later chunks of a raw DSD stream still need the Engine's silence.
        if (!rendered.ended && !rendered.underrun && !rendered.fault) rendered.frames += got;
        done += chunk;
        if (status == CAMEL_ENGINE_END)
            rendered.ended = true;
        else if (status == CAMEL_ENGINE_UNDERRUN)
            rendered.underrun = true;
        else if (status < 0 || result.state != CAMEL_RENDER_STATE_ACTIVE || got < chunk)
            rendered.fault = rendered.fault || result.state == CAMEL_RENDER_STATE_FAULT;
        else
            continue;
        if (!raw_) break;
    }
    return rendered;
}

void RustSession::reportAudible(uint64_t frame) {
    camel_engine_report_audible_position_v1(engine_, &command_, frame);
}

bool RustSession::queueNext(const std::filesystem::path& path, DecoderKind decoder) {
    const auto utf8 = toUtf8(path.wstring());
    const auto source = trackSource(utf8);
    const auto command = nextCommand();
    return camel_engine_queue_next_flac_v1(engine_, &command, &source, abiDecoder(decoderId(decoder)), &host_) == CAMEL_ENGINE_OK;
}

bool RustSession::clearNext() {
    const auto command = nextCommand();
    return camel_engine_clear_next_v1(engine_, &command) == CAMEL_ENGINE_OK;
}

Md5State RustSession::md5State() const noexcept {
    CamelFlacIntegrityV1 integrity{sizeof(CamelFlacIntegrityV1), CAMEL_ENGINE_ABI_VERSION};
    if (camel_engine_get_flac_integrity_v1(engine_, &integrity) != CAMEL_ENGINE_OK) return Md5State::Unavailable;
    switch (integrity.status) {
    case CAMEL_FLAC_INTEGRITY_INCOMPLETE:
        return Md5State::Incomplete;
    case CAMEL_FLAC_INTEGRITY_VERIFIED:
        return Md5State::Verified;
    case CAMEL_FLAC_INTEGRITY_FAILED:
        return Md5State::Failed;
    default:
        return Md5State::Unavailable;
    }
}

RustSession::Transitions RustSession::transitions() const noexcept {
    Transitions result;
    uint32_t queued = 0;
    camel_engine_get_transitions_v1(engine_, &result.count, &result.trackPosition, &queued);
    result.nextQueued = queued != 0;
    return result;
}

} // namespace camel::audio
