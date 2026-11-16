// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Upsampling per output (docs/Plan.md §6): the rate each mode picks for a source against outputs taking up to
// 192, 384 or 768 kHz, with and without the output's highest PCM rate; DSD converted to PCM under each mode; and the
// sample-rate choice and filter each output keeps in settings.json.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <string>

#include "DsdFixture.h"
#include "Engine/FormatRule.h"
#include "Engine/PlaybackEngine.h"
#include "OutputBridge/CaptureOutput.h"
#include "StartedSnapshot.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::audio;
using namespace std::chrono_literals;

namespace {

constexpr uint32_t kStandardRates[] = {44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000};

bool standardUpTo(const DeviceFormat& f, uint32_t highest) {
    return f.type == SampleType::Int32 && f.sampleRate <= highest &&
           std::ranges::find(kStandardRates, f.sampleRate) != std::end(kStandardRates);
}

// The rate a PCM source plays at, the way the Engine tries the rates a mode lists, without "convert when unsupported".
uint32_t picked(Upsampling mode, uint32_t multiple, uint32_t source, uint32_t deviceHighest, uint32_t limit = 0) {
    const auto supports = [&](const DeviceFormat& f) {
        return (!limit || f.sampleRate <= limit) && standardUpTo(f, deviceHighest);
    };
    for (const uint32_t rate : upsamplingRates(mode, multiple, source, limit))
        if (const auto plan = negotiateOutput({source, 2, 24}, false, std::nullopt, false, supports, rate))
            return plan->format.sampleRate;
    return 0;
}

struct Played {
    EngineSnapshot started;
    EngineSnapshot ended;
};

Played play(const std::filesystem::path& file, const Settings& settings, uint32_t deviceHighest) {
    CaptureOptions options;
    options.file = testutil::tempDir() / "upsampling.raw";
    options.accepts = [deviceHighest](const DeviceFormat& f) { return standardUpTo(f, deviceHighest); };
    PlaybackEngine engine(settings, [&](const Settings&) { return createCaptureOutput(options); });
    testutil::StartedSnapshot started(engine);
    engine.openAndPlay(file);
    engine.waitIdle();
    Played played;
    played.started = started.get().value_or(engine.snapshot());
    EXPECT_TRUE(engine.waitForState(PlayState::Stopped, 30s));
    played.ended = engine.snapshot();
    return played;
}

std::filesystem::path dsd64File() {
    using namespace dsdfixture;
    const auto file = testutil::tempDir() / "upsampling.dsf";
    writeFile(file, dsf(rawDsd(kDsd64 / 8 / 5, 2, 31), {.channels = 2, .rate = kDsd64}));
    return file;
}

} // namespace

TEST(Upsampling, IntegerToTheHighestStaysInTheSourcesFamily) {
    const auto at = [](uint32_t source, uint32_t device, uint32_t limit = 0) {
        return picked(Upsampling::IntegerHighest, 2, source, device, limit);
    };
    EXPECT_EQ(at(44100, 192000), 176400u);
    EXPECT_EQ(at(48000, 192000), 192000u);
    EXPECT_EQ(at(88200, 192000), 176400u);
    EXPECT_EQ(at(96000, 192000), 192000u);
    EXPECT_EQ(at(352800, 192000), 176400u);
    EXPECT_EQ(at(44100, 384000), 352800u);
    EXPECT_EQ(at(48000, 384000), 384000u);
    EXPECT_EQ(at(352800, 384000), 352800u);
    EXPECT_EQ(at(44100, 768000), 705600u);
    EXPECT_EQ(at(96000, 768000), 768000u);
    EXPECT_EQ(at(44100, 768000, 192000), 176400u);
    EXPECT_EQ(at(48000, 768000, 384000), 384000u);
    EXPECT_EQ(at(352800, 768000, 192000), 176400u);
}

TEST(Upsampling, ByAMultipleTakesTheHighestMultipleThatFits) {
    const auto at = [](uint32_t multiple, uint32_t source, uint32_t device, uint32_t limit = 0) {
        return picked(Upsampling::IntegerMultiple, multiple, source, device, limit);
    };
    EXPECT_EQ(at(2, 44100, 192000), 88200u);
    EXPECT_EQ(at(4, 44100, 192000), 176400u);
    EXPECT_EQ(at(8, 44100, 192000), 176400u);
    EXPECT_EQ(at(8, 48000, 384000), 384000u);
    EXPECT_EQ(at(8, 44100, 768000), 352800u);
    EXPECT_EQ(at(4, 88200, 192000), 176400u);
    EXPECT_EQ(at(4, 96000, 768000), 384000u);
    EXPECT_EQ(at(2, 352800, 768000), 705600u);
    EXPECT_EQ(at(2, 352800, 384000), 352800u);
    EXPECT_EQ(at(2, 352800, 192000), 176400u);
    EXPECT_EQ(at(8, 44100, 768000, 192000), 176400u);
    EXPECT_EQ(at(2, 352800, 768000, 192000), 176400u);
}

