// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Writes the encoded files in tests/fixtures with FFmpeg's encoders, and the FFmpeg decode the DTS tests compare with.
// Not part of the solution: the tests only read what it wrote. generate <output directory>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

#include "Common/util/Text.h"
#include "Tools/testaudio/TestAudio.h"

using camel::audio::testaudio::GenSpec;
using camel::audio::testaudio::makeSignal;
using camel::audio::testaudio::writeWav;
namespace fs = std::filesystem;

namespace {

void check(int result, const char* what) {
    if (result < 0) throw std::runtime_error(std::string(what) + " failed");
}

// Encodes right-aligned `bits`-bit `samples` with an FFmpeg encoder into `muxer`'s container.
void encode(const fs::path& path, const char* muxer, AVCodecID codecId, AVSampleFormat sampleFormat, int64_t bitRate,
            const std::vector<int32_t>& samples, uint32_t sampleRate, uint32_t bits, uint32_t channels) {
    AVFormatContext* oc = nullptr;
    const std::string utf8 = camel::toUtf8(path.wstring());
    check(avformat_alloc_output_context2(&oc, nullptr, muxer, utf8.c_str()), "output context");
    const AVCodec* codec = avcodec_find_encoder(codecId);
    check(codec ? 0 : -1, "encoder");
    AVStream* st = avformat_new_stream(oc, nullptr);
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    check(st && ctx ? 0 : -1, "stream");
    ctx->sample_rate = int(sampleRate);
    av_channel_layout_default(&ctx->ch_layout, int(channels));
    ctx->sample_fmt = sampleFormat;
    ctx->bits_per_raw_sample = int(bits);
    if (bitRate) ctx->bit_rate = bitRate;
    ctx->time_base = AVRational{1, int(sampleRate)};
    if (oc->oformat->flags & AVFMT_GLOBALHEADER) ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    check(avcodec_open2(ctx, codec, nullptr), "encoder open");
    check(avcodec_parameters_from_context(st->codecpar, ctx), "stream parameters");
    st->time_base = ctx->time_base;
    check(avio_open(&oc->pb, utf8.c_str(), AVIO_FLAG_WRITE), "file open");
    check(avformat_write_header(oc, nullptr), "header");
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    const auto drain = [&] {
        for (;;) {
            const int r = avcodec_receive_packet(ctx, pkt);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return;
            check(r, "encode");
            av_packet_rescale_ts(pkt, ctx->time_base, st->time_base);
            pkt->stream_index = st->index;
            check(av_interleaved_write_frame(oc, pkt), "write");
        }
    };
    const size_t frames = samples.size() / channels;
    const size_t frameSize = ctx->frame_size > 0 ? size_t(ctx->frame_size) : 4096;
    for (size_t start = 0; start < frames; start += frameSize) {
        const size_t n = std::min(frameSize, frames - start);
        frame->nb_samples = int(n);
        frame->format = ctx->sample_fmt;
        frame->sample_rate = ctx->sample_rate;
        check(av_channel_layout_copy(&frame->ch_layout, &ctx->ch_layout), "layout");
        check(av_frame_get_buffer(frame, 0), "frame buffer");
        for (uint32_t c = 0; c < channels; c++)
            for (size_t i = 0; i < n; i++) {
                const int32_t s = samples[(start + i) * channels + c];
                if (sampleFormat == AV_SAMPLE_FMT_FLTP)
                    reinterpret_cast<float*>(frame->data[c])[i] = float(std::ldexp(double(s), -int(bits - 1)));
                else if (sampleFormat == AV_SAMPLE_FMT_S16P)
                    reinterpret_cast<int16_t*>(frame->data[c])[i] = int16_t(s);
                else
                    reinterpret_cast<int32_t*>(frame->data[c])[i] = int32_t(uint32_t(s) << (32 - bits)); // left-aligned
            }
        frame->pts = int64_t(start);
        check(avcodec_send_frame(ctx, frame), "send frame");
        av_frame_unref(frame);
        drain();
    }
    check(avcodec_send_frame(ctx, nullptr), "flush");
    drain();
    check(av_write_trailer(oc), "trailer");
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    avio_closep(&oc->pb);
    avformat_free_context(oc);
}

void writeAlac(const fs::path& path, const std::vector<int32_t>& samples, const GenSpec& s) {
    encode(path, "ipod", AV_CODEC_ID_ALAC, s.bits == 16 ? AV_SAMPLE_FMT_S16P : AV_SAMPLE_FMT_S32P, 0, samples, s.sampleRate,
           s.bits, s.channels);
}

void writeAac(const fs::path& path, const std::vector<int32_t>& samples, uint32_t bits, bool adts) {
    encode(path, adts ? "adts" : "ipod", AV_CODEC_ID_AAC, AV_SAMPLE_FMT_FLTP, 256000, samples, 44100, bits, 2);
}

// Two inharmonic, slowly modulated tones, as LossyDecoderTest measures them.
std::vector<int32_t> tones(double amplitude) {
    constexpr uint32_t kRate = 44100, kBits = 24, kChannels = 2;
    const auto frames = size_t(3.0 * kRate);
    std::vector<int32_t> out(frames * kChannels);
    const double full = std::ldexp(1.0, int(kBits) - 1) - 1;
    const auto clip = int32_t(full);
    for (size_t i = 0; i < frames; i++)
        for (uint32_t c = 0; c < kChannels; c++) {
            const double t = double(i) / kRate;
            const double phase = 0.4 * c;
            const double v =
                0.35 * std::sin(2 * std::numbers::pi * 440.7 * t + 0.3 * std::sin(2 * std::numbers::pi * 0.7 * t) + phase) +
                0.25 * std::sin(2 * std::numbers::pi * 1987.3 * t + phase) *
                    (0.6 + 0.4 * std::sin(2 * std::numbers::pi * 1.9 * t));
            out[i * kChannels + c] = std::clamp(int32_t(std::lround(amplitude * v * full)), -clip, clip);
        }
    return out;
}

constexpr int kDtsRate = 44100;
constexpr size_t kDtsFrameBytes = 1792; // 512 samples at 1234.8 kbit/s: 1024 words of 14 bits

std::vector<std::string> encodeDts(double seconds) {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_DTS);
    check(codec ? 0 : -1, "DTS encoder");
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    ctx->sample_rate = kDtsRate;
    ctx->sample_fmt = AV_SAMPLE_FMT_S32;
    ctx->bit_rate = 1234800;
    ctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    av_channel_layout_from_mask(&ctx->ch_layout, AV_CH_LAYOUT_5POINT1);
    check(avcodec_open2(ctx, codec, nullptr), "opening the DTS encoder");
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    std::vector<std::string> packets;
    const auto drain = [&] {
        while (avcodec_receive_packet(ctx, packet) == 0) {
            packets.emplace_back(reinterpret_cast<char*>(packet->data), size_t(packet->size));
            av_packet_unref(packet);
        }
    };
    const int frames = int(seconds * kDtsRate) / ctx->frame_size;
    for (int n = 0; n < frames; n++) {
        frame->nb_samples = ctx->frame_size;
        frame->format = AV_SAMPLE_FMT_S32;
        check(av_channel_layout_copy(&frame->ch_layout, &ctx->ch_layout), "copying the layout");
        check(av_frame_get_buffer(frame, 0), "allocating a frame");
        auto* samples = reinterpret_cast<int32_t*>(frame->data[0]);
        for (int i = 0; i < ctx->frame_size; i++)
            for (int c = 0; c < 6; c++) {
                const double t = double(n * ctx->frame_size + i) / kDtsRate;
                samples[i * 6 + c] =
                    int32_t(std::lround(0.2 * 2147483647.0 * std::sin(2 * std::numbers::pi * (300.0 + 200.0 * c) * t)));
            }
        check(avcodec_send_frame(ctx, frame), "encoding");
        av_frame_unref(frame);
        drain();
    }
    check(avcodec_send_frame(ctx, nullptr), "flushing the encoder");
    drain();
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return packets;
}

