// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// DTS-CD: the chosen WAVE decoder reads the DTS bitstream as 16-bit stereo words, and libavcodec's DTS decoder turns
// them into the PCM they carry. DTS-CD frames fill the PCM rate exactly, so a frame that starts at word frame n plays
// from sample n; frames are placed by their position, which keeps the length and seeking exact.
#include "Engine/decode/DecoderImpl.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/samplefmt.h>
}

#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "Engine/decode/Quantize.h"

namespace camel::audio {
namespace {

constexpr size_t kWordFrames = 4096; // read from the WAVE decoder at a time
constexpr uint64_t kBytesPerWordFrame = 4;
// Read before a seek target, so the frame holding it is found and the synthesis filter has settled.
constexpr uint64_t kPreroll = 8192;

class DtsStage final : public IDecoder {
public:
    DtsStage(std::unique_ptr<IDecoder> words, uint32_t bits) : words_(std::move(words)) {
        const SourceInfo& in = words_->info();
        if (words_->readsFloat() || in.bitsPerSample != 16 || in.channels != 2)
            throw Error(ErrorCode::UnsupportedStream, "DTS in a WAVE file must be read as 16-bit stereo PCM");
        codec_ = avcodec_find_decoder(AV_CODEC_ID_DTS);
        if (!codec_) throw Error(ErrorCode::UnsupportedStream, "no DTS decoder");
        pkt_ = av_packet_alloc();
        frame_ = av_frame_alloc();
        info_ = in;
        info_.bitsPerSample = bits;
        probe(in.sampleRate);
        restart(0);
    }

    ~DtsStage() override { close(); }
    DtsStage(const DtsStage&) = delete;
    DtsStage& operator=(const DtsStage&) = delete;

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
    void close() {
        av_parser_close(parser_);
        parser_ = nullptr;
        avcodec_free_context(&ctx_);
        av_frame_free(&frame_);
        av_packet_free(&pkt_);
    }

    void openCodec() {
        av_parser_close(parser_);
        avcodec_free_context(&ctx_);
        parser_ = av_parser_init(AV_CODEC_ID_DTS);
        ctx_ = avcodec_alloc_context3(codec_);
        if (!parser_ || !ctx_ || avcodec_open2(ctx_, codec_, nullptr) < 0)
            throw Error(ErrorCode::UnsupportedStream, "cannot open the DTS decoder");
        parserStarted_ = false;
    }

    // The layout comes from the first frames, which must also follow the PCM rate one sample per word frame.
    void probe(uint32_t wordRate) {
        openCodec();
        readFrom_ = 0;
        wordsDone_ = false;
        std::vector<std::pair<uint64_t, int>> frames; // start, samples
        onFrame_ = [&](uint64_t start, const AVFrame& f) {
            if (frames.empty()) {
                info_.sampleRate = uint32_t(f.sample_rate);
                info_.channels = uint32_t(f.ch_layout.nb_channels);
            }
            frames.emplace_back(start, f.nb_samples);
        };
        while (frames.size() < 2 && pump()) {
        }
        onFrame_ = nullptr;
        if (frames.size() < 2) throw Error(ErrorCode::UnsupportedStream, "no DTS frames found");
        if (info_.sampleRate != wordRate || frames[1].first - frames[0].first != uint64_t(frames[0].second))
            throw Error(ErrorCode::UnsupportedStream, "the DTS frames do not follow the PCM rate");
        if (!info_.channels || info_.channels > 8) throw Error(ErrorCode::UnsupportedStream, "unsupported DTS channel layout");
    }

    // Decoding continues from `frame`, reading from shortly before it.
    void restart(uint64_t frame) {
        const uint64_t from = frame > kPreroll ? frame - kPreroll : 0;
        words_->seek(from);
        openCodec();
        readFrom_ = from;
        wordsDone_ = false;
        next_ = frame;
        out_.clear();
        outPos_ = 0;
        onFrame_ = [this](uint64_t start, const AVFrame& f) { place(start, f); };
    }

    // Decodes more into out_; false once the stream has ended and out_ reaches the end of the track.
    bool refill() {
        while (out_.empty()) {
            if (pump()) continue;
            if (next_ >= info_.totalFrames) return false;
            out_.assign(size_t(info_.totalFrames - next_) * info_.channels, 0.0f); // a short last frame
            next_ = info_.totalFrames;
        }
        return true;
    }

