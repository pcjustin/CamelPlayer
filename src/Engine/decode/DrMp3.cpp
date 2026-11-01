// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// MP3 through dr_mp3 (dr_libs, header-only), which trims LAME/Xing encoder delay and padding itself.
#include "Engine/decode/DecoderImpl.h"

#pragma warning(push, 0)
#define DR_MP3_IMPLEMENTATION
#include <dr_mp3.h>
#pragma warning(pop)

#include <cstdio>
#include <vector>

#include "Common/util/Text.h"
#include "Engine/decode/Quantize.h"

namespace camel::audio {
namespace {

class DrMp3 final : public IDecoder {
public:
    DrMp3(const std::filesystem::path& path, uint32_t lossyBits, uint64_t start) : start_(start) {
        if (_wfopen_s(&file_, longPath(path).c_str(), L"rb") != 0 || !file_)
            throw Error(ErrorCode::FileOpen, "dr_mp3: cannot open file");
        if (!onSeek(this, 0, DRMP3_SEEK_SET) || !drmp3_init(&mp3_, onRead, onSeek, onTell, nullptr, this, nullptr)) {
            std::fclose(file_);
            throw Error(ErrorCode::FileOpen, "drmp3_init failed");
        }
        info_.sampleRate = mp3_.sampleRate;
        info_.channels = mp3_.channels;
        info_.bitsPerSample = lossyBits;
        info_.totalFrames = drmp3_get_pcm_frame_count(&mp3_);
        if (!info_.sampleRate || !info_.channels) {
            drmp3_uninit(&mp3_);
            std::fclose(file_);
            throw Error(ErrorCode::UnsupportedStream, "dr_mp3: unexpected stream parameters");
        }
    }

    ~DrMp3() override {
        drmp3_uninit(&mp3_);
        std::fclose(file_);
    }
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
    // dr_mp3 reads the file from `start_` on, as if it began there.
    static size_t onRead(void* self, void* out, size_t bytes) {
        return std::fread(out, 1, bytes, static_cast<DrMp3*>(self)->file_);
    }
    static drmp3_bool32 onSeek(void* self, int offset, drmp3_seek_origin origin) {
        auto* d = static_cast<DrMp3*>(self);
        if (origin == DRMP3_SEEK_SET) return _fseeki64(d->file_, int64_t(d->start_) + offset, SEEK_SET) == 0;
        return _fseeki64(d->file_, offset, origin == DRMP3_SEEK_CUR ? SEEK_CUR : SEEK_END) == 0;
    }
    static drmp3_bool32 onTell(void* self, drmp3_int64* cursor) {
        auto* d = static_cast<DrMp3*>(self);
        const int64_t at = _ftelli64(d->file_);
        *cursor = at - int64_t(d->start_);
        return at >= 0;
    }

    uint64_t start_;
    std::FILE* file_ = nullptr;
    drmp3 mp3_{};
    SourceInfo info_;
    std::vector<float> buffer_;
};

} // namespace

std::unique_ptr<IDecoder> openDrMp3(const std::filesystem::path& path, uint32_t lossyBits, uint64_t start) {
    return std::make_unique<DrMp3>(path, lossyBits, start);
}

} // namespace camel::audio
