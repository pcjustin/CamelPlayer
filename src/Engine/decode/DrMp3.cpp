// MP3 through dr_mp3 (dr_libs, header-only), which trims LAME/Xing encoder delay and padding itself.
#include "Engine/decode/DecoderImpl.h"

#pragma warning(push, 0)
#define DR_MP3_IMPLEMENTATION
#include <dr_mp3.h>
#pragma warning(pop)

#include <vector>

#include "Common/util/Text.h"
#include "Engine/decode/Quantize.h"

namespace camel::audio {
namespace {

class DrMp3 final : public IDecoder {
public:
    DrMp3(const std::filesystem::path& path, uint32_t lossyBits) {
        if (!drmp3_init_file_w(&mp3_, longPath(path).c_str(), nullptr))
            throw Error(ErrorCode::FileOpen, "drmp3_init_file_w failed");
        info_.sampleRate = mp3_.sampleRate;
        info_.channels = mp3_.channels;
        info_.bitsPerSample = lossyBits;
        info_.totalFrames = drmp3_get_pcm_frame_count(&mp3_);
        if (!info_.sampleRate || !info_.channels) {
            drmp3_uninit(&mp3_);
            throw Error(ErrorCode::UnsupportedStream, "dr_mp3: unexpected stream parameters");
        }
    }

    ~DrMp3() override { drmp3_uninit(&mp3_); }
    DrMp3(const DrMp3&) = delete;
    DrMp3& operator=(const DrMp3&) = delete;

    const SourceInfo& info() const override { return info_; }

    size_t read(int32_t* dst, size_t frames) override {
        buffer_.resize(frames * info_.channels);
        const size_t got = readFloat(buffer_.data(), frames);
        for (size_t i = 0, n = got * info_.channels; i < n; i++) dst[i] = quantize(buffer_[i], info_.bitsPerSample);
        return got;
    }

    bool readsFloat() const override { return true; }
    size_t readFloat(float* dst, size_t frames) override {
        return frames ? size_t(drmp3_read_pcm_frames_f32(&mp3_, frames, dst)) : 0;
    }

    void seek(uint64_t frame) override {
        if (!drmp3_seek_to_pcm_frame(&mp3_, frame)) throw Error(ErrorCode::SeekFailed, "drmp3_seek_to_pcm_frame failed");
    }

private:
    drmp3 mp3_{};
    SourceInfo info_;
    std::vector<float> buffer_;
};

} // namespace

std::unique_ptr<IDecoder> openDrMp3(const std::filesystem::path& path, uint32_t lossyBits) {
    return std::make_unique<DrMp3>(path, lossyBits);
}

} // namespace camel::audio
