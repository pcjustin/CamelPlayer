// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// MP3 through dr_mp3 (dr_libs, header-only), which trims LAME/Xing encoder delay and padding itself.
#include "Engine/decode/DecoderImpl.h"
#include "Engine/decode/FormatProbe.h"

#pragma warning(push, 0)
#define DR_MP3_IMPLEMENTATION
#include <dr_mp3.h>
#pragma warning(pop)

#include <algorithm>
#include <cstdio>
#include <optional>
#include <vector>

#include "Common/util/Text.h"
#include "Engine/decode/Quantize.h"

namespace camel::audio {
namespace {

class DrMp3 final : public IDecoder {
public:
    DrMp3(const std::filesystem::path& path, uint32_t lossyBits, uint64_t start) : path_(path), start_(start) {
        if (_wfopen_s(&file_, longPath(path).c_str(), L"rb") != 0 || !file_)
            throw Error(ErrorCode::FileOpen, "dr_mp3: cannot open file");
        if (!onSeek(this, 0, DRMP3_SEEK_SET) || !drmp3_init(&mp3_, onRead, onSeek, onTell, nullptr, this, nullptr)) {
            std::fclose(file_);
            throw Error(ErrorCode::FileOpen, "drmp3_init failed");
        }
        info_.sampleRate = mp3_.sampleRate;
        info_.channels = mp3_.channels;
        info_.bitsPerSample = lossyBits;
        if (!info_.sampleRate || !info_.channels) {
            drmp3_uninit(&mp3_);
            std::fclose(file_);
            throw Error(ErrorCode::UnsupportedStream, "dr_mp3: unexpected stream parameters");
        }
        // A length measured before is the one to show, and places a seek better. Without it or a Xing/Info header
        // dr_mp3 would read the whole file to count its frames.
        if (const auto measured = measuredLength(path))
            info_.totalFrames = *measured;
        else if (mp3_.totalPCMFrameCount != DRMP3_UINT64_MAX)
            info_.totalFrames = drmp3_get_pcm_frame_count(&mp3_);
        // The first audio frame's header, which a Xing/Info frame before it would have cleared.
        if (mp3_.decoder.header[0] == 0xFF)
            frameBytes_ = (info_.sampleRate >= 32000 ? 144.0 : 72.0) * drmp3_hdr_bitrate_kbps(mp3_.decoder.header) * 1000.0 /
                          info_.sampleRate;
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
        const bool estimated = estimateSeekPoint(frame);
        drmp3_bind_seek_table(&mp3_, estimated ? 1 : 0, estimated ? &point_ : nullptr);
        if (!drmp3_seek_to_pcm_frame(&mp3_, frame)) throw Error(ErrorCode::SeekFailed, "drmp3_seek_to_pcm_frame failed");
    }

private:
    // Without a seek table dr_mp3 reads the file up to the target. The table's one point is where the frames from
    // DRMP3_SEEK_LEADING_MP3_FRAMES before the target's, which dr_mp3 decodes to warm up, are estimated to start: by
    // the Xing table, in proportion to the length, or else by the first frame's size, which is every frame's in CBR.
    bool estimateSeekPoint(uint64_t frame) {
        // A Layer III frame's samples: MPEG-1 is 32 kHz and up.
        const uint64_t samples = info_.sampleRate >= 32000 ? 1152 : 576;
        const uint64_t target = (frame + mp3_.delayInPCMFrames) / samples; // the MP3 frame that holds it
        if (target <= DRMP3_SEEK_LEADING_MP3_FRAMES) return false;
        // Halfway into the frame before, so as not to miss the start of the first.
        const double from = double(target - DRMP3_SEEK_LEADING_MP3_FRAMES) - 0.5;
        const auto begin = double(mp3_.streamStartOffset);
        double at = 0;
        if (info_.totalFrames) {
            if (!tocRead_) {
                toc_ = xingToc(path_);
                tocRead_ = true;
            }
            const auto total = double(info_.totalFrames + mp3_.delayInPCMFrames + mp3_.paddingInPCMFrames);
            const double share = std::min(1.0, from * double(samples) / total);
            at = toc_ ? toc_->byteShare(share) * toc_->bytes : begin + share * (double(mp3_.streamLength) - begin);
        } else if (frameBytes_ > 0) {
            at = begin + from * frameBytes_;
        } else {
            return false;
        }
        point_ = {uint64_t(std::max(at, begin)), target * samples, DRMP3_SEEK_LEADING_MP3_FRAMES, 0};
        return true;
    }

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

    std::filesystem::path path_;
    uint64_t start_;
    std::FILE* file_ = nullptr;
    drmp3 mp3_{};
    SourceInfo info_;
    double frameBytes_ = 0;
    std::optional<XingToc> toc_; // read at the first seek
    bool tocRead_ = false;
    drmp3_seek_point point_{};
    std::vector<float> buffer_;
};

} // namespace

std::unique_ptr<IDecoder> openDrMp3(const std::filesystem::path& path, uint32_t lossyBits, uint64_t start) {
    return std::make_unique<DrMp3>(path, lossyBits, start);
}

} // namespace camel::audio
