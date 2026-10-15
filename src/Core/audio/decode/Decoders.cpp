#include "audio/decode/DecoderImpl.h"
#include "audio/decode/FlacStreamInfo.h"

namespace camel::audio {

const char* decoderName(DecoderKind k) {
    switch (k) {
    case DecoderKind::FFmpeg: return "FFmpeg";
    case DecoderKind::Symphonia: return "Symphonia";
    case DecoderKind::DrFlac: return "dr_flac";
    case DecoderKind::Sndfile: return "libsndfile";
    }
    return "?";
}

const char* decoderId(DecoderKind k) {
    switch (k) {
    case DecoderKind::FFmpeg: return "ffmpeg";
    case DecoderKind::Symphonia: return "symphonia";
    case DecoderKind::DrFlac: return "drflac";
    case DecoderKind::Sndfile: return "sndfile";
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
    }
    const SourceInfo& info() const override { return info_; }
    size_t read(int32_t* dst, size_t frames) override { return inner_->read(dst, frames); }
    void seek(uint64_t frame) override { inner_->seek(frame); }

private:
    std::unique_ptr<IDecoder> inner_;
    SourceInfo info_;
};

}  // namespace

std::unique_ptr<IDecoder> openDecoder(DecoderKind kind, const std::filesystem::path& path) {
    std::unique_ptr<IDecoder> d;
    switch (kind) {
    case DecoderKind::FFmpeg: d = openFfmpegFlac(path); break;
    case DecoderKind::Symphonia: d = openSymphoniaFlac(path); break;
    case DecoderKind::DrFlac: d = openDrFlac(path); break;
    case DecoderKind::Sndfile: d = openSndfileFlac(path); break;
    }
    SourceInfo parsed;
    if (readFlacStreamInfo(path, parsed)) return std::make_unique<WithStreamInfo>(std::move(d), parsed);
    return d;
}

}  // namespace camel::audio
