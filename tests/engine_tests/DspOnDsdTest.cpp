// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// DSP on DSD sources per output (docs/Plan.md §6): the switch, off by default; with it on, each DSP node, ReplayGain,
// the software volume and mute reaching DSD64 and DSD256 sources over native DSD, DoP and DSF, and over DSD to PCM;
// raw DSD bit-exact with the switch off and with nothing acting; a muted DSD source sending only silence.
#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <fstream>
#include <numbers>
#include <string>
#include <vector>

#include "DsdFixture.h"
#include "Engine/EngineAbi.h"
#include "Engine/FormatRule.h"
#include "Engine/PlaybackEngine.h"
#include "OutputBridge/CaptureOutput.h"
#include "StartedSnapshot.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::audio;
using namespace std::chrono_literals;

namespace {

constexpr uint32_t kDsd64Bytes = 352800;

struct Path {
    const char* name;
    Backend backend;
    bool dop;
};

const Path kPaths[] = {{"native", Backend::Asio, false}, {"DoP", Backend::Wasapi, true}, {"DSF", Backend::Network, false}};

// A device taking DSD up to DSD256, natively or as DoP, and 32-bit PCM up to 192 kHz.
bool takesDsd(const DeviceFormat& f, bool dop) {
    if (f.type == SampleType::Dsd8) return !dop && f.sampleRate <= 4 * kDsd64Bytes;
    if (dop && f.type == SampleType::Int24 && f.sampleRate % 44100 == 0 && f.sampleRate <= 2 * kDsd64Bytes) return true;
    return f.type == SampleType::Int32 && f.sampleRate <= 192000;
}

// A 1 kHz sine at `amplitude` as stereo DSD at `multiple` times DSD64, 0.4 s.
std::filesystem::path dsdTone(uint32_t multiple, double amplitude = 0.25) {
    using namespace dsdfixture;
    const uint32_t rate = kDsd64 * multiple;
    const auto path = testutil::tempDir() / ("dsp-on-dsd-" + std::to_string(64 * multiple) + ".dsf");
    std::error_code error;
    if (!std::filesystem::exists(path, error))
        writeFile(path, dsf(dsdSine(rate, 1000, amplitude, size_t(rate) * 4 / 80), {.channels = 2, .rate = rate}));
    return path;
}

std::filesystem::path impulseFile() {
    const auto path = testutil::tempDir() / "dsp-on-dsd-impulse.wav";
    std::string out;
    const auto put = [&](uint32_t value, int bytes) {
        for (int i = 0; i < bytes; ++i) out.push_back(char((value >> (8 * i)) & 0xff));
    };
    constexpr uint32_t frames = 64, rate = 44100;
    out += "RIFF";
    put(36 + frames * 4, 4);
    out += "WAVEfmt ";
    put(16, 4);
    put(3, 2);
    put(1, 2);
    put(rate, 4);
    put(rate * 4, 4);
    put(4, 2);
    put(32, 2);
    out += "data";
    put(frames * 4, 4);
    for (uint32_t n = 0; n < frames; ++n) {
        const float v = n == 0 ? 0.5f : n == 1 ? 0.25f : 0.0f;
        put(std::bit_cast<uint32_t>(v), 4);
    }
    std::ofstream(path, std::ios::binary | std::ios::trunc).write(out.data(), std::streamsize(out.size()));
    return path;
}

struct Played {
    EngineSnapshot started;
    EngineSnapshot ended;
    std::string bytes;
};

Played play(const std::filesystem::path& file, const Settings& settings, bool dop, double gainDb = 0.0) {
    const auto out = testutil::tempDir() / "dsp-on-dsd.raw";
    std::filesystem::remove(out);
    CaptureOptions options;
    options.file = out;
    options.accepts = [dop](const DeviceFormat& f) { return takesDsd(f, dop); };
    options.realtime = true;
    Played played;
    {
        PlaybackEngine engine(settings, [&](const Settings&) { return createCaptureOutput(options); });
        testutil::StartedSnapshot started(engine);
        engine.openAndPlay(file, std::nullopt, gainDb);
        engine.waitIdle();
        played.started = started.get().value_or(engine.snapshot());
        EXPECT_TRUE(engine.waitForState(PlayState::Stopped, 30s));
        played.ended = engine.snapshot();
    }
    std::ifstream in(out, std::ios::binary);
    played.bytes.assign(std::istreambuf_iterator<char>(in), {});
    return played;
}

// The left channel's DSD bytes in time order, from native DSD or 24-bit DoP.
std::vector<uint8_t> leftDsd(const std::string& bytes, bool dop) {
    std::vector<uint8_t> left;
    if (!dop) {
        for (size_t i = 0; i + 1 < bytes.size(); i += 2) left.push_back(uint8_t(bytes[i]));
        return left;
    }
    for (size_t i = 0; i + 5 < bytes.size(); i += 6) {
        left.push_back(uint8_t(bytes[i + 1]));
        left.push_back(uint8_t(bytes[i]));
    }
    return left;
}

// The amplitude of the 1 kHz tone in DSD at `byteRate`, decoded as playback decodes it, over the middle of the track:
// the idle pattern the device gets before and after it, which lasts longer on a busy machine, is left out.
double toneAmplitude(const std::vector<uint8_t>& dsd, uint32_t byteRate) {
    constexpr uint8_t kIdle = 0x69;
    const auto first = std::ranges::find_if(dsd, [](uint8_t b) { return b != kIdle; });
    const auto last = std::find_if(dsd.rbegin(), dsd.rend(), [](uint8_t b) { return b != kIdle; }).base();
    if (first >= last) return 0.0;
    const std::vector<uint8_t> track(first, last);
    CamelDsdToPcm* converter = camel_dsd_to_pcm_create_v1(byteRate * 8, 1);
    EXPECT_NE(converter, nullptr);
    if (!converter) return 0.0;
    std::vector<float> pcm(track.size());
    size_t produced = 0;
    EXPECT_EQ(camel_dsd_to_pcm_process_v1(converter, track.data(), track.size(), pcm.data(), pcm.size(), &produced),
              CAMEL_ENGINE_OK);
    camel_dsd_to_pcm_destroy_v1(converter);
    constexpr size_t kPeriods = 1764; // five periods of 1 kHz at 352.8 kHz
    const size_t from = produced / 4, count = (produced / 2) / kPeriods * kPeriods;
    double re = 0.0, im = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const double phase = 2 * std::numbers::pi * 1000.0 * double(i) / 352800.0;
        re += pcm[from + i] * std::cos(phase);
        im += pcm[from + i] * std::sin(phase);
    }
    return count ? 2.0 * std::hypot(re, im) / double(count) : 0.0;
}

