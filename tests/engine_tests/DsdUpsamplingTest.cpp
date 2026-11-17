// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// DSD upsampling and PCM to DSD per output (docs/Plan.md §6): the rate each mode picks for DSD64, DSD128 and 44.1
// and 96 kHz PCM sources against outputs limited to DSD64, DSD128 and DSD256, over native DSD, DoP and DSF; the DSP
// chain and the software volume on the modulated path; the stop when modulation falls behind; and the choice each
// output keeps in settings.json.
#include <gtest/gtest.h>

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
enum class Carrier : uint8_t { Native, Dop };

// A device taking DSD up to `highest` (a byte frame rate) natively, or as DoP in carriers up to half of it, and 32-bit
// PCM up to 192 kHz, below every DoP carrier but DSD64's.
bool takesDsdUpTo(const DeviceFormat& f, Carrier carrier, uint32_t highest) {
    if (f.type == SampleType::Dsd8) return carrier == Carrier::Native && f.sampleRate <= highest;
    if (carrier == Carrier::Dop && (f.type == SampleType::Int24 || f.type == SampleType::Int32) && f.sampleRate % 44100 == 0 &&
        f.sampleRate <= highest / 2)
        return true;
    return f.type == SampleType::Int32 && f.sampleRate <= 192000;
}

// The DSD rate, as a multiple of 44.1 kHz, that `d` sends `src` at; 0 when none.
uint32_t picked(const DsdUpsamplingSettings& d, const SourceInfo& src, Carrier carrier, uint32_t highest, uint32_t limit = 0) {
    const auto rates = dsdUpsamplingRates(d, src.bitsPerSample == 1 ? src.sampleRate : 0, limit);
    const auto plan = negotiateModulated(src, rates, 7, carrier == Carrier::Native, std::nullopt,
                                         [&](const DeviceFormat& f) { return takesDsdUpTo(f, carrier, highest); });
    if (!plan) return 0;
    const uint32_t byteRate = plan->dsd == DsdPath::Dop ? plan->format.sampleRate * 2 : plan->format.sampleRate;
    return byteRate / kDsd64Bytes * 64;
}

uint32_t dsd(uint32_t multiple) {
    return dsdByteRate(multiple);
}

const SourceInfo kDsd64Source{kDsd64Bytes, 2, 1}, kDsd128Source{2 * kDsd64Bytes, 2, 1}, kPcm44{44100, 2, 24},
    kPcm96{96000, 2, 24};

// `seconds` of a 1 kHz sine at `amplitude` as 24-bit stereo WAVE at `rate`.
std::filesystem::path sineWave(const std::string& name, uint32_t rate, double amplitude, double seconds) {
    const auto frames = uint32_t(rate * seconds);
    std::string out;
    const auto put = [&](uint32_t value, int bytes) {
        for (int i = 0; i < bytes; ++i) out.push_back(char((value >> (8 * i)) & 0xff));
    };
    out += "RIFF";
    put(36 + frames * 6, 4);
    out += "WAVEfmt ";
    put(16, 4);
    put(1, 2);
    put(2, 2);
    put(rate, 4);
    put(rate * 6, 4);
    put(6, 2);
    put(24, 2);
    out += "data";
    put(frames * 6, 4);
    for (uint32_t n = 0; n < frames; ++n) {
        const auto v = int32_t(std::lround(amplitude * std::sin(2 * std::numbers::pi * 1000.0 * n / rate) * 8388607.0));
        put(uint32_t(v), 3);
        put(uint32_t(v), 3);
    }
    const auto path = testutil::tempDir() / ("dsd-upsampling-" + name + ".wav");
    std::ofstream(path, std::ios::binary | std::ios::trunc).write(out.data(), std::streamsize(out.size()));
    return path;
}

std::filesystem::path dsd64File() {
    using namespace dsdfixture;
    const auto file = testutil::tempDir() / "dsd-upsampling.dsf";
    writeFile(file, dsf(rawDsd(kDsd64 / 8 / 4, 2, 7), {.channels = 2, .rate = kDsd64}));
    return file;
}

struct Played {
    EngineSnapshot started;
    EngineSnapshot ended;
    std::string bytes;
};

