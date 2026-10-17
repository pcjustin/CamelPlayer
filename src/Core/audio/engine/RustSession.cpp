#include "audio/engine/RustSession.h"

#include <algorithm>
#include <string>

#include "audio/decode/DecoderHost.h"
#include "util/Text.h"

namespace camel::audio {
namespace {

CamelAudioFormatV1 abiFormat(const DeviceFormat& f) {
    CamelAudioFormatV1 format{};
    format.struct_size = sizeof(format);
    format.abi_version = CAMEL_ENGINE_ABI_VERSION;
    format.stream_kind = CAMEL_AUDIO_PCM;
    format.rate_hz = f.sampleRate;
    format.channel_count = f.channels;
    format.channel_mask = f.channels == 2 ? 3 : 0;
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
    }
    return format;
}

uint32_t abiDecoder(DecoderKind kind) {
    switch (kind) {
    case DecoderKind::FFmpeg:
        return CAMEL_DECODER_FFMPEG;
    case DecoderKind::Symphonia:
        return CAMEL_DECODER_SYMPHONIA;
    case DecoderKind::DrFlac:
        return CAMEL_DECODER_DR_FLAC;
    case DecoderKind::Sndfile:
        return CAMEL_DECODER_SNDFILE;
    }
    return CAMEL_DECODER_SNDFILE;
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
        return Error(ErrorCode::UnsupportedStream, detail + " (16/24-bit sources only)");
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

RustSession::RustSession(const std::filesystem::path& path, DecoderKind decoder, const DeviceFormat& format, uint64_t startFrame)
    : host_(makeDecoderHostV1()) {
    command_.struct_size = sizeof(command_);
    command_.abi_version = CAMEL_ENGINE_ABI_VERSION;
    command_.generation = 1;
    const CamelEngineConfigV1 config{sizeof(CamelEngineConfigV1), CAMEL_ENGINE_ABI_VERSION, CAMEL_ENGINE_MAX_RENDER_UNITS,
                                     abiFormat(format)};
    if (const auto status = camel_engine_create_v1(&config, &engine_); status != CAMEL_ENGINE_OK)
        throw engineError("create", status);
    const auto utf8 = toUtf8(path.wstring());
    const auto source = trackSource(utf8);
    auto command = nextCommand();
    auto status = camel_engine_load_flac_v1(engine_, &command, &source, abiDecoder(decoder), &host_);
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
    for (size_t done = 0; done < frames;) {
        const size_t chunk = std::min<size_t>(frames - done, CAMEL_ENGINE_MAX_RENDER_UNITS);
        const CamelRenderRequestV1 request{sizeof(CamelRenderRequestV1), CAMEL_ENGINE_ABI_VERSION, chunk,
                                           destination + done * bytesPerFrame, chunk * bytesPerFrame};
        CamelRenderResultV1 result{sizeof(CamelRenderResultV1), CAMEL_ENGINE_ABI_VERSION, 0, 0, 0, 0};
        const auto status = camel_engine_render_v1(engine_, &request, &result);
        rendered.frames += size_t(result.units_rendered);
        if (status == CAMEL_ENGINE_END) {
            rendered.ended = true;
            break;
        }
        if (status == CAMEL_ENGINE_UNDERRUN) {
            rendered.underrun = true;
            break;
        }
        if (status < 0 || result.state != CAMEL_RENDER_STATE_ACTIVE || result.units_rendered < chunk) {
            rendered.fault = result.state == CAMEL_RENDER_STATE_FAULT;
            break;
        }
        done += chunk;
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
    return camel_engine_queue_next_flac_v1(engine_, &command, &source, abiDecoder(decoder), &host_) == CAMEL_ENGINE_OK;
}

bool RustSession::clearNext() {
    const auto command = nextCommand();
    return camel_engine_clear_next_v1(engine_, &command) == CAMEL_ENGINE_OK;
}

RustSession::Transitions RustSession::transitions() const noexcept {
    Transitions result;
    uint32_t queued = 0;
    camel_engine_get_transitions_v1(engine_, &result.count, &result.trackPosition, &queued);
    result.nextQueued = queued != 0;
    return result;
}

} // namespace camel::audio