// The captured stream without the idle pattern the device gets before and after the track.
constexpr char kDsdIdle = 0x69;

std::string withoutIdle(const std::string& bytes) {
    const size_t from = bytes.find_first_not_of(kDsdIdle);
    if (from == std::string::npos) return {};
    return bytes.substr(from, bytes.find_last_not_of(kDsdIdle) + 1 - from);
}

Settings withSwitch(Backend backend, bool on = true) {
    Settings settings;
    settings.backend = backend;
    settings.asio.dsdOutput = DsdOutput::Native;
    settings.wasapi.dspOnDsd = settings.asio.dspOnDsd = settings.network.dspOnDsd = on;
    return settings;
}

enum class Step : uint8_t { Eq, Crossfeed, Convolution, ReplayGain, Volume, Mute };

// `settings` with one step that changes the signal, and the ReplayGain to play with.
double turnOn(Settings& settings, Step step) {
    switch (step) {
    case Step::Eq:
        settings.eq.enabled = true;
        settings.eq.bands.front().gainDb = 6.0;
        break;
    case Step::Crossfeed:
        settings.crossfeed.enabled = true;
        break;
    case Step::Convolution:
        settings.convolution = {true, impulseFile().string()};
        break;
    case Step::ReplayGain:
        return -3.0;
    case Step::Volume:
        settings.volume = {.mode = VolumeMode::Software, .db = -6.0};
        break;
    case Step::Mute:
        settings.volume = {.mode = VolumeMode::Software, .muted = true};
        break;
    }
    return 0.0;
}

} // namespace

TEST(DspOnDsdSettings, OffByDefaultAndKeptPerOutput) {
    EXPECT_FALSE(fromJson("{}").asio.dspOnDsd);
    Settings settings;
    settings.wasapi.dspOnDsd = true;
    settings.network.dspOnDsd = true;
    SavedOutput saved;
    saved.backend = Backend::Ks;
    saved.id = saved.ks.deviceId = "filter|1";
    saved.ks.dspOnDsd = true;
    settings.outputs.push_back(saved);
    const Settings back = fromJson(toJson(settings));
    EXPECT_TRUE(back.wasapi.dspOnDsd);
    EXPECT_FALSE(back.asio.dspOnDsd);
    EXPECT_TRUE(back.network.dspOnDsd);
    ASSERT_EQ(back.outputs.size(), 1u);
    EXPECT_TRUE(back.outputs[0].ks.dspOnDsd);
}