    // Feeds one block of words through the parser and decoder; false at the end of the stream.
    bool pump() {
        if (wordsDone_) return false;
        words16_.resize(kWordFrames * 2);
        const size_t got = words_->read(words16_.data(), kWordFrames);
        bytes_.resize(got * kBytesPerWordFrame + AV_INPUT_BUFFER_PADDING_SIZE);
        for (size_t i = 0; i < got * 2; i++) { // the bytes as the file holds them
            bytes_[2 * i] = uint8_t(words16_[i]);
            bytes_[2 * i + 1] = uint8_t(words16_[i] >> 8);
        }
        const int size = int(got * kBytesPerWordFrame);
        int used = 0;
        do {
            uint8_t* packet = nullptr;
            int packetSize = 0;
            // The first call sets the stream offset the parser reports frame positions from.
            const int64_t pos = parserStarted_ ? 0 : int64_t(readFrom_ * kBytesPerWordFrame);
            parserStarted_ = true;
            used += av_parser_parse2(parser_, ctx_, &packet, &packetSize, size ? bytes_.data() + used : nullptr, size - used,
                                     AV_NOPTS_VALUE, AV_NOPTS_VALUE, pos);
            if (packetSize) decode(packet, packetSize, uint64_t(parser_->frame_offset) / kBytesPerWordFrame);
        } while (used < size);
        if (!got) {
            wordsDone_ = true;
            decode(nullptr, 0, next_);
        }
        return true;
    }

    void decode(uint8_t* data, int size, uint64_t start) {
        pkt_->data = data;
        pkt_->size = size;
        // A packet the decoder refuses, such as bytes before the first sync word, leaves a gap that place() fills.
        if (avcodec_send_packet(ctx_, data ? pkt_ : nullptr) < 0) return;
        while (avcodec_receive_frame(ctx_, frame_) == 0) {
            if (onFrame_) onFrame_(start, *frame_);
            start += uint64_t(frame_->nb_samples);
            av_frame_unref(frame_);
        }
    }

    // Appends a decoded frame at its position: silence fills a gap before it, and what precedes next_ is dropped.
    void place(uint64_t start, const AVFrame& f) {
        if (uint32_t(f.ch_layout.nb_channels) != info_.channels)
            throw Error(ErrorCode::DecodeFailed, "the DTS channel layout changed within the track");
        const size_t ch = info_.channels;
        const uint64_t end = std::min(start + uint64_t(f.nb_samples), info_.totalFrames);
        if (end <= next_ || start >= end) return;
        if (start > next_) {
            out_.resize(out_.size() + size_t(start - next_) * ch, 0.0f);
            next_ = start;
        }
        const auto fmt = AVSampleFormat(f.format);
        const AVSampleFormat packed = av_get_packed_sample_fmt(fmt);
        const bool planar = av_sample_fmt_is_planar(fmt);
        if (packed != AV_SAMPLE_FMT_FLT && packed != AV_SAMPLE_FMT_S32 && packed != AV_SAMPLE_FMT_S16)
            throw Error(ErrorCode::DecodeFailed, std::string("unexpected DTS sample format ") + av_get_sample_fmt_name(fmt));
        for (uint64_t i = next_ - start; i < end - start; i++)
            for (size_t c = 0; c < ch; c++) {
                const size_t index = planar ? size_t(i) : size_t(i) * ch + c;
                const uint8_t* plane = f.extended_data[planar ? c : 0];
                out_.push_back(packed == AV_SAMPLE_FMT_FLT ? reinterpret_cast<const float*>(plane)[index]
                               : packed == AV_SAMPLE_FMT_S32
                                   ? float(double(reinterpret_cast<const int32_t*>(plane)[index]) / 2147483648.0)
                                   : float(reinterpret_cast<const int16_t*>(plane)[index]) / 32768.0f);
            }
        next_ = end;
    }

    std::unique_ptr<IDecoder> words_;
    const AVCodec* codec_ = nullptr;
    AVCodecContext* ctx_ = nullptr;
    AVCodecParserContext* parser_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVFrame* frame_ = nullptr;
    bool parserStarted_ = false;
    bool wordsDone_ = false;
    uint64_t readFrom_ = 0; // word frame the parser started at
    uint64_t next_ = 0;     // track position of the next sample appended to out_
    std::function<void(uint64_t, const AVFrame&)> onFrame_;
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