// FFmpeg's decode of the same frames, interleaved L R C LFE Ls Rs.
std::vector<float> decodeDts(const std::vector<std::string>& packets) {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_DTS);
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    check(avcodec_open2(ctx, codec, nullptr), "opening the DTS decoder");
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    std::vector<float> out;
    const auto take = [&] {
        while (avcodec_receive_frame(ctx, frame) == 0) {
            for (int i = 0; i < frame->nb_samples; i++)
                for (int c = 0; c < frame->ch_layout.nb_channels; c++)
                    out.push_back(reinterpret_cast<const float*>(frame->extended_data[c])[i]);
            av_frame_unref(frame);
        }
    };
    for (const auto& data : packets) {
        check(av_new_packet(packet, int(data.size())), "allocating a packet");
        std::copy(data.begin(), data.end(), packet->data);
        check(avcodec_send_packet(ctx, packet), "decoding");
        av_packet_unref(packet);
        take();
    }
    check(avcodec_send_packet(ctx, nullptr), "flushing the decoder");
    take();
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return out;
}

// The big-endian DTS stream in 14-bit words, sign-extended to 16 bits, as DTS-CD stores it.
std::vector<int32_t> pack14(const std::vector<std::string>& packets) {
    std::vector<int32_t> words;
    uint32_t bits = 0;
    int count = 0;
    for (std::string data : packets) {
        if (data.size() > kDtsFrameBytes) throw std::runtime_error("a frame larger than DTS-CD's");
        data.resize(kDtsFrameBytes, '\0');
        for (const char byte : data) {
            bits = (bits << 8) | uint8_t(byte);
            count += 8;
            if (count >= 14) {
                count -= 14;
                const uint32_t word = (bits >> count) & 0x3FFF;
                words.push_back(int32_t(word << 18) >> 18);
            }
        }
    }
    return words;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::puts("usage: generate <output directory>");
        return 2;
    }
    const fs::path out = argv[1];
    fs::create_directories(out);
    for (const GenSpec& spec :
         {GenSpec{"16-44.1k", 44100, 16, 2, 0.5}, GenSpec{"24-96k", 96000, 24, 2, 0.25}, GenSpec{"24-192k", 192000, 24, 2, 0.25}})
        writeAlac(out / ("alac-" + spec.name + ".m4a"), makeSignal(spec), spec);
    const GenSpec shortSpec{"short", 44100, 16, 2, 0.1};
    const auto shortSignal = makeSignal(shortSpec);
    writeAlac(out / "alac-short.m4a", shortSignal, shortSpec);
    writeAac(out / "aac-short.m4a", shortSignal, 16, false);
    writeAac(out / "aac-short.aac", shortSignal, 16, true);
    writeAac(out / "aac.m4a", tones(1.0), 24, false);
    writeAac(out / "aac.aac", tones(1.0), 24, true);
    writeAac(out / "aac-loud.m4a", tones(1.6), 24, false);
    const auto packets = encodeDts(0.5);
    writeWav(out / "dts-cd.wav", pack14(packets), kDtsRate, 16, 2);
    const auto reference = decodeDts(packets);
    std::ofstream(out / "dts-cd.f32", std::ios::binary)
        .write(reinterpret_cast<const char*>(reference.data()), std::streamsize(reference.size() * sizeof(float)));
    return 0;
}
