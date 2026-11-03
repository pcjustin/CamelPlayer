// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// MP3 through minimp3 (header-only), whose streaming reader trims the encoder delay and padding a LAME/Xing frame
// declares.
#include "Engine/decode/DecoderImpl.h"
#include "Engine/decode/FormatProbe.h"

#pragma warning(push, 0)
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#define MINIMP3_NO_STDIO
#include <minimp3_ex.h>
#pragma warning(pop)

#include <algorithm>
#include <cstdio>
#include <optional>
#include <vector>

#include "Common/util/Text.h"
#include "Engine/decode/Quantize.h"

namespace camel::audio {
namespace {

class Minimp3 final : public IDecoder {
public:
    Minimp3(const std::filesystem::path& path, uint32_t lossyBits, uint64_t start) : path_(path), start_(start) {
        if (_wfopen_s(&file_, longPath(path).c_str(), L"rb") != 0 || !file_)
            throw Error(ErrorCode::FileOpen, "minimp3: cannot open file");
        if (_fseeki64(file_, 0, SEEK_END) == 0) end_ = double(_ftelli64(file_) - int64_t(start_));
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
        // A length measured before is the one to show, and places a seek better.
        if (const auto measured = measuredLength(path)) info_.totalFrames = *measured;
        declared_ = mp3_.detected_samples;
        // Without a Xing/Info frame, the first frame's size, which is every frame's in CBR.
        if (!mp3_.vbr_tag_found)
            frameBytes_ = (info_.sampleRate >= 32000 ? 144.0 : 72.0) * mp3_.info.bitrate_kbps * 1000.0 / info_.sampleRate;
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

    // minimp3 would index the frames of the whole file first. Instead the stream goes to where the frames from kLead
    // before the target's are estimated to start, and skips their samples, counting any frame it cannot decode yet, as
    // its own seek by the index does.
    void seek(uint64_t frame) override {
        constexpr uint64_t kLead = 16; // frames that warm the decoder up
        const uint64_t channels = info_.channels;
        // A Layer III frame's samples: MPEG-1 is 32 kHz and up.
        const uint64_t samples = info_.sampleRate >= 32000 ? 1152 : 576;
        const uint64_t delay = uint64_t(mp3_.start_delay) / channels;
        const uint64_t target = (frame + delay) / samples; // the MP3 frame that holds it
        const uint64_t from = target > kLead ? target - kLead : 0;
        const auto at = from ? estimateStart(from, samples) : std::optional(double(mp3_.start_offset));
        mp3_.detected_samples = declared_;
        if (!at) {
            if (mp3dec_ex_seek(&mp3_, frame * channels)) throw Error(ErrorCode::SeekFailed, "mp3dec_ex_seek failed");
            return;
        }
        mp3_.flags &= ~MP3D_SEEK_TO_SAMPLE;
        const int sought = mp3dec_ex_seek(&mp3_, uint64_t(*at));
        mp3_.flags |= MP3D_SEEK_TO_SAMPLE;
        if (sought) throw Error(ErrorCode::SeekFailed, "mp3dec_ex_seek failed");
        mp3_.to_skip = int((frame + delay - from * samples) * channels);
        mp3_.cur_sample = frame * channels;
        // After an estimated seek the stream ends where its audio does: the declared length, counted from the estimate,
        // could cut it short.
        if (from) mp3_.detected_samples = 0;
    }

private:
    // Where MP3 frame `index` is estimated to start, halfway into the frame before so as not to miss it: by the Xing
    // table, in proportion to the length, or else by the first frame's size.
    std::optional<double> estimateStart(uint64_t index, uint64_t samples) {
        const double from = double(index) - 0.5;
        const auto begin = double(mp3_.start_offset);
        double at = 0;
        if (info_.totalFrames) {
            if (!tocRead_) {
                toc_ = xingToc(path_);
                tocRead_ = true;
            }
            const double total = double(info_.totalFrames) + double(mp3_.start_delay) / double(info_.channels);
            const double share = std::min(1.0, from * double(samples) / total);
            at = toc_ ? toc_->byteShare(share) * toc_->bytes : begin + share * (end_ - begin);
        } else if (frameBytes_ > 0) {
            at = begin + from * frameBytes_;
        } else {
            return std::nullopt;
        }
        return std::max(at, begin);
    }

    // minimp3 reads the file from `start_` on, as if it began there.
    static size_t onRead(void* out, size_t bytes, void* self) {
        return std::fread(out, 1, bytes, static_cast<Minimp3*>(self)->file_);
    }
    static int onSeek(uint64_t position, void* self) {
        auto* d = static_cast<Minimp3*>(self);
        return _fseeki64(d->file_, int64_t(d->start_ + position), SEEK_SET);
    }

    std::filesystem::path path_;
    uint64_t start_;
    std::FILE* file_ = nullptr;
    mp3dec_io_t io_{onRead, this, onSeek, this};
    mp3dec_ex_t mp3_{};
    SourceInfo info_;
    double end_ = 0;        // the file's bytes from `start_` on
    uint64_t declared_ = 0; // the samples the Xing/Info frame declares, where the stream ends after an exact seek
    double frameBytes_ = 0;
    std::optional<XingToc> toc_; // read at the first seek
    bool tocRead_ = false;
    std::vector<float> buffer_;
};

} // namespace

std::unique_ptr<IDecoder> openMinimp3(const std::filesystem::path& path, uint32_t lossyBits, uint64_t start) {
    return std::make_unique<Minimp3>(path, lossyBits, start);
}

} // namespace camel::audio