TEST(Upsampling, AnyRateToTheHighestCrossesFamilies) {
    const auto at = [](uint32_t source, uint32_t device, uint32_t limit = 0) {
        return picked(Upsampling::AnyHighest, 2, source, device, limit);
    };
    for (const uint32_t source : {44100u, 48000u, 88200u, 96000u, 352800u}) {
        EXPECT_EQ(at(source, 192000), 192000u) << source;
        EXPECT_EQ(at(source, 384000), 384000u) << source;
        EXPECT_EQ(at(source, 768000), 768000u) << source;
        EXPECT_EQ(at(source, 768000, 192000), 192000u) << source;
        EXPECT_EQ(at(source, 768000, 176400), 176400u) << source;
    }
}

TEST(Upsampling, OffListsNoRate) {
    EXPECT_TRUE(upsamplingRates(Upsampling::Off, 2, 44100, 0).empty());
    EXPECT_TRUE(upsamplingRates(Upsampling::Off, 2, 352800, 192000).empty());
}

// Choosing a mode is the permission to resample, as a set rate is: it plays with "convert when unsupported" off.
TEST(Upsampling, PlaysResampledWithoutTheConvertSetting) {
    Settings settings;
    settings.convertWhenUnsupported = false;
    settings.wasapi.upsampling = Upsampling::IntegerHighest;
    const auto played = play(testutil::testFile("16-44.1k"), settings, 192000);
    ASSERT_FALSE(played.ended.error) << played.ended.errorDetail;
    ASSERT_TRUE(played.started.plan);
    EXPECT_EQ(played.started.plan->format.sampleRate, 176400u);
    EXPECT_TRUE(played.started.plan->resample);
    EXPECT_EQ(played.started.plan->state, PipelineState::Processed);
    EXPECT_TRUE(played.started.outputStatus.adjustments.empty());
}

TEST(Upsampling, ASetRateWinsOverTheMode) {
    Settings settings;
    settings.backend = Backend::Asio;
    settings.asio.upsampling = Upsampling::AnyHighest;
    settings.asio.fixedRate = 96000;
    const auto played = play(testutil::testFile("16-44.1k"), settings, 768000);
    ASSERT_FALSE(played.ended.error) << played.ended.errorDetail;
    ASSERT_TRUE(played.started.plan);
    EXPECT_EQ(played.started.plan->format.sampleRate, 96000u);
}

TEST(Upsampling, IgnoredInSharedModeAndOnAirPlay) {
    Settings settings;
    settings.wasapi.upsampling = Upsampling::AnyHighest;
    EXPECT_EQ(upsampling(settings), Upsampling::AnyHighest);
    settings.wasapi.shared = true;
    EXPECT_EQ(upsampling(settings), Upsampling::Off);
    settings.backend = Backend::AirPlay;
    EXPECT_EQ(upsampling(settings), Upsampling::Off);
}

// DSD converted to PCM follows the mode as a 44.1 kHz source would, from the decimator's 352.8 kHz.
TEST(Upsampling, DsdToPcmFollowsTheMode) {
    const auto file = dsd64File();
    const auto rateFor = [&](Upsampling mode, uint32_t multiple, uint32_t device, uint32_t fixed = 0) {
        Settings settings;
        settings.backend = Backend::Ks;
        settings.ks.dsdOutput = DsdOutput::Pcm;
        settings.ks.dsdPcmRate = 88200;
        settings.ks.upsampling = mode;
        settings.ks.upsamplingMultiple = multiple;
        settings.ks.fixedRate = fixed;
        settings.convertWhenUnsupported = false;
        const auto played = play(file, settings, device);
        EXPECT_FALSE(played.ended.error) << played.ended.errorDetail;
        EXPECT_TRUE(played.started.plan && played.started.plan->dsd == DsdPath::Pcm);
        return played.started.plan ? played.started.plan->format.sampleRate : 0;
    };
    EXPECT_EQ(rateFor(Upsampling::Off, 2, 192000), 88200u); // its own DSD to PCM rate
    EXPECT_EQ(rateFor(Upsampling::IntegerHighest, 2, 192000), 176400u);
    EXPECT_EQ(rateFor(Upsampling::IntegerHighest, 2, 768000), 705600u);
    EXPECT_EQ(rateFor(Upsampling::IntegerMultiple, 4, 768000), 176400u);
    EXPECT_EQ(rateFor(Upsampling::IntegerMultiple, 8, 384000), 352800u);
    EXPECT_EQ(rateFor(Upsampling::AnyHighest, 2, 384000), 384000u);
    EXPECT_EQ(rateFor(Upsampling::Off, 2, 192000, 96000), 96000u);
}

