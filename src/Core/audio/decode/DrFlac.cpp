// FLAC through dr_flac (dr_libs, header-only).
#include "audio/decode/DecoderImpl.h"

#pragma warning(push, 0)
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_OGG
#include <dr_flac.h>
#pragma warning(pop)

#include "util/Text.h"

namespace camel::audio {
namespace {

class DrFlac final : public IDecoder {
public:
    explicit DrFlac(const std::filesystem::path& path) {
        flac_ = drflac_open_file_w(longPath(path).c_str(), nullptr);
        if (!flac_) throw Error(ErrorCode::FileOpen, "drflac_open_file_w failed");
        info_.sampleRate = flac_->sampleRate;
        info_.channels = flac_->channels;
        info_.bitsPerSample = flac_->bitsPerSample;
        info_.totalFrames = flac_->totalPCMFrameCount;
        if (!info_.bitsPerSample || info_.bitsPerSample > 32) {
            drflac_close(flac_);
            throw Error(ErrorCode::UnsupportedStream, "dr_flac: unexpected bit depth");
        }
    }

    ~DrFlac() override { drflac_close(flac_); }

    const SourceInfo& info() const override { return info_; }

    size_t read(int32_t* dst, size_t frames) override {
        drflac_uint64 got = drflac_read_pcm_frames_s32(flac_, frames, dst);
        // dr_flac stops early instead of reporting corrupt frames; treat a short read before the end as an error.
        if (got == 0 && info_.totalFrames && flac_->currentPCMFrame < info_.totalFrames)
            throw Error(ErrorCode::DecodeFailed, "dr_flac: decoding stopped at frame " + std::to_string(flac_->currentPCMFrame));
        const int shift = 32 - int(info_.bitsPerSample);
        if (shift)
            for (size_t i = 0, n = size_t(got) * info_.channels; i < n; i++) dst[i] >>= shift;
        return size_t(got);
    }

    void seek(uint64_t frame) override {
        if (!drflac_seek_to_pcm_frame(flac_, frame)) throw Error(ErrorCode::SeekFailed, "drflac_seek_to_pcm_frame failed");
    }

private:
    drflac* flac_ = nullptr;
    SourceInfo info_;
};

} // namespace

std::unique_ptr<IDecoder> openDrFlac(const std::filesystem::path& path) {
    return std::make_unique<DrFlac>(path);
}

} // namespace camel::audio
