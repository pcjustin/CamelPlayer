// Conversion through the Rust Engine into simulated devices (docs/Plan.md §5.2, §5.3): what reaches the device when it
// does not take the source as it is, and that nothing is converted without "convert when unsupported".
#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <numbers>
#include <optional>
#include <string>
#include <thread>

#include "Engine/PlaybackEngine.h"
#include "Engine/SamplePacker.h"
#include "OutputBridge/CaptureOutput.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::audio;

namespace {

struct Device {
    std::vector<uint32_t> rates;
    std::vector<SampleType> types;
    bool takes(const DeviceFormat& f) const {
        return std::find(rates.begin(), rates.end(), f.sampleRate) != rates.end() &&
               std::find(types.begin(), types.end(), f.type) != types.end();
    }
};

struct Played {
    EngineSnapshot snapshot;
    std::optional<OutputPlan> plan; // while playing; it is cleared once the stream stops
    std::vector<int32_t> samples;   // unpacked at the device's container depth; float devices as 24-bit integers
    std::vector<float> floats;
};

std::vector<int32_t> tone(uint32_t rate, uint32_t channels, uint32_t bits, double seconds, double level = 0.5) {
    const size_t frames = size_t(seconds * rate);
    std::vector<int32_t> out(frames * channels);
    const double full = std::ldexp(1.0, int(bits) - 1);
    for (size_t i = 0; i < frames; i++)
        for (uint32_t c = 0; c < channels; c++)
            out[i * channels + c] =
                int32_t(std::lround(level * full * std::sin(2 * std::numbers::pi * (997.0 + 101.0 * c) * double(i) / rate)));
    return out;
}

std::filesystem::path wav(const std::string& name, const std::vector<int32_t>& samples, uint32_t rate, uint32_t bits,
                          uint32_t channels) {
    const auto path = testutil::tempDir() / ("conversion-" + name + ".wav");
    testaudio::writeWav(path, samples, rate, bits, channels);
    return path;
}

// `during` runs while the file plays; it needs a device paced like real hardware, which `during` implies.
Played play(const std::filesystem::path& file, const Device& device, bool convert,
            const std::function<void(PlaybackEngine&)>& during = {}) {
    const auto out = testutil::tempDir() / "conversion-capture.raw";
    std::filesystem::remove(out);
    Settings settings;
    settings.convertWhenUnsupported = convert;
    CaptureOptions options;
    options.file = out;
    options.accepts = [&device](const DeviceFormat& f) { return device.takes(f); };
    options.realtime = bool(during);
    Played played;
    {
        PlaybackEngine engine(settings, [&](const Settings&) { return createCaptureOutput(options); });
        engine.openAndPlay(file);
        engine.waitIdle();
        played.plan = engine.snapshot().plan;
        if (during) during(engine);
        EXPECT_TRUE(engine.waitForState(PlayState::Stopped, std::chrono::seconds(30)));
        played.snapshot = engine.snapshot();
    }
    std::ifstream in(out, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (bytes.empty() || !played.plan) return played;
    const SampleType type = played.plan->format.type;
    if (type == SampleType::Float32) {
        played.floats.resize(bytes.size() / 4);
        std::memcpy(played.floats.data(), bytes.data(), bytes.size());
    } else {
        played.samples.resize(bytes.size() / bytesPerSample(type));
        const uint32_t bits = type == SampleType::Int16 ? 16 : 24;
        unpackSamples(reinterpret_cast<const uint8_t*>(bytes.data()), played.samples.size(), bits, type, played.samples.data());
    }
    std::filesystem::remove(out);
    return played;
}

int32_t maxDifference(const std::vector<int32_t>& a, const std::vector<int32_t>& b) {
    int32_t worst = 0;
    for (size_t i = 0; i < std::min(a.size(), b.size()); i++) worst = std::max(worst, std::abs(a[i] - b[i]));
    return worst;
}

} // namespace

TEST(Conversion, OtherRatesAreRefusedWithoutPermission) {
    const auto file = wav("44k", tone(44100, 2, 24, 0.5), 44100, 24, 2);
    const auto played = play(file, {{48000}, {SampleType::Int24}}, false);
    EXPECT_EQ(played.snapshot.error, ErrorCode::FormatNotSupported);
    EXPECT_NE(played.snapshot.errorDetail.find("convert when unsupported"), std::string::npos) << played.snapshot.errorDetail;
    EXPECT_TRUE(played.samples.empty());
}

TEST(Conversion, ResamplesToTheRateTheDeviceTakes) {
    const double seconds = 1.0;
    const auto file = wav("44k-resample", tone(44100, 2, 24, seconds), 44100, 24, 2);
    const auto played = play(file, {{48000, 96000}, {SampleType::Int24}}, true);
    ASSERT_FALSE(played.snapshot.error) << played.snapshot.errorDetail;
    ASSERT_TRUE(played.plan);
    EXPECT_EQ(played.plan->format.sampleRate, 48000u); // the lowest other-family rate at or above the source
    EXPECT_TRUE(played.plan->resample);
    EXPECT_EQ(played.plan->state, PipelineState::Processed);
    EXPECT_EQ(played.samples.size() / 2, 48000u);
    // Compared with the ideal tone at the new rate, inside the edges.
    const auto ideal = tone(48000, 2, 24, seconds);
    double signal = 0, noise = 0;
    for (size_t i = size_t(2000) * 2; i < size_t(46000) * 2; i++) {
        signal += double(ideal[i]) * ideal[i];
        noise += (double(played.samples[i]) - ideal[i]) * (double(played.samples[i]) - ideal[i]);
    }
    EXPECT_GT(10 * std::log10(signal / noise), 90.0);
}

TEST(Conversion, MonoIsDuplicatedBitForBit) {
    const auto source = tone(48000, 1, 16, 0.5);
    const auto played = play(wav("mono", source, 48000, 16, 1), {{48000}, {SampleType::Int16}}, false);
    ASSERT_FALSE(played.snapshot.error) << played.snapshot.errorDetail;
    EXPECT_EQ(played.plan->state, PipelineState::BitPerfect);
    EXPECT_EQ(played.plan->state, PipelineState::BitPerfect);
    ASSERT_EQ(played.samples.size(), source.size() * 2);
    for (size_t i = 0; i < source.size(); i++) {
        ASSERT_EQ(played.samples[i * 2], source[i]);
        ASSERT_EQ(played.samples[i * 2 + 1], source[i]);
    }
}

TEST(Conversion, SurroundDownmixesOnlyWithPermission) {
    const auto source = tone(48000, 6, 24, 0.5, 0.2);
    const auto file = wav("surround", source, 48000, 24, 6);
    const Device device{{48000}, {SampleType::Int24}};
    const auto refused = play(file, device, false);
    EXPECT_EQ(refused.snapshot.error, ErrorCode::FormatNotSupported);

    const auto played = play(file, device, true);
    ASSERT_FALSE(played.snapshot.error) << played.snapshot.errorDetail;
    EXPECT_TRUE(played.plan->downmix);
    const double a = std::numbers::sqrt2 / 2;
    std::vector<int32_t> expected;
    for (size_t i = 0; i < source.size() / 6; i++) {
        const auto* f = &source[i * 6]; // L R C LFE Ls Rs
        expected.push_back(int32_t(std::lround(f[0] + a * f[2] + a * f[4])));
        expected.push_back(int32_t(std::lround(f[1] + a * f[2] + a * f[5])));
    }
    ASSERT_EQ(played.samples.size(), expected.size());
    EXPECT_LE(maxDifference(played.samples, expected), 1); // TPDF dither of one LSB
}

TEST(Conversion, BitReductionDithersToTheDeviceDepth) {
    const auto source = tone(48000, 2, 24, 0.5);
    const auto played = play(wav("reduce", source, 48000, 24, 2), {{48000}, {SampleType::Int16}}, true);
    ASSERT_FALSE(played.snapshot.error) << played.snapshot.errorDetail;
    EXPECT_TRUE(played.plan->reduce);
    ASSERT_EQ(played.samples.size(), source.size());
    std::vector<int32_t> rounded(source.size());
    for (size_t i = 0; i < source.size(); i++) rounded[i] = int32_t(std::lround(source[i] / 256.0));
    EXPECT_LE(maxDifference(played.samples, rounded), 1);
}

TEST(Conversion, FloatOnlyDevicesGetExactFloats) {
    const auto source = tone(96000, 2, 24, 0.5);
    const auto played = play(wav("float", source, 96000, 24, 2), {{96000}, {SampleType::Float32}}, false);
    ASSERT_FALSE(played.snapshot.error) << played.snapshot.errorDetail;
    EXPECT_EQ(played.plan->state, PipelineState::LosslessConversion);
    ASSERT_EQ(played.floats.size(), source.size());
    for (size_t i = 0; i < source.size(); i++) ASSERT_EQ(double(played.floats[i]) * 8388608.0, double(source[i]));
}

TEST(Conversion, ResampledTracksAreGaplessAndPositionsCountSourceFrames) {
    const auto first = wav("gapless-a", tone(44100, 2, 24, 0.5), 44100, 24, 2);
    const auto second = wav("gapless-b", tone(44100, 2, 24, 0.5, 0.25), 44100, 24, 2);
    uint64_t sourceTotal = 0;
    const auto played = play(first, {{48000}, {SampleType::Int24}}, true, [&](PlaybackEngine& engine) {
        bool queued = false;
        for (int i = 0; i < 400 && !queued; ++i) {
            queued = engine.queueNext(second);
            if (!queued) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        EXPECT_TRUE(queued);
        sourceTotal = engine.snapshot().total;
    });
    ASSERT_FALSE(played.snapshot.error) << played.snapshot.errorDetail;
    EXPECT_EQ(sourceTotal, 22050u); // the source's length, not the device's
    EXPECT_EQ(played.snapshot.transitions, 1u);
    EXPECT_EQ(played.samples.size() / 2, 2 * 24000u); // each track resampled to exactly its length at 48 kHz
}
