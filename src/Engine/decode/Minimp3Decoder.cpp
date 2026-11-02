// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// MP3 through minimp3 (header-only), whose streaming reader trims the encoder delay and padding a LAME/Xing frame
// declares.
#include "Engine/decode/DecoderImpl.h"

#pragma warning(push, 0)
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#define MINIMP3_NO_STDIO
#include <minimp3_ex.h>
#pragma warning(pop)

#include <cstdio>
#include <vector>

#include "Common/util/Text.h"
#include "Engine/decode/Quantize.h"

namespace camel::audio {
namespace {

class Minimp3 final : public IDecoder {
public:
    Minimp3(const std::filesystem::path& path, uint32_t lossyBits, uint64_t start) : start_(start) {
        if (_wfopen_s(&file_, longPath(path).c_str(), L"rb") != 0 || !file_)
            throw Error(ErrorCode::FileOpen, "minimp3: cannot open file");
        // Without a Xing/Info header minimp3 would read the whole file to count its samples.
        const int opened = mp3dec_ex_open_cb(&mp3_, &io_, MP3D_SEEK_TO_SAMPLE | MP3D_DO_NOT_SCAN);
        if (opened || !mp3_.info.hz || mp3_.info.channels <= 0) {
            mp3dec_ex_close(&mp3_);
            std::fclose(file_);
            throw Error(ErrorCode::UnsupportedStream, "minimp3: no MPEG audio found");
        }
        info_.sampleRate = uint32_t(mp3_.info.hz);
        info_.channels = uint32_t(mp3_.info.channels);
        info_.bitsPerSample = lossyBits;
        info_.totalFrames = mp3_.samples / info_.channels;
    }

    ~Minimp3() override {
        mp3dec_ex_close(&mp3_);
        std::fclose(file_);
    }
    Minimp3(const Minimp3&) = delete;
    Minimp3& operator=(const Minimp3&) = delete;

    const SourceInfo& info() const override { return info_; }

    size_t read(int32_t* dst, size_t frames) override {
        buffer_.resize(frames * info_.channels);
        const size_t got = readFloat(buffer_.data(), frames);
        for (size_t i = 0, n = got * info_.channels; i < n; i++) dst[i] = quantize(buffer_[i], info_.bitsPerSample);
        return got;
    }

    bool readsFloat() const override { return true; }
    size_t readFloat(float* dst, size_t frames) override {
        const size_t got = mp3dec_ex_read(&mp3_, dst, frames * info_.channels) / info_.channels;
        if (!got && frames && mp3_.last_error)
            throw Error(ErrorCode::DecodeFailed, "minimp3: error " + std::to_string(mp3_.last_error));
        return got;
    }

    void seek(uint64_t frame) override {
        if (mp3dec_ex_seek(&mp3_, frame * info_.channels)) throw Error(ErrorCode::SeekFailed, "mp3dec_ex_seek failed");
    }

private:
    // minimp3 reads the file from `start_` on, as if it began there.
    static size_t onRead(void* out, size_t bytes, void* self) {
        return std::fread(out, 1, bytes, static_cast<Minimp3*>(self)->file_);
    }
    static int onSeek(uint64_t position, void* self) {
        auto* d = static_cast<Minimp3*>(self);
        return _fseeki64(d->file_, int64_t(d->start_ + position), SEEK_SET);
    }

    uint64_t start_;
    std::FILE* file_ = nullptr;
    mp3dec_io_t io_{onRead, this, onSeek, this};
    mp3dec_ex_t mp3_{};
    SourceInfo info_;
    std::vector<float> buffer_;
};

} // namespace

std::unique_ptr<IDecoder> openMinimp3(const std::filesystem::path& path, uint32_t lossyBits, uint64_t start) {
    return std::make_unique<Minimp3>(path, lossyBits, start);
}

} // namespace camel::audio
