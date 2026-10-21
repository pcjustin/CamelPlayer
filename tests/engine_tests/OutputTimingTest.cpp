// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Output parameters of docs/Plan.md §6 through a simulated device: a fixed sample rate resamples PCM without the
// convert setting and falls back visibly when the device refuses it, silence after a format change is
// format-correct (DoP markers continue), the device is released after a pause, and timing changes never reopen it.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

#include "DsdFixture.h"
#include "Engine/PlaybackEngine.h"
#include "Engine/SamplePacker.h"
#include "OutputBridge/CaptureOutput.h"
#include "StartedSnapshot.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::audio;
using namespace std::chrono_literals;

namespace {

struct Played {
    EngineSnapshot started; // right after the stream opened
    EngineSnapshot ended;
    std::string bytes;
};

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

// Plays `file` to its end `times` times on one engine (one device) and returns the capture of the last run.
Played play(const std::filesystem::path& file, const Settings& settings, std::function<bool(const DeviceFormat&)> accepts,
            int times = 1) {
    CaptureOptions options;
    options.file = testutil::tempDir() / "output-timing.raw";
    options.accepts = std::move(accepts);
    Played played;
    PlaybackEngine engine(settings, [&](const Settings&) { return createCaptureOutput(options); });
    testutil::StartedSnapshot started(engine);
    for (int i = 0; i < times; i++) {
        started.reset();
        engine.openAndPlay(file);
        engine.waitIdle();
        played.started = started.get().value_or(engine.snapshot());
        EXPECT_TRUE(engine.waitForState(PlayState::Stopped, 30s));
    }
    played.ended = engine.snapshot();
    played.bytes = readFile(options.file);
    return played;
}

bool int32At(const DeviceFormat& f, std::initializer_list<uint32_t> rates) {
    return f.type == SampleType::Int32 && std::find(rates.begin(), rates.end(), f.sampleRate) != rates.end();
}

bool waitFor(PlaybackEngine& engine, const std::function<bool(const EngineSnapshot&)>& done) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        engine.waitIdle();
        if (done(engine.snapshot())) return true;
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

const auto playing = [](const EngineSnapshot& s) { return s.state == PlayState::Playing && s.position > 0; };

// A real-time simulated device that counts how often the engine created an output.
struct Rig {
    std::atomic<int> outputs{0};
    std::unique_ptr<PlaybackEngine> engine;
    explicit Rig(const Settings& settings) {
        CaptureOptions options;
        options.file = testutil::tempDir() / "output-timing-rt.raw";
        options.realtime = true;
        engine = std::make_unique<PlaybackEngine>(settings, [this, options](const Settings&) {
            ++outputs;
            return createCaptureOutput(options);
        });
    }
};

} // namespace

TEST(FixedRate, ResamplesPcmWithoutTheConvertSetting) {
    Settings settings;
    settings.wasapi.fixedRate = 48000;
    ASSERT_FALSE(settings.convertWhenUnsupported);
    const auto played =
        play(testutil::testFile("16-44.1k"), settings, [](const DeviceFormat& f) { return int32At(f, {44100, 48000}); });
    ASSERT_FALSE(played.ended.error) << played.ended.errorDetail;
    ASSERT_TRUE(played.started.plan);
    EXPECT_EQ(played.started.plan->format, (DeviceFormat{48000, 2, SampleType::Int32}));
    EXPECT_TRUE(played.started.plan->resample);
    EXPECT_EQ(played.started.plan->state, PipelineState::Processed);
    EXPECT_TRUE(played.started.outputStatus.adjustments.empty());
    const double seconds = double(played.bytes.size()) / (48000.0 * 2 * 4);
    EXPECT_NEAR(seconds, 2.0, 0.02);
}

TEST(FixedRate, TheFilesOwnRateStaysBitPerfect) {
    Settings settings;
    settings.asio.fixedRate = 44100;
    settings.backend = Backend::Asio;
    const auto played =
        play(testutil::testFile("16-44.1k"), settings, [](const DeviceFormat& f) { return int32At(f, {44100, 48000}); });
    ASSERT_FALSE(played.ended.error) << played.ended.errorDetail;
    ASSERT_TRUE(played.started.plan);
    EXPECT_EQ(played.started.plan->format.sampleRate, 44100u);
    EXPECT_EQ(played.started.plan->state, PipelineState::BitPerfect);
}