TEST(DspOnDsdSettings, OnlyOutputsThatSendDsdTakeIt) {
    Settings settings = withSwitch(Backend::Wasapi);
    EXPECT_TRUE(dspOnDsd(settings));
    settings.wasapi.shared = true;
    EXPECT_FALSE(dspOnDsd(settings));
    settings.wasapi.shared = false;
    settings.wasapi.dsdOutput = DsdOutput::Pcm;
    EXPECT_FALSE(dspOnDsd(settings));
    for (const Backend backend : {Backend::AirPlay, Backend::HQPlayer}) {
        settings.backend = backend;
        EXPECT_FALSE(dspOnDsd(settings));
    }
}

// Raw DSD reaches the device bit for bit with the switch off while the DSP acts, and with the switch on while nothing
// acts.
TEST(DspOnDsd, RawDsdStaysBitExactWithTheSwitchOffOrNothingActing) {
    const auto file = dsdTone(1);
    const auto plain = play(file, withSwitch(Backend::Asio, false), false);
    ASSERT_TRUE(plain.started.plan);
    EXPECT_EQ(plain.started.plan->modulatorOrder, 0u);
    ASSERT_FALSE(plain.bytes.empty());

    auto off = withSwitch(Backend::Asio, false);
    turnOn(off, Step::Eq);
    const auto withEq = play(file, off, false);
    ASSERT_TRUE(withEq.started.plan);
    EXPECT_EQ(withEq.started.plan->modulatorOrder, 0u);
    EXPECT_EQ(withEq.started.plan->state, PipelineState::BitPerfect);
    EXPECT_EQ(withoutIdle(withEq.bytes), withoutIdle(plain.bytes));

    const auto idle = play(file, withSwitch(Backend::Asio), false);
    ASSERT_TRUE(idle.started.plan);
    EXPECT_EQ(idle.started.plan->modulatorOrder, 0u);
    EXPECT_EQ(withoutIdle(idle.bytes), withoutIdle(plain.bytes));
}

// With the switch on, each step that changes the signal takes DSD64 and DSD256 sources through the DSP and modulates
// them again at their own rate, natively, as DoP and as DSF, and the path shows the step.
TEST(DspOnDsd, EachStepReachesDsdOverNativeDopAndDsf) {
    for (const Path& path : kPaths) {
        for (const uint32_t multiple : {1u, 4u}) {
            for (const Step step : {Step::Eq, Step::Crossfeed, Step::Convolution, Step::ReplayGain, Step::Volume, Step::Mute}) {
                auto settings = withSwitch(path.backend);
                const double gainDb = turnOn(settings, step);
                const auto played = play(dsdTone(multiple), settings, path.dop, gainDb);
                const std::string name =
                    std::string(path.name) + " DSD" + std::to_string(64 * multiple) + " step " + std::to_string(int(step));
                ASSERT_FALSE(played.ended.error) << name << ": " << played.ended.errorDetail;
                ASSERT_TRUE(played.started.plan) << name;
                const auto& plan = *played.started.plan;
                EXPECT_EQ(plan.dsd, path.dop ? DsdPath::Dop : DsdPath::Native) << name;
                EXPECT_EQ(plan.modulatorOrder, 7u) << name;
                EXPECT_EQ(path.dop ? plan.format.sampleRate * 2 : plan.format.sampleRate, multiple * kDsd64Bytes) << name;
                EXPECT_EQ(plan.state, PipelineState::Processed) << name;
                switch (step) {
                case Step::Eq:
                    EXPECT_EQ(plan.dsp, std::vector<DspNode>{DspNode::Eq}) << name;
                    break;
                case Step::Crossfeed:
                    EXPECT_EQ(plan.dsp, std::vector<DspNode>{DspNode::Crossfeed}) << name;
                    break;
                case Step::Convolution:
                    EXPECT_EQ(plan.dsp, std::vector<DspNode>{DspNode::Convolution}) << name;
                    break;
                case Step::ReplayGain:
                    EXPECT_EQ(plan.gainDb, -3.0) << name;
                    break;
                case Step::Volume:
                    EXPECT_EQ(plan.volumeDb, -6.0) << name;
                    break;
                case Step::Mute:
                    EXPECT_TRUE(plan.muted) << name;
                    break;
                }
            }
        }
    }
}

