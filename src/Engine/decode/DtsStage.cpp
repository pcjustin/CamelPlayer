// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// DTS-CD: the chosen WAVE decoder reads the DTS bitstream as 16-bit stereo words, and dts_core turns them into the PCM
// they carry. DTS-CD frames fill the PCM rate exactly, so a frame that starts at word frame n plays from sample n; frames
// are placed by their position, which keeps the length and seeking exact.
#include "Engine/decode/DecoderImpl.h"

#include <dts_abi.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "Engine/decode/Quantize.h"

namespace camel::audio {
namespace {

constexpr size_t kWordFrames = 4096; // read from the WAVE decoder at a time
constexpr uint64_t kBytesPerWordFrame = 4;
// Read before a seek target, so the frame holding it is found and the synthesis filter has settled.
constexpr uint64_t kPreroll = 8192;
constexpr size_t kHeaderBytes = 64;     // enough of a frame to read its header
constexpr size_t kLongestSyncBytes = 6; // what can start a sync word that the next read completes
constexpr size_t kWordBytes = 2;

struct FreeDts {
    void operator()(DtsDecoder* decoder) const { dts_free(decoder); }
};

class DtsStage final : public IDecoder {
public:
    DtsStage(std::unique_ptr<IDecoder> words, uint32_t bits) : words_(std::move(words)), decoder_(dts_new()) {
        const SourceInfo& in = words_->info();
        if (words_->readsFloat() || in.bitsPerSample != 16 || in.channels != 2)
            throw Error(ErrorCode::UnsupportedStream, "DTS in a WAVE file must be read as 16-bit stereo PCM");
        if (!decoder_) throw Error(ErrorCode::UnsupportedStream, "no DTS decoder");
        info_ = in;
        info_.bitsPerSample = bits;
        probe(in.sampleRate);
        restart(0);
    }

    const SourceInfo& info() const override { return info_; }

    size_t read(int32_t* dst, size_t frames) override {
        floats_.resize(frames * info_.channels);
        const size_t got = readFloat(floats_.data(), frames);
        for (size_t i = 0, n = got * info_.channels; i < n; i++) dst[i] = quantize(floats_[i], info_.bitsPerSample);
        return got;
    }

    bool readsFloat() const override { return true; }

    size_t readFloat(float* dst, size_t frames) override {
        const size_t ch = info_.channels;
        size_t done = 0;
        while (done < frames) {
            if (silence_) {
                const auto n = size_t(std::min<uint64_t>(frames - done, silence_));
                std::fill_n(dst + done * ch, n * ch, 0.0f);
                silence_ -= n;
                done += n;
                continue;
            }
            if (outPos_ == out_.size()) {
                out_.clear();
                outPos_ = 0;
                if (!refill()) break;
                continue;
            }
            const size_t n = std::min((frames - done) * ch, out_.size() - outPos_);
            std::copy_n(out_.data() + outPos_, n, dst + done * ch);
            outPos_ += n;
            done += n / ch;
        }
        return done;
    }

    void seek(uint64_t frame) override {
        if (frame > info_.totalFrames) throw Error(ErrorCode::SeekFailed, "seek target is past the end");
        restart(frame);
    }

private:
    void startReadingAt(uint64_t wordFrame) {
        words_->seek(wordFrame);
        dts_reset(decoder_.get());
        bytes_.clear();
        bytesFrom_ = wordFrame * kBytesPerWordFrame;
        wordsDone_ = false;
    }

    // The layout comes from the first frames, which must also follow the PCM rate one sample per word frame.
    void probe(uint32_t wordRate) {
        std::vector<std::pair<uint64_t, uint32_t>> frames; // start, samples
        bytesFrom_ = 0;
        onFrame_ = [&](uint64_t start, const DtsPcm& pcm) {
            if (frames.empty()) {
                info_.sampleRate = pcm.sample_rate;
                info_.channels = pcm.channels;
                info_.channelMask = pcm.channel_mask;
            }
            frames.emplace_back(start, pcm.frames);
        };
        while (frames.size() < 2 && decodeNextFrame()) {
        }
        onFrame_ = nullptr;
        if (frames.size() < 2) throw Error(ErrorCode::UnsupportedStream, "no DTS frames found");
        if (info_.sampleRate != wordRate || frames[1].first - frames[0].first != uint64_t(frames[0].second))
            throw Error(ErrorCode::UnsupportedStream, "the DTS frames do not follow the PCM rate");
    }