TEST(FixedRate, ARateTheDeviceRefusesFollowsTheFileAndSaysSo) {
    Settings settings;
    settings.backend = Backend::Ks;
    settings.ks.fixedRate = 96000;
    const auto played = play(testutil::testFile("16-44.1k"), settings, [](const DeviceFormat& f) { return int32At(f, {44100}); });
    ASSERT_FALSE(played.ended.error) << played.ended.errorDetail;
    ASSERT_TRUE(played.started.plan);
    EXPECT_EQ(played.started.plan->format.sampleRate, 44100u);
    EXPECT_EQ(played.started.plan->state, PipelineState::BitPerfect);
    ASSERT_EQ(played.started.outputStatus.adjustments.size(), 1u);
    EXPECT_EQ(played.started.outputStatus.adjustments[0],
              "Fixed sample rate: requested 96000 Hz, using 44100 Hz (the device refused it)");
}

TEST(FixedRate, IgnoredForRawDsdAndInSharedMode) {
    using namespace dsdfixture;
    const auto file = testutil::tempDir() / "output-timing.dsf";
    writeFile(file, dsf(rawDsd(kDsd64 / 8 / 5, 2, 31), {.channels = 2, .rate = kDsd64}));
    Settings settings;
    settings.wasapi.fixedRate = 48000;
    const auto dop = play(file, settings, [](const DeviceFormat& f) { return f.type == SampleType::Int32; });
    ASSERT_FALSE(dop.ended.error) << dop.ended.errorDetail;
    ASSERT_TRUE(dop.started.plan);
    EXPECT_EQ(dop.started.plan->dsd, DsdPath::Dop);
    EXPECT_EQ(dop.started.plan->format.sampleRate, 176400u);

    settings.wasapi.shared = true;
    EXPECT_EQ(fixedRate(settings), 0u);
}

TEST(FormatChangeSilence, PcmStartsWithAtLeastTheConfiguredZerosOnlyAfterAChange) {
    const auto file = testutil::testFile("16-44.1k");
    const auto accepts = [](const DeviceFormat& f) { return int32At(f, {44100}); };
    const auto plain = play(file, Settings{}, accepts);
    ASSERT_FALSE(plain.ended.error) << plain.ended.errorDetail;
    Settings settings;
    settings.wasapi.timing.formatChangeSilenceMs = 500;
    const auto silenced = play(file, settings, accepts);
    ASSERT_FALSE(silenced.ended.error) << silenced.ended.errorDetail;
    EXPECT_EQ(silenced.ended.underruns, 0u);
    constexpr size_t kFrameBytes = 8;
    ASSERT_GT(silenced.bytes.size(), plain.bytes.size());
    const size_t lead = (silenced.bytes.size() - plain.bytes.size()) / kFrameBytes;
    EXPECT_GE(lead, 22050u);
    EXPECT_LT(lead, 22050u + 4096u); // rounded up to the device buffer
    EXPECT_TRUE(std::all_of(silenced.bytes.begin(), silenced.bytes.begin() + ptrdiff_t(lead * kFrameBytes),
                            [](char b) { return b == 0; }));
    EXPECT_EQ(silenced.bytes.substr(lead * kFrameBytes), plain.bytes);
    // The same format again on the same device: no silence.
    const auto again = play(file, settings, accepts, 2);
    EXPECT_EQ(again.bytes, plain.bytes);
}