// The tone is measured over the track alone: the idle pattern the device gets after it, until playback has stopped,
// grows on a busy machine and must not dilute the level (the full suite of 2026-11-24 read 0.61 for 0.5 once).
TEST(DspOnDsd, TheToneIsMeasuredOverTheTrackHoweverLongTheIdleAfterIt) {
    for (const uint32_t multiple : {1u, 4u}) {
        const uint32_t byteRate = multiple * kDsd64Bytes;
        auto left = leftDsd(play(dsdTone(multiple), withSwitch(Backend::Asio, false), false).bytes, false);
        const double level = toneAmplitude(left, byteRate);
        ASSERT_GT(level, 0.2) << byteRate;
        left.insert(left.end(), size_t(byteRate) * 3 / 10, uint8_t(0x69));
        EXPECT_NEAR(toneAmplitude(left, byteRate) / level, 1.0, 0.01) << byteRate;
    }
}

// The volume halves the decoded level and mute leaves only silence, at DSD64 and DSD256 and over DoP.
TEST(DspOnDsd, TheVolumeAndMuteReachTheSound) {
    for (const auto& [multiple, dop] : {std::pair{1u, false}, std::pair{4u, false}, std::pair{1u, true}}) {
        const auto file = dsdTone(multiple);
        const uint32_t byteRate = multiple * kDsd64Bytes;
        auto settings = withSwitch(dop ? Backend::Wasapi : Backend::Asio);
        settings.volume = {.mode = VolumeMode::Software, .db = -20.0 * std::log10(2.0)};
        const auto half = play(file, settings, dop);
        const auto source = play(file, withSwitch(dop ? Backend::Wasapi : Backend::Asio, false), dop);
        const double level = toneAmplitude(leftDsd(source.bytes, dop), byteRate);
        EXPECT_GT(level, 0.2) << byteRate;
        EXPECT_NEAR(toneAmplitude(leftDsd(half.bytes, dop), byteRate) / level, 0.5, 0.01) << byteRate;
        settings.volume.muted = true;
        const auto muted = play(file, settings, dop);
        ASSERT_TRUE(muted.started.plan);
        EXPECT_EQ(muted.started.plan->modulatorOrder, 7u);
        EXPECT_LT(toneAmplitude(leftDsd(muted.bytes, dop), byteRate), 1e-4) << byteRate;
    }
}

// An output set to DSD to PCM sends PCM, which the DSP takes as before.
TEST(DspOnDsd, DsdToPcmTakesTheDspAsBefore) {
    auto settings = withSwitch(Backend::Asio);
    settings.asio.dsdOutput = DsdOutput::Pcm;
    turnOn(settings, Step::Volume);
    const auto played = play(dsdTone(1), settings, false);
    ASSERT_TRUE(played.started.plan);
    EXPECT_EQ(played.started.plan->dsd, DsdPath::Pcm);
    EXPECT_EQ(played.started.plan->modulatorOrder, 0u);
    EXPECT_EQ(played.started.plan->volumeDb, -6.0);
}

// Turning the DSP on while raw DSD plays reopens it modulated, and off again raw.
TEST(DspOnDsd, TheDspTakesAPlayingDsdSourceToModulatedAndBack) {
    using namespace dsdfixture;
    const auto file = testutil::tempDir() / "dsp-on-dsd-long.dsf";
    writeFile(file, dsf(dsdSine(kDsd64, 1000, 0.25, size_t(kDsd64) / 8 * 4), {.channels = 2, .rate = kDsd64}));
    CaptureOptions options;
    options.file = testutil::tempDir() / "dsp-on-dsd-long.raw";
    options.accepts = [](const DeviceFormat& f) { return takesDsd(f, false); };
    options.realtime = true;
    auto settings = withSwitch(Backend::Asio);
    PlaybackEngine engine(settings, [&](const Settings&) { return createCaptureOutput(options); });
    engine.openAndPlay(file);
    engine.waitIdle();
    ASSERT_TRUE(engine.waitForState(PlayState::Playing, 5s));
    ASSERT_TRUE(engine.snapshot().plan);
    EXPECT_EQ(engine.snapshot().plan->modulatorOrder, 0u);
    turnOn(settings, Step::Crossfeed);
    engine.applySettings(settings);
    engine.waitIdle();
    ASSERT_TRUE(engine.waitForState(PlayState::Playing, 5s));
    ASSERT_TRUE(engine.snapshot().plan);
    EXPECT_EQ(engine.snapshot().plan->modulatorOrder, 7u);
    settings.crossfeed.enabled = false;
    engine.applySettings(settings);
    engine.waitIdle();
    ASSERT_TRUE(engine.waitForState(PlayState::Playing, 5s));
    ASSERT_TRUE(engine.snapshot().plan);
    EXPECT_EQ(engine.snapshot().plan->modulatorOrder, 0u);
    engine.stop();
    engine.waitIdle();
}