    // Decoding continues from `frame`, reading from shortly before it.
    void restart(uint64_t frame) {
        startReadingAt(frame > kPreroll ? frame - kPreroll : 0);
        next_ = frame;
        silence_ = 0;
        out_.clear();
        outPos_ = 0;
        onFrame_ = [this](uint64_t start, const DtsPcm& pcm) { place(start, pcm); };
    }

    // Decodes more into out_, after the silence owed before it; false once the stream has ended at the end of the track.
    bool refill() {
        while (out_.empty() && !silence_) {
            if (decodeNextFrame()) continue;
            if (next_ >= info_.totalFrames) return false;
            silence_ = info_.totalFrames - next_; // a short last frame, or a bitstream that stops early
            next_ = info_.totalFrames;
        }
        return true;
    }

    // Appends the next block of words as the file holds them; false at the end of the stream.
    bool readWords() {
        if (wordsDone_) return false;
        words16_.resize(kWordFrames * 2);
        const size_t got = words_->read(words16_.data(), kWordFrames);
        wordsDone_ = got == 0;
        for (size_t i = 0; i < got * 2; i++) {
            bytes_.push_back(uint8_t(words16_[i]));
            bytes_.push_back(uint8_t(words16_[i] >> 8));
        }
        return !wordsDone_;
    }

    void dropBytes(size_t count) {
        bytes_.erase(bytes_.begin(), bytes_.begin() + ptrdiff_t(count));
        bytesFrom_ += count;
    }

    // Decodes the next frame there is; false at the end of the stream. What only looks like a frame is passed over and
    // leaves a gap that place() fills.
    bool decodeNextFrame() {
        for (;;) {
            const int64_t sync = dts_find_sync(bytes_.data(), bytes_.size(), 0);
            if (sync < 0) {
                dropBytes(bytes_.size() - std::min(bytes_.size(), kLongestSyncBytes));
                if (!readWords()) return false;
                continue;
            }
            dropBytes(size_t(sync));
            if (bytes_.size() < kHeaderBytes && readWords()) continue;
            DtsFrameInfo frame{};
            if (dts_frame_info(bytes_.data(), bytes_.size(), &frame) != DTS_OK) {
                dropBytes(std::min(bytes_.size(), kWordBytes));
                continue;
            }
            if (bytes_.size() < frame.container_bytes) {
                if (readWords()) continue;
                return false;
            }
            DtsPcm pcm{};
            pcm.struct_size = sizeof(pcm);
            if (dts_decode(decoder_.get(), bytes_.data(), bytes_.size(), &pcm) != DTS_OK) {
                dropBytes(kWordBytes);
                continue;
            }
            if (onFrame_) onFrame_(bytesFrom_ / kBytesPerWordFrame, pcm);
            dropBytes(frame.container_bytes);
            return true;
        }
    }

    // Appends a decoded frame at its position: silence fills a gap before it, and what precedes next_ is dropped.
    void place(uint64_t start, const DtsPcm& pcm) {
        if (pcm.channels != info_.channels || pcm.channel_mask != info_.channelMask)
            throw Error(ErrorCode::DecodeFailed, "the DTS channel layout changed within the track");
        const size_t ch = info_.channels;
        const uint64_t end = std::min(start + uint64_t(pcm.frames), info_.totalFrames);
        if (end <= next_ || start >= end) return;
        if (start > next_) {
            silence_ += start - next_; // refill() asks only with out_ empty, so the silence comes before it
            next_ = start;
        }
        out_.insert(out_.end(), pcm.samples + size_t(next_ - start) * ch, pcm.samples + size_t(end - start) * ch);
        next_ = end;
    }

    std::unique_ptr<IDecoder> words_;
    std::unique_ptr<DtsDecoder, FreeDts> decoder_;
    bool wordsDone_ = false;
    uint64_t bytesFrom_ = 0; // stream offset of bytes_[0]
    uint64_t next_ = 0;      // track position of the next sample appended to out_
    uint64_t silence_ = 0;   // frames of silence readFloat() gives before out_, where the bitstream has none
    std::function<void(uint64_t, const DtsPcm&)> onFrame_;
    std::vector<int32_t> words16_;
    std::vector<uint8_t> bytes_;
    std::vector<float> out_; // interleaved, from outPos_ on
    size_t outPos_ = 0;
    std::vector<float> floats_;
    SourceInfo info_;
};

} // namespace

std::unique_ptr<IDecoder> openDtsStage(std::unique_ptr<IDecoder> words, uint32_t lossyBits) {
    return std::make_unique<DtsStage>(std::move(words), lossyBits);
}

} // namespace camel::audio