// Plays `file` to a capture at the device's pace, as a raw DSD stream needs.
Played play(const std::filesystem::path& file, const Settings& settings, Carrier carrier, uint32_t highest) {
    const auto out = testutil::tempDir() / "dsd-upsampling.raw";
    std::filesystem::remove(out);
    CaptureOptions options;
    options.file = out;
    options.accepts = [=](const DeviceFormat& f) { return takesDsdUpTo(f, carrier, highest); };
    options.realtime = true;
    Played played;
    {
        PlaybackEngine engine(settings, [&](const Settings&) { return createCaptureOutput(options); });
        testutil::StartedSnapshot started(engine);
        engine.openAndPlay(file);
        engine.waitIdle();
        played.started = started.get().value_or(engine.snapshot());
        EXPECT_TRUE(engine.waitForState(PlayState::Stopped, 30s));
        played.ended = engine.snapshot();
    }
    std::ifstream in(out, std::ios::binary);
    played.bytes.assign(std::istreambuf_iterator<char>(in), {});
    return played;
}

// The amplitude of the 1 kHz tone in the left channel of captured native DSD at `byteRate`, decoded as playback
// decodes DSD, over its middle.
double toneAmplitude(const std::string& bytes, uint32_t byteRate) {
    CamelDsdToPcm* converter = camel_dsd_to_pcm_create_v1(byteRate * 8, 2);
    EXPECT_NE(converter, nullptr);
    if (!converter) return 0.0;
    const size_t frames = bytes.size() / 2;
    std::vector<float> pcm(frames * 2);
    size_t produced = 0;
    EXPECT_EQ(camel_dsd_to_pcm_process_v1(converter, reinterpret_cast<const uint8_t*>(bytes.data()), frames, pcm.data(), frames,
                                          &produced),
              CAMEL_ENGINE_OK);
    camel_dsd_to_pcm_destroy_v1(converter);
    // Whole periods of 1 kHz at 352.8 kHz: 1764 samples are five, so the shaped noise above the band averages out.
    constexpr size_t kPeriods = 1764;
    const size_t from = produced / 4, count = (produced / 2) / kPeriods * kPeriods;
    double re = 0.0, im = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const double phase = 2 * std::numbers::pi * 1000.0 * double(i) / 352800.0;
        re += pcm[2 * (from + i)] * std::cos(phase);
        im += pcm[2 * (from + i)] * std::sin(phase);
    }
    return count ? 2.0 * std::hypot(re, im) / double(count) : 0.0;
}

Settings asioUpsampling(DsdUpsampling mode, bool pcm = false) {
    Settings settings;
    settings.backend = Backend::Asio;
    settings.asio.dsdOutput = DsdOutput::Native;
    settings.asio.dsdUpsampling = {.mode = mode, .pcm = pcm};
    settings.convertWhenUnsupported = false;
    return settings;
}

} // namespace

TEST(DsdUpsampling, ToTheHighestTakesTheHighestRateTheOutputAllows) {
    const DsdUpsamplingSettings highest{.mode = DsdUpsampling::Highest};
    for (const Carrier carrier : {Carrier::Native, Carrier::Dop}) {
        for (const SourceInfo& src : {kDsd64Source, kDsd128Source, kPcm44, kPcm96}) {
            EXPECT_EQ(picked(highest, src, carrier, dsd(256)), 256u);
            EXPECT_EQ(picked(highest, src, carrier, dsd(128)), 128u);
            EXPECT_EQ(picked(highest, src, carrier, dsd(64)), 64u);
            // The output's own highest DSD rate limits it as the device does.
            EXPECT_EQ(picked(highest, src, carrier, dsd(256), dsd(128)), 128u);
            EXPECT_EQ(picked(highest, src, carrier, dsd(256), dsd(64)), 64u);
        }
    }
}

