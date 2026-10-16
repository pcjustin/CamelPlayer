#include "audio/decode/DecoderImpl.h"
#include "audio/decode/FlacStreamInfo.h"
#include "audio/verify/FlacMd5.h"

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

} // namespace

std::unique_ptr<IDecoder> openDecoder(DecoderKind kind, const std::filesystem::path& path) {
    std::unique_ptr<IDecoder> d;
    switch (kind) {
    case DecoderKind::FFmpeg:
        d = openFfmpegFlac(path);
        break;
    case DecoderKind::Symphonia:
        d = openSymphoniaFlac(path);
        break;
    case DecoderKind::DrFlac:
        d = openDrFlac(path);
        break;
    case DecoderKind::Sndfile:
        d = openSndfileFlac(path);
        break;
    default:
        throw Error(ErrorCode::UnsupportedStream, "unknown decoder selection");
    }
    SourceInfo parsed;
    if (readFlacStreamInfo(path, parsed)) return std::make_unique<WithStreamInfo>(std::move(d), parsed);
    return d;
}

} // namespace camel::audio
