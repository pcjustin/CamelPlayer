// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// DTS-CD: the chosen WAVE decoder reads the DTS bitstream as 16-bit stereo words, and dts_core turns them into the PCM
// they carry. DTS-CD frames fill the PCM rate exactly, so a frame that starts at word frame n plays from sample n; frames
// are placed by their position, which keeps the length and seeking exact. A disc ripped track by track cuts a frame at
// each track change; told its neighbors, a track completes the frame at its end from the next file and starts after the
// previous file's last frames, so both tracks play as the whole disc does.
#include "Engine/decode/DecoderImpl.h"

#include <dts_abi.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "Engine/decode/FormatProbe.h"
#include "Engine/decode/Quantize.h"

namespace camel::audio {
namespace {

constexpr size_t kWordFrames = 4096; // read from the WAVE decoder at a time
constexpr uint64_t kBytesPerWordFrame = 4;
// Read before a seek target, so the frame holding it is found and the synthesis filter has settled.
constexpr uint64_t kPreroll = 8192;
constexpr size_t kHeaderBytes = 64; // enough of a frame to read its header
// How far the layout is looked for: the minute of a rip's leading silence that FormatProbe passes over, then room for the
// probed frames at the largest a frame can be.
constexpr int64_t kProbeBytes = int64_t{44100} * 4 * 60 + int64_t{64} * 32768;
constexpr size_t kLongestSyncBytes = 6; // what can start a sync word that the next read completes
constexpr size_t kWordBytes = 2;

struct FreeDts {
    void operator()(DtsDecoder* decoder) const { dts_free(decoder); }
};

class DtsStage final : public IDecoder {
public:
    DtsStage(std::unique_ptr<IDecoder> words, uint32_t bits, WordsOpener openWords)
        : words_(std::move(words)), openWords_(std::move(openWords)), decoder_(dts_new()) {
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

    void playsOnInto(const std::filesystem::path& next) override {
        std::lock_guard lock(neighborsMutex_);
        nextFile_ = next;
    }

    // Told before the first read.
    void followsOn(const std::filesystem::path& previous) override {
        {
            std::lock_guard lock(neighborsMutex_);
            previousFile_ = previous;
        }
        restart(0);
    }

private:
    std::filesystem::path neighbor(const std::filesystem::path& file) {
        std::lock_guard lock(neighborsMutex_);
        return file;
    }

    void startReadingAt(uint64_t wordFrame) {
        words_->seek(wordFrame);
        dts_reset(decoder_.get());
        bytes_.clear();
        bytesFrom_ = int64_t(wordFrame * kBytesPerWordFrame);
        wordsDone_ = false;
        joiningPrevious_ = false;
        joinedNext_ = false;
    }

    // The previous track's last word frames before the file's own: its frames settle the filters and the frame cut at
    // the track change is decoded whole, if it is one (decodeNextFrame checks).
    void prependPrevious() {
        const auto previous = neighbor(previousFile_);
        if (previous.empty()) return;
        try {
            if (probeFormat(previous) != AudioFormat::Dts) return;
            const auto words = openWords_(previous);
            const uint64_t total = words->info().totalFrames;
            words->seek(total > kPreroll ? total - kPreroll : 0);
            std::vector<uint8_t> tail;
            while (appendWords(*words, tail)) {
            }
            if (tail.empty()) return;
            tail.insert(tail.end(), bytes_.begin(), bytes_.end());
            bytes_ = std::move(tail);
            bytesFrom_ -= int64_t(bytes_.size());
            joiningPrevious_ = true;
        } catch (const Error&) {
            // a previous track that cannot be read is not joined
        }
    }

    // Ends the join with the previous track: what is left of its bytes goes, and the filters start afresh.
    void leavePrevious() {
        if (bytesFrom_ < 0) dropBytes(std::min(bytes_.size(), size_t(-bytesFrom_)));
        dts_reset(decoder_.get());
        joiningPrevious_ = false;
    }

    // Completes the frame cut at the file's end, `frameBytes` long, from the next track, when the next track's own first
    // frame starts right after it: the next file goes on with this one. False when it does not, or was tried.
    bool joinNext(size_t frameBytes) {
        if (joinedNext_ || bytesFrom_ < 0 || bytes_.size() >= frameBytes) return false;
        joinedNext_ = true;
        const auto next = neighbor(nextFile_);
        if (next.empty()) return false;
        try {
            if (probeFormat(next) != AudioFormat::Dts) return false;
            const auto words = openWords_(next);
            const size_t missing = frameBytes - bytes_.size();
            std::vector<uint8_t> head;
            while (head.size() < missing + kHeaderBytes && appendWords(*words, head)) {
            }
            if (head.size() < missing + kHeaderBytes || dts_find_sync(head.data() + missing, head.size() - missing, 0) != 0)
                return false;
            bytes_.insert(bytes_.end(), head.begin(), head.begin() + ptrdiff_t(missing));
            return true;
        } catch (const Error&) {
            return false;
        }
    }

    // The layout comes from the first two frames that follow each other at the PCM rate, one sample per word frame: a
    // frame near the start that does not decode, or decodes apart from the others, decides nothing.
    void probe(uint32_t wordRate) {
        constexpr size_t kProbedFrames = 64;
        struct Frame {
            int64_t start = 0;
            uint32_t samples = 0, rate = 0, channels = 0, mask = 0;
        };
        std::optional<Frame> previous;
        size_t decoded = 0;
        bool found = false;
        bytesFrom_ = 0;
        onFrame_ = [&](int64_t start, const DtsPcm& pcm) {
            ++decoded;
            const Frame frame{start, pcm.frames, pcm.sample_rate, pcm.channels, pcm.channel_mask};
            if (previous && previous->rate == wordRate && frame.rate == wordRate && frame.channels == previous->channels &&
                frame.mask == previous->mask && start - previous->start == int64_t(previous->samples)) {
                info_.sampleRate = frame.rate;
                info_.channels = frame.channels;
                info_.channelMask = frame.mask;
                found = true;
            }
            previous = frame;
        };
        readLimit_ = kProbeBytes;
        while (!found && decoded < kProbedFrames && decodeNextFrame()) {
        }
        readLimit_.reset();
        onFrame_ = nullptr;
        if (!decoded) throw Error(ErrorCode::UnsupportedStream, "no DTS frames found");
        if (!found) throw Error(ErrorCode::UnsupportedStream, "the DTS frames do not follow the PCM rate");
    }

    // Decoding continues from `frame`, reading from shortly before it, which near the start is in the previous track.
    void restart(uint64_t frame) {
        startReadingAt(frame > kPreroll ? frame - kPreroll : 0);
        if (frame <= kPreroll) prependPrevious();
        next_ = frame;
        silence_ = 0;
        out_.clear();
        outPos_ = 0;
        onFrame_ = [this](int64_t start, const DtsPcm& pcm) { place(start, pcm); };
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

    // Appends the next block of `words` to `to` as the file holds them; false at the end of the file.
    bool appendWords(IDecoder& words, std::vector<uint8_t>& to) {
        words16_.resize(kWordFrames * 2);
        const size_t got = words.read(words16_.data(), kWordFrames);
        for (size_t i = 0; i < got * 2; i++) {
            to.push_back(uint8_t(words16_[i]));
            to.push_back(uint8_t(words16_[i] >> 8));
        }
        return got != 0;
    }

    // Appends the next block of the file's words; false at the end of the stream, or past readLimit_.
    bool readWords() {
        if (wordsDone_ || (readLimit_ && bytesFrom_ + int64_t(bytes_.size()) >= *readLimit_)) return false;
        wordsDone_ = !appendWords(*words_, bytes_);
        return !wordsDone_;
    }

    void dropBytes(size_t count) {
        bytes_.erase(bytes_.begin(), bytes_.begin() + ptrdiff_t(count));
        bytesFrom_ += int64_t(count);
    }

    // The word frame bytes_[0] is in, counted back from the file's start within the previous track.
    int64_t wordFrameOfBytes() const {
        constexpr auto word = int64_t(kBytesPerWordFrame);
        return bytesFrom_ >= 0 ? bytesFrom_ / word : -((-bytesFrom_ + word - 1) / word);
    }

    // Decodes the next frame there is; false at the end of the stream. What only looks like a frame is passed over and
    // leaves a gap that place() fills.
    bool decodeNextFrame() {
        for (;;) {
            // The previous track's frames all ended before the file's start: no frame was cut, nothing joins them.
            if (joiningPrevious_ && bytesFrom_ >= 0) leavePrevious();
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
                if (readWords() || joinNext(frame.container_bytes)) continue;
                return false;
            }
            // A frame begun in the previous track and cut at the file's start is that track's only if the file's own
            // first frame starts right after it.
            if (joiningPrevious_ && bytesFrom_ + int64_t(frame.container_bytes) > 0) {
                if (bytes_.size() < frame.container_bytes + kHeaderBytes && readWords()) continue;
                const size_t after = std::min(bytes_.size(), size_t(frame.container_bytes));
                if (dts_find_sync(bytes_.data() + after, bytes_.size() - after, 0) != 0) {
                    leavePrevious();
                    continue;
                }
                joiningPrevious_ = false;
            }
            DtsPcm pcm{};
            pcm.struct_size = sizeof(pcm);
            if (dts_decode(decoder_.get(), bytes_.data(), bytes_.size(), &pcm) != DTS_OK) {
                dropBytes(kWordBytes);
                continue;
            }
            if (onFrame_) onFrame_(wordFrameOfBytes(), pcm);
            dropBytes(frame.container_bytes);
            return true;
        }
    }

    // Appends a decoded frame at its position: silence fills a gap before it, and what precedes next_ is dropped, as are
    // the previous track's frames before the file's start.
    void place(int64_t start, const DtsPcm& pcm) {
        const auto next = int64_t(next_);
        const int64_t end = std::min(start + int64_t(pcm.frames), int64_t(info_.totalFrames));
        // A frame of another layout is passed over as a chance frame is: silence fills its place.
        if (end <= next || start >= end || pcm.channels != info_.channels || pcm.channel_mask != info_.channelMask) return;
        const size_t ch = info_.channels;
        if (start > next) {
            silence_ += uint64_t(start - next); // refill() asks only with out_ empty, so the silence comes before it
            next_ = uint64_t(start);
        }
        out_.insert(out_.end(), pcm.samples + size_t(int64_t(next_) - start) * ch, pcm.samples + size_t(end - start) * ch);
        next_ = uint64_t(end);
    }

    std::unique_ptr<IDecoder> words_;
    WordsOpener openWords_;
    std::unique_ptr<DtsDecoder, FreeDts> decoder_;
    std::mutex neighborsMutex_;
    std::filesystem::path nextFile_, previousFile_;
    bool wordsDone_ = false;
    bool joiningPrevious_ = false;     // bytes_ begins in the previous track, whose cut frame is not yet checked
    bool joinedNext_ = false;          // the frame cut at the end was completed from the next track, or could not be
    int64_t bytesFrom_ = 0;            // stream offset of bytes_[0]; before the file's start in the previous track's bytes
    std::optional<int64_t> readLimit_; // the stream offset probe() reads no further than
    uint64_t next_ = 0;                // track position of the next sample appended to out_
    uint64_t silence_ = 0;             // frames of silence readFloat() gives before out_, where the bitstream has none
    std::function<void(int64_t, const DtsPcm&)> onFrame_;
    std::vector<int32_t> words16_;
    std::vector<uint8_t> bytes_;
    std::vector<float> out_; // interleaved, from outPos_ on
    size_t outPos_ = 0;
    std::vector<float> floats_;
    SourceInfo info_;
};

} // namespace

std::unique_ptr<IDecoder> openDtsStage(std::unique_ptr<IDecoder> words, uint32_t lossyBits, WordsOpener openWords) {
    return std::make_unique<DtsStage>(std::move(words), lossyBits, std::move(openWords));
}

} // namespace camel::audio