TEST(DsdUpsampling, ByAMultipleTakesTheHighestMultipleThatFits) {
    const DsdUpsamplingSettings twice{.mode = DsdUpsampling::Multiple, .multiple = 2};
    const DsdUpsamplingSettings fourTimes{.mode = DsdUpsampling::Multiple, .multiple = 4};
    for (const Carrier carrier : {Carrier::Native, Carrier::Dop}) {
        EXPECT_EQ(picked(twice, kDsd64Source, carrier, dsd(256)), 128u);
        EXPECT_EQ(picked(twice, kDsd128Source, carrier, dsd(256)), 256u);
        EXPECT_EQ(picked(fourTimes, kDsd64Source, carrier, dsd(256)), 256u);
        EXPECT_EQ(picked(fourTimes, kDsd64Source, carrier, dsd(128)), 128u);
        EXPECT_EQ(picked(fourTimes, kDsd64Source, carrier, dsd(256), dsd(128)), 128u);
        EXPECT_EQ(picked(twice, kDsd64Source, carrier, dsd(64)), 64u);
        // DSD128 times 4 is past DSD256: the highest multiple that fits is 2.
        EXPECT_EQ(picked(fourTimes, kDsd128Source, carrier, dsd(256)), 256u);
        // PCM has no DSD rate to multiply: it goes at the set rate.
        EXPECT_EQ(picked({.mode = DsdUpsampling::Multiple, .rate = 128}, kPcm44, carrier, dsd(256)), 128u);
        EXPECT_EQ(picked({.mode = DsdUpsampling::Multiple, .rate = 256}, kPcm96, carrier, dsd(256)), 256u);
        EXPECT_EQ(picked({.mode = DsdUpsampling::Multiple, .rate = 256}, kPcm96, carrier, dsd(128)), 128u);
    }
}

TEST(DsdUpsampling, ASetRateStepsDownToWhatTheOutputTakes) {
    for (const Carrier carrier : {Carrier::Native, Carrier::Dop}) {
        for (const SourceInfo& src : {kDsd64Source, kDsd128Source, kPcm44, kPcm96}) {
            EXPECT_EQ(picked({.mode = DsdUpsampling::Set, .rate = 128}, src, carrier, dsd(256)), 128u);
            EXPECT_EQ(picked({.mode = DsdUpsampling::Set, .rate = 256}, src, carrier, dsd(256)), 256u);
            EXPECT_EQ(picked({.mode = DsdUpsampling::Set, .rate = 256}, src, carrier, dsd(128)), 128u);
            EXPECT_EQ(picked({.mode = DsdUpsampling::Set, .rate = 128}, src, carrier, dsd(64)), 64u);
        }
    }
}

TEST(DsdUpsampling, OffListsNoRateAndEveryRateIsWithinDsd64ToDsd256) {
    EXPECT_TRUE(dsdUpsamplingRates({}, kDsd64Bytes, 0).empty());
    EXPECT_EQ(dsdUpsamplingRates({.mode = DsdUpsampling::Highest}, kDsd64Bytes, 0),
              (std::vector<uint32_t>{dsd(256), dsd(128), dsd(64)}));
    // DSD512 has no multiple within DSD256; the paths without upsampling take it.
    EXPECT_TRUE(dsdUpsamplingRates({.mode = DsdUpsampling::Multiple, .multiple = 2}, dsd(512), 0).empty());
}

// DoP at a rate only where the output reports the carrier it needs: DSD256 needs 705.6 kHz.
TEST(DsdUpsampling, DopTakesOnlyTheCarriersTheOutputReports) {
    const DsdUpsamplingSettings highest{.mode = DsdUpsampling::Highest};
    const SourceInfo src = kPcm44;
    const auto plan =
        negotiateModulated(src, dsdUpsamplingRates(highest, 0, 0), 7, false, std::nullopt,
                           [](const DeviceFormat& f) { return f.type == SampleType::Int24 && f.sampleRate <= 352800; });
    ASSERT_TRUE(plan);
    EXPECT_EQ(plan->dsd, DsdPath::Dop);
    EXPECT_EQ(plan->format.sampleRate, 352800u);
    EXPECT_EQ(plan->modulatorOrder, 7u);
    EXPECT_EQ(plan->state, PipelineState::Processed);
}

