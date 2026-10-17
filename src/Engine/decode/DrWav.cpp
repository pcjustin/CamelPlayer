// WAV and AIFF through dr_wav (dr_libs, header-only).
#include "Engine/decode/DecoderImpl.h"

#pragma warning(push, 0)
#define DR_WAV_IMPLEMENTATION
#include <dr_wav.h>
#pragma warning(pop)

#include "Common/util/Text.h"

namespace camel::audio {
namespace {

class DrWav final : public IDecoder {
public:
    explicit DrWav(const std::filesystem::path& path) {
        if (!drwav_init_file_w(&wav_, longPath(path).c_str(), nullptr))
            throw Error(ErrorCode::FileOpen, "drwav_init_file_w failed");
        if (wav_.translatedFormatTag != DR_WAVE_FORMAT_PCM) {
            drwav_uninit(&wav_);
            throw Error(ErrorCode::UnsupportedStream, "dr_wav: only integer PCM is supported");
        }
        info_.sampleRate = wav_.sampleRate;
        info_.channels = wav_.channels;
        info_.bitsPerSample = wav_.bitsPerSample;
        info_.totalFrames = wav_.totalPCMFrameCount;
    }

    ~DrWav() override { drwav_uninit(&wav_); }
    DrWav(const DrWav&) = delete;
    DrWav& operator=(const DrWav&) = delete;

    const SourceInfo& info() const override { return info_; }

    size_t read(int32_t* dst, size_t frames) override {
        if (!frames) return 0;
        const drwav_uint64 got = drwav_read_pcm_frames_s32(&wav_, frames, dst);
        // dr_wav stops early instead of reporting truncated data; treat a short read before the end as an error.
        if (got == 0 && wav_.readCursorInPCMFrames < info_.totalFrames)
            throw Error(ErrorCode::DecodeFailed, "dr_wav: data ends at frame " + std::to_string(wav_.readCursorInPCMFrames));
        const int shift = 32 - int(info_.bitsPerSample);
        for (size_t i = 0, n = size_t(got) * info_.channels; i < n; i++) dst[i] >>= shift;
        return size_t(got);
    }

    void seek(uint64_t frame) override {
        if (!drwav_seek_to_pcm_frame(&wav_, frame)) throw Error(ErrorCode::SeekFailed, "drwav_seek_to_pcm_frame failed");
    }

private:
    drwav wav_{};
    SourceInfo info_;
};

} // namespace

std::unique_ptr<IDecoder> openDrWav(const std::filesystem::path& path) {
    return std::make_unique<DrWav>(path);
}

} // namespace camel::audio
