// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include <fstream>
#include <random>
#include <thread>

#include "Engine/decode/FlacStreamInfo.h"
#include "Engine/FormatRule.h"
#include "Engine/SamplePacker.h"
#include "Common/settings/Settings.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::audio;

TEST(SamplePacker, RoundTripEveryLayoutAndDepth) {
    std::mt19937 rng(1);
    for (SampleType t : {SampleType::Int16, SampleType::Int24, SampleType::Int24In32, SampleType::Int32}) {
        for (uint32_t bits : {16u, 24u}) {
            if (!canCarryBitPerfect(t, bits)) continue;
            std::vector<int32_t> in(1000);
            const int64_t lo = -(int64_t(1) << (bits - 1)), hi = (int64_t(1) << (bits - 1)) - 1;
            std::uniform_int_distribution<int64_t> d(lo, hi);
            for (auto& v : in) v = int32_t(d(rng));
            in[0] = int32_t(lo);
            in[1] = int32_t(hi);
            std::vector<uint8_t> packed(in.size() * bytesPerSample(t));
            packSamples(in.data(), in.size(), bits, t, packed.data());
            std::vector<int32_t> out(in.size());
            unpackSamples(packed.data(), in.size(), bits, t, out.data());
            EXPECT_EQ(in, out) << sampleTypeName(t) << " / " << bits << " bit";
        }
    }
}

TEST(SamplePacker, ZeroPaddingLayout) {
    const int32_t v = 0x123456; // 24-bit sample
    uint8_t b[4];
    packSamples(&v, 1, 24, SampleType::Int24In32, b);
    EXPECT_EQ(b[0], 0x00); // low 8 bits padded with zero, value MSB-aligned
    EXPECT_EQ(b[1], 0x56);
    EXPECT_EQ(b[3], 0x12);
    const int32_t s = -2; // 16-bit into 24 packed
    packSamples(&s, 1, 16, SampleType::Int24, b);
    EXPECT_EQ(b[0], 0x00);
    EXPECT_EQ(b[1], 0xFE);
    EXPECT_EQ(b[2], 0xFF);
    EXPECT_FALSE(canCarryBitPerfect(SampleType::Int16, 24));
    EXPECT_FALSE(canCarryBitPerfect(SampleType::Float32, 16));
}

// Device capability tables measured in docs/Plan.md §15.2.
TEST(FormatRule, PicksSmallestContainerTheDeviceTakes) {
    auto only = [](std::initializer_list<SampleType> list) {
        std::vector<SampleType> types(list);
        return [types](const DeviceFormat& f) { return std::find(types.begin(), types.end(), f.type) != types.end(); };
    };
    SourceInfo s16{44100, 2, 16}, s24{96000, 2, 24};
    auto daveUsbAudio2 = only({SampleType::Int32});
    auto daveChord = only({SampleType::Int16, SampleType::Int24, SampleType::Int32});
    auto cirrus = only({SampleType::Int16, SampleType::Int24In32, SampleType::Int32});
    auto type = [](const SourceInfo& s, std::optional<SampleType> fixed, const auto& device) {
        const auto plan = negotiateOutput(s, false, fixed, false, device);
        return plan ? std::optional(plan->format.type) : std::nullopt;
    };
    EXPECT_EQ(type(s16, std::nullopt, daveUsbAudio2), SampleType::Int32);
    EXPECT_EQ(type(s24, std::nullopt, daveChord), SampleType::Int24);
    EXPECT_EQ(type(s24, std::nullopt, cirrus), SampleType::Int24In32);
    EXPECT_EQ(type(s16, std::nullopt, cirrus), SampleType::Int16);
    EXPECT_EQ(type(s16, SampleType::Int32, cirrus), SampleType::Int32); // fixed in settings
    EXPECT_FALSE(type(s24, SampleType::Int16, cirrus));                 // would truncate without conversion
    const auto mono = negotiateOutput(SourceInfo{44100, 1, 16}, false, std::nullopt, false, cirrus);
    ASSERT_TRUE(mono);
    EXPECT_EQ(mono->format, (DeviceFormat{44100, 2, SampleType::Int16}));
    EXPECT_TRUE(mono->duplicate);
    EXPECT_EQ(mono->state, PipelineState::BitPerfect); // duplicated bit for bit
    const auto floatOnly = negotiateOutput(s24, false, std::nullopt, false, only({SampleType::Float32}));
    ASSERT_TRUE(floatOnly);
    EXPECT_TRUE(floatOnly->widen);
    EXPECT_EQ(floatOnly->state, PipelineState::LosslessConversion);
}

