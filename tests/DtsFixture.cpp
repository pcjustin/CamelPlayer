// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "DtsFixture.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

#include <cmath>
#include <numbers>
#include <stdexcept>
#include <string>

#include "Engine/testing/TestAudio.h"

namespace testutil {
namespace {

constexpr int kRate = 44100;
constexpr int kChannels = 6;
constexpr size_t kFrameBytes = 1792; // 512 samples at 1234.8 kbit/s: 1024 words of 14 bits

void check(int result, const char* what) {
    if (result < 0) throw std::runtime_error(std::string("DTS fixture: ") + what + " failed (" + std::to_string(result) + ")");
}

std::vector<std::string> encode(double seconds) {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_DTS);
    if (!codec) throw std::runtime_error("DTS fixture: no DTS encoder");
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    ctx->sample_rate = kRate;
    ctx->sample_fmt = AV_SAMPLE_FMT_S32;
    ctx->bit_rate = 1234800;
    ctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    av_channel_layout_from_mask(&ctx->ch_layout, AV_CH_LAYOUT_5POINT1);
    check(avcodec_open2(ctx, codec, nullptr), "opening the encoder");
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    std::vector<std::string> packets;
    const auto drain = [&] {
        while (avcodec_receive_packet(ctx, packet) == 0) {
            packets.emplace_back(reinterpret_cast<const char*>(packet->data), size_t(packet->size));
            av_packet_unref(packet);
        }
    };
    const int frames = int(seconds * kRate) / ctx->frame_size;
    for (int n = 0; n < frames; n++) {
        frame->nb_samples = ctx->frame_size;
        frame->format = AV_SAMPLE_FMT_S32;
        check(av_channel_layout_copy(&frame->ch_layout, &ctx->ch_layout), "copying the layout");
        check(av_frame_get_buffer(frame, 0), "allocating a frame");
        auto* samples = reinterpret_cast<int32_t*>(frame->data[0]);
        for (int i = 0; i < ctx->frame_size; i++)
            for (int c = 0; c < kChannels; c++) {
                const double t = double(n * ctx->frame_size + i) / kRate;
                samples[i * kChannels + c] =
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

std::vector<float> decode(const std::vector<std::string>& packets) {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_DTS);
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    check(avcodec_open2(ctx, codec, nullptr), "opening the decoder");
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    std::vector<float> out;
    for (const auto& data : packets) {
        check(av_new_packet(packet, int(data.size())), "allocating a packet");
        std::copy(data.begin(), data.end(), packet->data);
        check(avcodec_send_packet(ctx, packet), "decoding");
        av_packet_unref(packet);
        while (avcodec_receive_frame(ctx, frame) == 0) {
            if (frame->format != AV_SAMPLE_FMT_FLTP || frame->ch_layout.nb_channels != kChannels)
                throw std::runtime_error("DTS fixture: unexpected decoded format");
            for (int i = 0; i < frame->nb_samples; i++)
                for (int c = 0; c < kChannels; c++) out.push_back(reinterpret_cast<const float*>(frame->extended_data[c])[i]);
            av_frame_unref(frame);
        }
    }
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
        if (data.size() > kFrameBytes) throw std::runtime_error("DTS fixture: frame larger than DTS-CD's");
        data.resize(kFrameBytes, '\0');
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

DtsCd writeDtsCd(const std::filesystem::path& path, double seconds) {
    const auto packets = encode(seconds);
    const auto words = pack14(packets);
    camel::audio::testaudio::writeWav(path, words, kRate, 16, 2);
    return {words.size() / 2, decode(packets)};
}

} // namespace testutil
