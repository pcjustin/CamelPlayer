#include <gtest/gtest.h>

#include <fstream>
#include <random>
#include <thread>

#include "audio/decode/FlacStreamInfo.h"
#include "audio/engine/FormatRule.h"
#include "audio/engine/SamplePacker.h"
#include "settings/Settings.h"
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
    EXPECT_EQ(chooseOutputFormat(s16, std::nullopt, daveUsbAudio2)->type, SampleType::Int32);
    EXPECT_EQ(chooseOutputFormat(s24, std::nullopt, daveChord)->type, SampleType::Int24);
    EXPECT_EQ(chooseOutputFormat(s24, std::nullopt, cirrus)->type, SampleType::Int24In32);
    EXPECT_EQ(chooseOutputFormat(s16, std::nullopt, cirrus)->type, SampleType::Int16);
    EXPECT_EQ(chooseOutputFormat(s16, SampleType::Int32, cirrus)->type, SampleType::Int32);       // fixed in settings
    EXPECT_FALSE(chooseOutputFormat(s24, SampleType::Int16, cirrus).has_value());                 // would truncate
    EXPECT_FALSE(chooseOutputFormat(s24, std::nullopt, only({SampleType::Float32})).has_value()); // no conversion in the MVP
    EXPECT_FALSE(chooseOutputFormat(SourceInfo{44100, 1, 16}, std::nullopt, cirrus).has_value()); // stereo only
}

TEST(Settings, JsonRoundTrip) {
    Settings s;
    s.backend = Backend::Ks;
    s.decoder = DecoderKind::Symphonia;
    s.wasapi.eventDriven = false;
    s.wasapi.sampleType = SampleType::Int24In32;
    s.asio.driverClsid = "{ABC}";
    s.asio.bufferSize = 512;
    s.ks.mode = KsMode::Standard;
    s.ks.packetCount = 6;
    s.lastFile = L"D:\\音樂\\測試.flac";
    Settings t = fromJson(toJson(s));
    EXPECT_EQ(t.backend, Backend::Ks);
    EXPECT_EQ(t.decoder, DecoderKind::Symphonia);
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
