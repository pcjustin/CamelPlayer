// FLAC, ALAC, WAV and AIFF through FFmpeg (libavformat + libavcodec).
#include "Engine/decode/DecoderImpl.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
}

#include <bit>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <vector>

#include "Engine/decode/Quantize.h"
#include "Common/util/Log.h"
#include "Common/util/Text.h"

namespace camel::audio {
namespace {

// FFmpeg logs to stderr by default; send warnings and errors to our log instead.
void routeAvLog() {
    static std::once_flag once;
    std::call_once(once, [] {
        av_log_set_callback([](void*, int level, const char* fmt, va_list ap) {
            if (level > AV_LOG_WARNING) return;
            char buf[512];
            const int length = vsnprintf(buf, sizeof buf, fmt, ap);
            if (length < 0) {
                log::warn("ffmpeg: failed to format log message");
                return;
            }
            log::warn("ffmpeg: %s", buf);
        });
    });
}

std::string avErr(int e) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(e, buf, sizeof buf);
    return buf;
}

class FfmpegDecoder final : public IDecoder {
public:
    FfmpegDecoder(const std::filesystem::path& path, uint32_t lossyBits) {
        routeAvLog();
        std::string utf8 = toUtf8(std::filesystem::absolute(path).wstring());
        int e = avformat_open_input(&fmt_, utf8.c_str(), nullptr, nullptr);
        if (e < 0) throw Error(ErrorCode::FileOpen, "avformat_open_input: " + avErr(e));
        e = avformat_find_stream_info(fmt_, nullptr);
        if (e < 0) fail(ErrorCode::UnsupportedStream, "avformat_find_stream_info: " + avErr(e));
        const AVCodec* codec = nullptr;
        stream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
        if (stream_ < 0 || !codec) fail(ErrorCode::UnsupportedStream, "no audio stream");
        AVStream* st = fmt_->streams[stream_];
        // The float decoders keep full precision until the single quantization below.
        if (st->codecpar->codec_id == AV_CODEC_ID_MP3) codec = avcodec_find_decoder_by_name("mp3float");
        if (st->codecpar->codec_id == AV_CODEC_ID_AAC) codec = avcodec_find_decoder_by_name("aac");
        if (!codec) fail(ErrorCode::UnsupportedStream, "no float decoder for this stream");
        ctx_ = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(ctx_, st->codecpar);
        e = avcodec_open2(ctx_, codec, nullptr);
        if (e < 0) fail(ErrorCode::UnsupportedStream, "avcodec_open2: " + avErr(e));
        pkt_ = av_packet_alloc();
        frame_ = av_frame_alloc();
        info_.sampleRate = uint32_t(ctx_->sample_rate);
        info_.channels = uint32_t(ctx_->ch_layout.nb_channels);
        info_.bitsPerSample = uint32_t(ctx_->bits_per_raw_sample ? ctx_->bits_per_raw_sample : st->codecpar->bits_per_raw_sample);
        if (!info_.bitsPerSample) info_.bitsPerSample = uint32_t(av_get_bits_per_sample(ctx_->codec_id)); // 16-bit PCM
        const AVSampleFormat packed = av_get_packed_sample_fmt(ctx_->sample_fmt);
        lossy_ = (packed == AV_SAMPLE_FMT_FLT || packed == AV_SAMPLE_FMT_DBL) &&
                 (ctx_->codec_id == AV_CODEC_ID_MP3 || ctx_->codec_id == AV_CODEC_ID_AAC);
        if (lossy_) info_.bitsPerSample = lossyBits;
        if (st->duration > 0)
            info_.totalFrames = uint64_t(av_rescale_q(st->duration, st->time_base, AVRational{1, ctx_->sample_rate}));
        if (!info_.sampleRate || !info_.channels || !info_.bitsPerSample || info_.bitsPerSample > 32)
            fail(ErrorCode::UnsupportedStream, "unexpected stream parameters");
    }

    ~FfmpegDecoder() override { close(); }

    const SourceInfo& info() const override { return info_; }

    size_t read(int32_t* dst, size_t frames) override {
        const size_t got = readRaw(dst, frames);
        if (lossy_)
            for (size_t i = 0, n = got * info_.channels; i < n; i++)
                dst[i] = quantize(std::bit_cast<float>(dst[i]), info_.bitsPerSample);
        return got;
    }

    bool readsFloat() const override { return lossy_; }
    size_t readFloat(float* dst, size_t frames) override {
        raw_.resize(frames * info_.channels);
        const size_t got = readRaw(raw_.data(), frames);
        std::memcpy(dst, raw_.data(), got * info_.channels * sizeof(float));
        return got;
    }

    // Integers, or float32 bit patterns for a lossy stream.
    size_t readRaw(int32_t* dst, size_t frames) {
        const size_t ch = info_.channels;
        size_t done = 0;
        while (done < frames) {
            if (pendingPos_ >= pending_.size()) {
                if (!decodeMore()) break;
                continue;
            }
            size_t n = std::min((frames - done) * ch, pending_.size() - pendingPos_);
            std::copy_n(pending_.data() + pendingPos_, n, dst + done * ch);
            pendingPos_ += n;
            done += n / ch;
        }
        return done;
    }