TEST(DsdUpsampling, PcmGoesOutAsDsdOnlyWithTheSwitch) {
    const auto file = sineWave("switch", 44100, 0.5, 0.4);
    auto settings = asioUpsampling(DsdUpsampling::Highest);
    auto played = play(file, settings, Carrier::Native, dsd(128));
    ASSERT_FALSE(played.ended.error) << played.ended.errorDetail;
    ASSERT_TRUE(played.started.plan);
    EXPECT_EQ(played.started.plan->dsd, DsdPath::None);
    EXPECT_EQ(played.started.plan->modulatorOrder, 0u);

    settings.asio.dsdUpsampling.pcm = true;
    settings.asio.filter.modulatorOrder = 5;
    played = play(file, settings, Carrier::Native, dsd(128));
    ASSERT_FALSE(played.ended.error) << played.ended.errorDetail;
    ASSERT_TRUE(played.started.plan);
    EXPECT_EQ(played.started.plan->dsd, DsdPath::Native);
    EXPECT_EQ(played.started.plan->format.sampleRate, dsd(128));
    EXPECT_EQ(played.started.plan->modulatorOrder, 5u);
    EXPECT_EQ(played.started.plan->state, PipelineState::Processed);
    // About 0.4 s of DSD128 byte frames, and the sine at -6 dB in them: 0.25 peak.
    EXPECT_NEAR(double(played.bytes.size()) / 2, 0.4 * dsd(128), 0.1 * dsd(128));
    EXPECT_NEAR(toneAmplitude(played.bytes, dsd(128)), 0.25, 0.005);
}

TEST(DsdUpsampling, DsdSourcesUpsampleOverDopNativeAndDsf) {
    const auto file = dsd64File();
    Settings dop;
    dop.wasapi.dsdUpsampling = {.mode = DsdUpsampling::Multiple, .multiple = 2};
    auto played = play(file, dop, Carrier::Dop, dsd(256));
    ASSERT_FALSE(played.ended.error) << played.ended.errorDetail;
    ASSERT_TRUE(played.started.plan);
    EXPECT_EQ(played.started.plan->dsd, DsdPath::Dop);
    EXPECT_EQ(played.started.plan->format.sampleRate, 352800u);
    EXPECT_EQ(played.started.plan->modulatorOrder, 7u);

    played = play(file, asioUpsampling(DsdUpsampling::Highest), Carrier::Native, dsd(256));
    ASSERT_TRUE(played.started.plan);
    EXPECT_EQ(played.started.plan->dsd, DsdPath::Native);
    EXPECT_EQ(played.started.plan->format.sampleRate, dsd(256));

    // A network player takes DSD as DSF, which its output makes of the native stream.
    Settings dsf;
    dsf.backend = Backend::Network;
    dsf.network.dsdUpsampling = {.mode = DsdUpsampling::Set, .rate = 128};
    played = play(file, dsf, Carrier::Native, dsd(256));
    ASSERT_TRUE(played.started.plan);
    EXPECT_EQ(played.started.plan->dsd, DsdPath::Native);
    EXPECT_EQ(played.started.plan->format.sampleRate, dsd(128));

    // Off, and DSD converted to PCM, keep today's paths.
    Settings off;
    played = play(file, off, Carrier::Dop, dsd(256));
    ASSERT_TRUE(played.started.plan);
    EXPECT_EQ(played.started.plan->modulatorOrder, 0u);
    EXPECT_EQ(played.started.plan->format.sampleRate, 176400u);
    Settings pcm;
    pcm.wasapi.dsdOutput = DsdOutput::Pcm;
    pcm.wasapi.dsdUpsampling = {.mode = DsdUpsampling::Highest};
    EXPECT_EQ(dsdUpsampling(pcm).mode, DsdUpsampling::Off);
}

// ReplayGain, the DSP chain and the software volume act on DSD that is modulated, and the path shows them.
TEST(DsdUpsampling, TheDspAndTheVolumeReachTheModulatedPath) {
    const auto file = sineWave("volume", 96000, 0.5, 0.4);
    auto settings = asioUpsampling(DsdUpsampling::Set, true);
    settings.volume.mode = VolumeMode::Software;
    const auto full = play(file, settings, Carrier::Native, dsd(128));
    settings.volume.db = -20.0 * std::log10(2.0);
    const auto half = play(file, settings, Carrier::Native, dsd(128));
    ASSERT_TRUE(half.started.plan);
    EXPECT_NE(half.started.plan->volumeDb, 0.0);
    EXPECT_NEAR(toneAmplitude(half.bytes, dsd(128)) / toneAmplitude(full.bytes, dsd(128)), 0.5, 0.005);

    settings.volume.muted = true;
    const auto muted = play(file, settings, Carrier::Native, dsd(128));
    ASSERT_TRUE(muted.started.plan);
    EXPECT_TRUE(muted.started.plan->muted);
    EXPECT_LT(toneAmplitude(muted.bytes, dsd(128)), 1e-4);

    settings.volume = {};
    settings.crossfeed.enabled = true;
    const auto crossfed = play(file, settings, Carrier::Native, dsd(128));
    ASSERT_TRUE(crossfed.started.plan);
    EXPECT_EQ(crossfed.started.plan->dsp, std::vector<DspNode>{DspNode::Crossfeed});
}