TEST(FormatChangeSilence, DopSilenceIsIdlePatternWithContinuousMarkers) {
    using namespace dsdfixture;
    const Bytes raw = rawDsd(kDsd64 / 8 / 5, 2, 32);
    const auto file = testutil::tempDir() / "output-timing-silence.dsf";
    writeFile(file, dsf(raw, {.channels = 2, .rate = kDsd64}));
    Settings settings;
    settings.wasapi.timing.formatChangeSilenceMs = 200;
    const auto played = play(file, settings, [](const DeviceFormat& f) { return f.type == SampleType::Int32; });
    ASSERT_FALSE(played.ended.error) << played.ended.errorDetail;
    ASSERT_TRUE(played.started.plan);
    ASSERT_EQ(played.started.plan->dsd, DsdPath::Dop);
    const size_t frames = played.bytes.size() / 8;
    const auto byte = [&](size_t frame, size_t channel, size_t offset) {
        return uint8_t(played.bytes[(frame * 2 + channel) * 4 + offset]);
    };
    size_t lead = 0;
    while (lead < frames && byte(lead, 0, 2) == 0x69 && byte(lead, 0, 1) == 0x69 && byte(lead, 1, 2) == 0x69) lead++;
    EXPECT_GE(lead, 35280u);             // 200 ms of the 176.4 kHz carrier
    const size_t audio = raw.size() / 4; // byte frames in pairs
    ASSERT_GE(frames, lead + audio);
    for (size_t f = 0; f < frames; f++)
        for (size_t c = 0; c < 2; c++) {
            ASSERT_TRUE(byte(f, c, 3) == 0x05 || byte(f, c, 3) == 0xFA) << "frame " << f;
            if (f) ASSERT_NE(byte(f, c, 3), byte(f - 1, c, 3)) << "markers repeat at frame " << f;
        }
    for (size_t f = 0; f < audio; f++)
        for (size_t c = 0; c < 2; c++) {
            ASSERT_EQ(byte(lead + f, c, 2), raw[(2 * f) * 2 + c]) << "frame " << f;
            ASSERT_EQ(byte(lead + f, c, 1), raw[(2 * f + 1) * 2 + c]) << "frame " << f;
        }
}

TEST(ReleaseAfterPause, ClosesTheDeviceAndResumesAtThePosition) {
    Settings settings;
    settings.wasapi.timing.releaseAfterPauseSeconds = 1;
    Rig rig(settings);
    rig.engine->openAndPlay(testutil::testFile("16-44.1k"));
    ASSERT_TRUE(waitFor(*rig.engine, playing));
    rig.engine->pause();
    rig.engine->waitIdle();
    const auto paused = rig.engine->snapshot();
    ASSERT_EQ(paused.state, PlayState::Paused);
    ASSERT_TRUE(paused.output);
    std::this_thread::sleep_for(600ms);
    EXPECT_TRUE(rig.engine->snapshot().output) << "released early";
    ASSERT_TRUE(waitFor(*rig.engine, [](const EngineSnapshot& s) { return !s.output; }));
    auto s = rig.engine->snapshot();
    EXPECT_EQ(s.state, PlayState::Paused);
    EXPECT_FALSE(s.error) << s.errorDetail;
    EXPECT_EQ(s.position, paused.position);
    EXPECT_EQ(rig.outputs, 1);
    rig.engine->play();
    ASSERT_TRUE(waitFor(*rig.engine, [&](const EngineSnapshot& now) { return playing(now) && now.position > paused.position; }));
    EXPECT_EQ(rig.outputs, 2);
    rig.engine->stop();
}

TEST(ReleaseAfterPause, NeverWhenZero) {
    Settings settings;
    settings.wasapi.timing.releaseAfterPauseSeconds = 0;
    Rig rig(settings);
    rig.engine->openAndPlay(testutil::testFile("16-44.1k"));
    ASSERT_TRUE(waitFor(*rig.engine, playing));
    rig.engine->pause();
    std::this_thread::sleep_for(1500ms);
    rig.engine->waitIdle();
    EXPECT_TRUE(rig.engine->snapshot().output);
    rig.engine->stop();
}

TEST(OutputTiming, TimingChangesDoNotReopenTheDevice) {
    Rig rig(Settings{});
    rig.engine->openAndPlay(testutil::testFile("16-44.1k"));
    ASSERT_TRUE(waitFor(*rig.engine, playing));
    Settings settings;
    settings.wasapi.timing = {20, 300, 5};
    rig.engine->applySettings(settings);
    rig.engine->waitIdle();
    EXPECT_EQ(rig.outputs, 1);
    EXPECT_EQ(rig.engine->snapshot().state, PlayState::Playing);
    settings.wasapi.fixedRate = 48000;
    rig.engine->applySettings(settings);
    ASSERT_TRUE(waitFor(*rig.engine, [](const EngineSnapshot& s) { return s.output && s.output->sampleRate == 48000; }));
    EXPECT_EQ(rig.outputs, 2);
    rig.engine->stop();
}
