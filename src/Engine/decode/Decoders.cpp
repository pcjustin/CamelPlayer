#include "Engine/decode/DecoderImpl.h"

#include <algorithm>

#include "Engine/decode/FlacStreamInfo.h"
#include "Engine/decode/FormatProbe.h"
#include "Engine/verify/FlacMd5.h"

namespace camel::audio {

const char* decoderName(DecoderKind k) {
    switch (k) {
    case DecoderKind::FFmpeg:
        return "FFmpeg";
    case DecoderKind::Symphonia:
        return "Symphonia";
    case DecoderKind::DrFlac:
        return "dr_flac";
    case DecoderKind::Sndfile:
        return "libsndfile";
    case DecoderKind::DrWav:
        return "dr_wav";
    }
    return "?";
}

const char* decoderId(DecoderKind k) {
    switch (k) {
    case DecoderKind::FFmpeg:
        return "ffmpeg";
    case DecoderKind::Symphonia:
        return "symphonia";
    case DecoderKind::DrFlac:
        return "drflac";
    case DecoderKind::Sndfile:
        return "sndfile";
    case DecoderKind::DrWav:
        return "drwav";
    }
    return "?";
}

bool decoderFromId(std::string_view id, DecoderKind& out) {
    for (DecoderKind k : kAllDecoders)
        if (id == decoderId(k)) {
            out = k;
            return true;
        }
    return false;
}

namespace {

// Gives every decoder the same STREAMINFO MD5 (from our own parser), so the engine and reports
// do not depend on which decoder happens to expose it.
class WithStreamInfo final : public IDecoder {
public:
    WithStreamInfo(std::unique_ptr<IDecoder> inner, const SourceInfo& parsed) : inner_(std::move(inner)), info_(inner_->info()) {
        info_.md5 = parsed.md5;
        if (!info_.totalFrames) info_.totalFrames = parsed.totalFrames;
        if (info_.md5) md5_ = std::make_unique<FlacMd5>(info_.bitsPerSample);
    }
    const SourceInfo& info() const override { return info_; }
    size_t read(int32_t* dst, size_t frames) override {
        if (!frames || ended_) return 0;
        const size_t got = inner_->read(dst, frames);
        position_ += got;
        if (info_.totalFrames && (position_ > info_.totalFrames || (!got && position_ != info_.totalFrames)))
            throw Error(ErrorCode::DecodeFailed, "FLAC frame count does not match STREAMINFO");
        if (got) {
            if (md5_) md5_->update(dst, got * info_.channels);
        } else {
            ended_ = true;
            if (md5_ && info_.md5 && md5_->finish() != *info_.md5) throw Error(ErrorCode::DecodeFailed, "FLAC MD5 mismatch");
        }
        return got;
    }
    void seek(uint64_t frame) override {
        if (info_.totalFrames && frame >= info_.totalFrames) throw Error(ErrorCode::SeekFailed, "seek target is past FLAC end");
        inner_->seek(frame);
        position_ = frame;
        ended_ = false;
        md5_.reset();
        if (!frame && info_.md5) md5_ = std::make_unique<FlacMd5>(info_.bitsPerSample);
    }

private:
    std::unique_ptr<IDecoder> inner_;
    SourceInfo info_;
    uint64_t position_ = 0;
    bool ended_ = false;
    std::unique_ptr<FlacMd5> md5_;
};

// Stops at the container's frame count. Symphonia 0.5.5 counts the AIFF SSND offset and block-size words as audio, so
// it reports one or two extra frames and would decode bytes of a chunk that follows the sound data.
class FrameLimit final : public IDecoder {
public:
    FrameLimit(std::unique_ptr<IDecoder> inner, uint64_t frames) : inner_(std::move(inner)), info_(inner_->info()) {
        info_.totalFrames = frames;
    }
    const SourceInfo& info() const override { return info_; }
    size_t read(int32_t* dst, size_t frames) override {
        const size_t got = inner_->read(dst, size_t(std::min<uint64_t>(frames, info_.totalFrames - position_)));
        position_ += got;
        return got;
    }
    void seek(uint64_t frame) override {
        if (frame >= info_.totalFrames) throw Error(ErrorCode::SeekFailed, "seek target is past the end");
        inner_->seek(frame);
        position_ = frame;
    }

private:
    std::unique_ptr<IDecoder> inner_;
    SourceInfo info_;
    uint64_t position_ = 0;
};

} // namespace

std::unique_ptr<IDecoder> openDecoder(DecoderKind kind, const std::filesystem::path& path) {
    const AudioFormat format = probeFormat(path);
    if (!supports(format, kind))
        throw Error(ErrorCode::UnsupportedStream, std::string(decoderName(kind)) + " cannot decode " + formatName(format));
    std::unique_ptr<IDecoder> d;
    switch (kind) {
    case DecoderKind::FFmpeg:
        d = openFfmpeg(path);
        break;
    case DecoderKind::Symphonia:
        d = openSymphonia(path);
        break;
    case DecoderKind::DrFlac:
        d = openDrFlac(path);
        break;
    case DecoderKind::Sndfile:
        d = openSndfile(path);
        break;
    case DecoderKind::DrWav:
        d = openDrWav(path);
        break;
    default:
        throw Error(ErrorCode::UnsupportedStream, "unknown decoder selection");
    }
    // The player supports 16/24-bit integer PCM sources only (docs/Plan.md §3.1).
    if (const auto bits = d->info().bitsPerSample; bits != 16 && bits != 24)
        throw Error(ErrorCode::UnsupportedStream, std::to_string(bits) + "-bit sources are not supported (16/24-bit only)");
    if (format == AudioFormat::Aiff && kind == DecoderKind::Symphonia)
        if (const auto frames = aiffSampleFrames(path)) return std::make_unique<FrameLimit>(std::move(d), *frames);
    SourceInfo parsed;
    if (format == AudioFormat::Flac && readFlacStreamInfo(path, parsed))
        return std::make_unique<WithStreamInfo>(std::move(d), parsed);
    return d;
}

} // namespace camel::audio