// A DSP stage that falls behind the device stops playback and says the computer cannot keep up with the rate.
TEST(DsdUpsampling, ModulationThatFallsBehindStopsAndSaysSo) {
    const auto file = sineWave("slow", 44100, 0.5, 4.0);
    setDspStageDelayForTest(400ms);
    const auto played = play(file, asioUpsampling(DsdUpsampling::Set, true), Carrier::Native, dsd(256));
    setDspStageDelayForTest(0ms);
    ASSERT_TRUE(played.ended.error);
    EXPECT_EQ(*played.ended.error, ErrorCode::TooSlow);
    EXPECT_NE(played.ended.errorDetail.find("DSD128"), std::string::npos) << played.ended.errorDetail;
    EXPECT_EQ(played.ended.state, PlayState::Stopped);
}

TEST(DsdUpsamplingSettings, EachOutputKeepsItsChoiceAndOrder) {
    Settings settings;
    settings.wasapi.dsdUpsampling = {.mode = DsdUpsampling::Multiple, .multiple = 4, .rate = 256, .pcm = true};
    settings.wasapi.filter.modulatorOrder = 5;
    settings.asio.dsdUpsampling = {.mode = DsdUpsampling::Highest};
    settings.ks.dsdUpsampling = {.mode = DsdUpsampling::Set, .rate = 256};
    settings.network.dsdUpsampling = {.mode = DsdUpsampling::Set, .pcm = true};
    SavedOutput saved;
    saved.backend = Backend::Asio;
    saved.id = saved.asio.driverClsid = "{driver}";
    saved.asio.dsdUpsampling = {.mode = DsdUpsampling::Multiple, .multiple = 2};
    saved.asio.filter.modulatorOrder = 5;
    settings.outputs.push_back(saved);
    const Settings back = fromJson(toJson(settings));
    EXPECT_EQ(back.wasapi, settings.wasapi);
    EXPECT_EQ(back.asio, settings.asio);
    EXPECT_EQ(back.ks, settings.ks);
    EXPECT_EQ(back.network.dsdUpsampling, settings.network.dsdUpsampling);
    ASSERT_EQ(back.outputs.size(), 1u);
    EXPECT_EQ(back.outputs[0].asio, saved.asio);
}

TEST(DsdUpsamplingSettings, OffByDefaultAndInvalidValuesFallBack) {
    EXPECT_EQ(fromJson("{}").asio.dsdUpsampling, DsdUpsamplingSettings{});
    EXPECT_EQ(fromJson("{}").asio.filter.modulatorOrder, 7u);
    const auto parsed = fromJson(R"({"ks":{"dsdUpsampling":{"mode":"best","multiple":8,"rate":512,"pcm":1},
        "filter":{"modulatorOrder":6}}})");
    EXPECT_EQ(parsed.ks.dsdUpsampling, DsdUpsamplingSettings{});
    EXPECT_EQ(parsed.ks.filter.modulatorOrder, 7u);
}

TEST(DsdUpsamplingSettings, OnlyOutputsThatSendDsdUpsampleIt) {
    Settings settings;
    settings.wasapi.dsdUpsampling = {.mode = DsdUpsampling::Highest};
    EXPECT_EQ(dsdUpsampling(settings).mode, DsdUpsampling::Highest);
    settings.wasapi.shared = true;
    EXPECT_EQ(dsdUpsampling(settings).mode, DsdUpsampling::Off);
    for (const Backend backend : {Backend::AirPlay, Backend::HQPlayer}) {
        settings.backend = backend;
        EXPECT_EQ(dsdUpsampling(settings).mode, DsdUpsampling::Off);
    }
}