    void seek(uint64_t frame) override {
        AVStream* st = fmt_->streams[stream_];
        // The FLAC demuxer refuses targets inside the last frame or two (EPERM, see docs/Decoder-comparison.md).
        // Then land earlier and let convert() decode and drop up to the exact frame.
        int e = -1;
        for (double backoff : {0.0, 1.0, 5.0, -1.0}) {
            int64_t target = backoff < 0 ? 0 : std::max<int64_t>(0, int64_t(frame) - int64_t(backoff * info_.sampleRate));
            int64_t ts = av_rescale_q(target, AVRational{1, int(info_.sampleRate)}, st->time_base);
            e = av_seek_frame(fmt_, stream_, ts, AVSEEK_FLAG_BACKWARD);
            if (e >= 0) break;
        }
        if (e < 0) throw Error(ErrorCode::SeekFailed, "av_seek_frame: " + avErr(e));
        avcodec_flush_buffers(ctx_);
        pending_.clear();
        pendingPos_ = 0;
        draining_ = false;
        seekTarget_ = int64_t(frame);
    }

private:
    [[noreturn]] void fail(ErrorCode c, const std::string& msg) {
        close();
        throw Error(c, msg);
    }

    void close() {
        av_frame_free(&frame_);
        av_packet_free(&pkt_);
        avcodec_free_context(&ctx_);
        if (fmt_) avformat_close_input(&fmt_);
    }

    // Decodes one more frame into pending_. Returns false at end of stream.
    bool decodeMore() {
        for (;;) {
            int e = avcodec_receive_frame(ctx_, frame_);
            if (e == 0) {
                convert();
                av_frame_unref(frame_);
                return true;
            }
            if (e == AVERROR_EOF) return false;
            if (e != AVERROR(EAGAIN)) throw Error(ErrorCode::DecodeFailed, "avcodec_receive_frame: " + avErr(e));
            if (draining_) return false;
            e = av_read_frame(fmt_, pkt_);
            if (e == AVERROR_EOF) {
                draining_ = true;
                avcodec_send_packet(ctx_, nullptr);
                continue;
            }
            if (e < 0) throw Error(ErrorCode::DecodeFailed, "av_read_frame: " + avErr(e));
            if (pkt_->stream_index == stream_) {
                e = avcodec_send_packet(ctx_, pkt_);
                if (e < 0 && e != AVERROR(EAGAIN)) {
                    av_packet_unref(pkt_);
                    throw Error(ErrorCode::DecodeFailed, "avcodec_send_packet: " + avErr(e));
                }
            }
            av_packet_unref(pkt_);
        }
    }

    void convert() {
        const int ch = int(info_.channels);
        const int n = frame_->nb_samples;
        const auto sf = AVSampleFormat(frame_->format);
        const bool planar = av_sample_fmt_is_planar(sf);
        const AVSampleFormat packed = av_get_packed_sample_fmt(sf);
        const bool isFloat = packed == AV_SAMPLE_FMT_FLT || packed == AV_SAMPLE_FMT_DBL;
        if (packed != AV_SAMPLE_FMT_S16 && packed != AV_SAMPLE_FMT_S32 && !(lossy_ && isFloat))
            throw Error(ErrorCode::UnsupportedStream, std::string("unexpected sample format ") + av_get_sample_fmt_name(sf));
        const int shift = isFloat ? 0 : (packed == AV_SAMPLE_FMT_S32 ? 32 : 16) - int(info_.bitsPerSample);
        int skip = 0;
        if (seekTarget_ >= 0) {
            int64_t pts = frame_->best_effort_timestamp != AV_NOPTS_VALUE ? frame_->best_effort_timestamp : frame_->pts;
            int64_t start = pts == AV_NOPTS_VALUE
                                ? seekTarget_
                                : av_rescale_q(pts, fmt_->streams[stream_]->time_base, AVRational{1, int(info_.sampleRate)});
            int64_t drop = seekTarget_ - start;
            if (drop >= n) { // whole frame before the target
                pending_.clear();
                pendingPos_ = 0;
                return;
            }
            skip = drop > 0 ? int(drop) : 0;
            seekTarget_ = -1;
        }
        pending_.resize(size_t(n - skip) * ch);
        int32_t* out = pending_.data();
        for (int i = skip; i < n; i++)
            for (int c = 0; c < ch; c++) {
                const int index = planar ? i : i * ch + c;
                const uint8_t* plane = frame_->extended_data[planar ? c : 0];
                if (packed == AV_SAMPLE_FMT_FLT) {
                    *out++ = std::bit_cast<int32_t>(reinterpret_cast<const float*>(plane)[index]);
                    continue;
                }
                if (packed == AV_SAMPLE_FMT_DBL) {
                    *out++ = std::bit_cast<int32_t>(float(reinterpret_cast<const double*>(plane)[index]));
                    continue;
                }
                int32_t v;
                if (packed == AV_SAMPLE_FMT_S32)
                    v = planar ? reinterpret_cast<const int32_t*>(frame_->extended_data[c])[i]
                               : reinterpret_cast<const int32_t*>(frame_->extended_data[0])[i * ch + c];
                else
                    v = planar ? reinterpret_cast<const int16_t*>(frame_->extended_data[c])[i]
                               : reinterpret_cast<const int16_t*>(frame_->extended_data[0])[i * ch + c];
                *out++ = v >> shift;
            }
        pendingPos_ = 0;
    }

    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* ctx_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVFrame* frame_ = nullptr;
    int stream_ = -1;
    bool draining_ = false;
    int64_t seekTarget_ = -1;
    bool lossy_ = false;           // float decoder output, quantized to info_.bitsPerSample
    std::vector<int32_t> pending_; // float32 bit patterns for a lossy stream
    std::vector<int32_t> raw_;
    size_t pendingPos_ = 0;
    SourceInfo info_;
};

} // namespace

std::unique_ptr<IDecoder> openFfmpeg(const std::filesystem::path& path, uint32_t lossyBits) {
    return std::make_unique<FfmpegDecoder>(path, lossyBits);
}

} // namespace camel::audio