// docs/Plan.md §5.3: the source rate, then with conversion the lowest same-family rate at or above it, the lowest
// other-family rate at or above it, then the highest rate the device takes.
TEST(FormatRule, ConversionFollowsTheRateOrder) {
    auto rates = [](std::initializer_list<uint32_t> list) {
        std::vector<uint32_t> supported(list);
        return [supported](const DeviceFormat& f) {
            return f.type == SampleType::Int24 && std::find(supported.begin(), supported.end(), f.sampleRate) != supported.end();
        };
    };
    auto rate = [](uint32_t source, const auto& device) {
        const auto plan = negotiateOutput(SourceInfo{source, 2, 24}, false, std::nullopt, true, device);
        return plan ? plan->format.sampleRate : 0u;
    };
    EXPECT_EQ(rate(44100, rates({44100, 48000})), 44100u);
    EXPECT_EQ(rate(44100, rates({48000, 96000, 88200})), 88200u);
    EXPECT_EQ(rate(44100, rates({96000, 48000})), 48000u);
    EXPECT_EQ(rate(96000, rates({44100, 192000, 176400})), 192000u);
    EXPECT_EQ(rate(192000, rates({44100, 48000, 96000})), 96000u);
    EXPECT_EQ(rate(768000, rates({44100, 384000})), 384000u);
    EXPECT_FALSE(negotiateOutput(SourceInfo{44100, 2, 24}, false, std::nullopt, false, rates({48000})));
    const auto plan = negotiateOutput(SourceInfo{44100, 2, 24}, false, std::nullopt, true, rates({48000}));
    ASSERT_TRUE(plan);
    EXPECT_TRUE(plan->resample);
    EXPECT_EQ(plan->state, PipelineState::Processed);
}

TEST(FormatRule, DownmixAndBitReductionNeedConversion) {
    auto anyRate = [](std::initializer_list<SampleType> list) {
        std::vector<SampleType> types(list);
        return [types](const DeviceFormat& f) { return std::find(types.begin(), types.end(), f.type) != types.end(); };
    };
    const SourceInfo surround{48000, 6, 24}, hiRes{96000, 2, 24};
    EXPECT_FALSE(negotiateOutput(surround, false, std::nullopt, false, anyRate({SampleType::Int24})));
    const auto downmix = negotiateOutput(surround, false, std::nullopt, true, anyRate({SampleType::Int24}));
    ASSERT_TRUE(downmix);
    EXPECT_EQ(downmix->format, (DeviceFormat{48000, 2, SampleType::Int24}));
    EXPECT_TRUE(downmix->downmix);
    EXPECT_EQ(downmix->state, PipelineState::Processed);
    EXPECT_FALSE(negotiateOutput(SourceInfo{48000, 9, 24}, false, std::nullopt, true, anyRate({SampleType::Int24})));

    EXPECT_FALSE(negotiateOutput(hiRes, false, std::nullopt, false, anyRate({SampleType::Int16})));
    const auto reduced = negotiateOutput(hiRes, false, std::nullopt, true, anyRate({SampleType::Int16}));
    ASSERT_TRUE(reduced);
    EXPECT_TRUE(reduced->reduce);
    EXPECT_EQ(reduced->state, PipelineState::Processed);
    // A wider container or float32 is still preferred over dropping bits.
    const auto widened = negotiateOutput(hiRes, false, std::nullopt, true, anyRate({SampleType::Int16, SampleType::Float32}));
    ASSERT_TRUE(widened);
    EXPECT_EQ(widened->format.type, SampleType::Float32);

    const auto lossy = negotiateOutput(hiRes, true, std::nullopt, false, anyRate({SampleType::Int24}));
    ASSERT_TRUE(lossy);
    EXPECT_EQ(lossy->state, PipelineState::LossySource);
}

TEST(Settings, JsonRoundTrip) {
    Settings s;
    s.backend = Backend::Ks;
    s.decoderByFormat[AudioFormat::Flac] = DecoderKind::Symphonia;
    s.wasapi.eventDriven = false;
    s.wasapi.sampleType = SampleType::Int24In32;
    s.asio.driverClsid = "{ABC}";
    s.asio.bufferSize = 512;
    s.ks.mode = KsMode::Standard;
    s.ks.packetCount = 6;
    s.lastFile = L"D:\\音樂\\測試.flac";
    Settings t = fromJson(toJson(s));
    EXPECT_EQ(t.backend, Backend::Ks);
    EXPECT_EQ(decoderFor(t.decoderByFormat, AudioFormat::Flac), DecoderKind::Symphonia);
    EXPECT_EQ(t.wasapi, s.wasapi);
    EXPECT_EQ(t.asio, s.asio);
    EXPECT_EQ(t.ks, s.ks);
    EXPECT_EQ(t.lastFile, s.lastFile);
}

TEST(Settings, CorruptFileFallsBackToDefaults) {
    auto p = testutil::tempDir() / L"settings-corrupt.json";
    {
        std::ofstream o(p, std::ios::binary);
        o << "{ not json";
    }
    Settings s = loadSettings(p);
    EXPECT_EQ(s.backend, Backend::Wasapi);
    EXPECT_FALSE(std::filesystem::exists(p)); // moved aside as .bad
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(p).concat(L".bad")));
    std::filesystem::remove(std::filesystem::path(p).concat(L".bad"));
}

TEST(FlacStreamInfo, ReadsGeneratedFile) {
    SourceInfo si;
    ASSERT_TRUE(readFlacStreamInfo(testutil::testFile("24-96k"), si));
    EXPECT_EQ(si.sampleRate, 96000u);
    EXPECT_EQ(si.channels, 2u);
    EXPECT_EQ(si.bitsPerSample, 24u);
    EXPECT_EQ(si.totalFrames, 192000u);
    EXPECT_TRUE(si.md5.has_value());
    ASSERT_TRUE(readFlacStreamInfo(testutil::testFile("no-md5"), si));
    EXPECT_FALSE(si.md5.has_value());
}