TEST(Upsampling, RawDsdIsNeverTouched) {
    Settings settings;
    settings.wasapi.upsampling = Upsampling::AnyHighest;
    const auto played = play(dsd64File(), settings, 768000);
    ASSERT_FALSE(played.ended.error) << played.ended.errorDetail;
    ASSERT_TRUE(played.started.plan);
    EXPECT_EQ(played.started.plan->dsd, DsdPath::Dop);
    EXPECT_EQ(played.started.plan->format.sampleRate, 176400u);
}

TEST(UpsamplingSettings, EachOutputKeepsItsChoiceAndFilter) {
    Settings settings;
    settings.wasapi.upsampling = Upsampling::IntegerMultiple;
    settings.wasapi.upsamplingMultiple = 8;
    settings.wasapi.filter = {ResampleQuality::VeryHigh, FilterPhase::Minimum, FilterPassband::Steep};
    settings.asio.upsampling = Upsampling::AnyHighest;
    settings.asio.filter = {ResampleQuality::Quick, FilterPhase::Intermediate, FilterPassband::Gentle};
    settings.ks.upsampling = Upsampling::IntegerHighest;
    settings.network.fixedRate = 192000;
    settings.network.filter.passband = FilterPassband::Gentle;
    SavedOutput saved;
    saved.backend = Backend::Ks;
    saved.id = saved.ks.deviceId = "filter|1";
    saved.ks.upsampling = Upsampling::IntegerMultiple;
    saved.ks.upsamplingMultiple = 4;
    saved.ks.filter.phase = FilterPhase::Minimum;
    settings.outputs.push_back(saved);
    const Settings back = fromJson(toJson(settings));
    EXPECT_EQ(back.wasapi, settings.wasapi);
    EXPECT_EQ(back.asio, settings.asio);
    EXPECT_EQ(back.ks, settings.ks);
    EXPECT_EQ(back.network.fixedRate, 192000u);
    EXPECT_EQ(back.network.filter, settings.network.filter);
    ASSERT_EQ(back.outputs.size(), 1u);
    EXPECT_EQ(back.outputs[0].ks, saved.ks);
}

// Settings of 0.4.2: a fixed rate is the set rate, and every filter starts from the one global quality.
TEST(UpsamplingSettings, AnEarlierFileKeepsItsRateAndQuality) {
    const auto parsed = fromJson(R"({"schema":4,"backend":"asio","resampleQuality":"veryhigh",
        "asio":{"fixedRate":96000},"ks":{"fixedRate":0},
        "outputs":[{"backend":"wasapi","id":"endpoint","parameters":{"fixedRate":192000}}]})");
    EXPECT_EQ(fixedRate(parsed), 96000u);
    EXPECT_EQ(parsed.asio.upsampling, Upsampling::Off);
    EXPECT_EQ(parsed.asio.filter, (ResampleFilter{ResampleQuality::VeryHigh, FilterPhase::Linear, FilterPassband::Standard}));
    EXPECT_EQ(parsed.ks.fixedRate, 0u);
    EXPECT_EQ(parsed.ks.filter.quality, ResampleQuality::VeryHigh);
    EXPECT_EQ(parsed.wasapi.filter.quality, ResampleQuality::VeryHigh);
    EXPECT_EQ(parsed.network.filter.quality, ResampleQuality::VeryHigh);
    ASSERT_EQ(parsed.outputs.size(), 1u);
    EXPECT_EQ(parsed.outputs[0].wasapi.fixedRate, 192000u);
    EXPECT_EQ(parsed.outputs[0].wasapi.filter.quality, ResampleQuality::VeryHigh);
    EXPECT_EQ(resampleFilter(parsed), parsed.asio.filter);
}

TEST(UpsamplingSettings, InvalidValuesFallBack) {
    const auto parsed = fromJson(R"({"wasapi":{"upsampling":"best","upsamplingMultiple":3,
        "filter":{"quality":"ultra","phase":"maximum","passband":7}}})");
    EXPECT_EQ(parsed.wasapi.upsampling, Upsampling::Off);
    EXPECT_EQ(parsed.wasapi.upsamplingMultiple, 2u);
    EXPECT_EQ(parsed.wasapi.filter, ResampleFilter{});
}

TEST(UpsamplingSettings, SharedModeAndAirPlayResampleWithTheirOwnQuality) {
    Settings settings;
    settings.wasapi.filter = {ResampleQuality::Quick, FilterPhase::Minimum, FilterPassband::Steep};
    settings.wasapi.sharedResampleQuality = ResampleQuality::VeryHigh;
    settings.resampleQuality = ResampleQuality::Quick;
    EXPECT_EQ(resampleFilter(settings), settings.wasapi.filter);
    settings.wasapi.shared = true;
    EXPECT_EQ(resampleFilter(settings), ResampleFilter{ResampleQuality::VeryHigh});
    settings.backend = Backend::AirPlay;
    EXPECT_EQ(resampleFilter(settings), ResampleFilter{ResampleQuality::Quick});
}
