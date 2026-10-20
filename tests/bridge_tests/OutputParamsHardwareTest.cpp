// docs/Plan.md §6 output parameters on Chord/DAVE: WASAPI polling buffer and MMCSS, MMCSS on KS, a fixed sample rate,
// silence after a DoP-to-PCM switch and release of the device after a pause, each for a few seconds of real playback.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>

#include "Engine/PlaybackEngine.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::audio;
using namespace std::chrono_literals;

namespace {

bool selectDave(Settings& settings, Backend backend) {
    const auto devices = backend == Backend::Wasapi ? listWasapiDevices() : listKsPins();
    const auto it = std::find_if(devices.begin(), devices.end(), [](const DeviceEntry& device) {
        return device.name.find("Chord") != std::string::npos && device.name.find("unavailable") == std::string::npos;
    });
    if (it == devices.end()) return false;
    settings.backend = backend;
    (backend == Backend::Wasapi ? settings.wasapi.deviceId : settings.ks.deviceId) = it->id;
    return true;
}

std::filesystem::path testdata(const char* name) {
    return testutil::tempDir().parent_path().parent_path() / "testdata" / name;
}

const char* kFlac96 = "Sample_BeeMoved_96kHz24bit.flac";
const char* kDsf = "Dayan Kai - 15 November 2012 - 01 - When You Know - 2.8MHz-DSF.dsf";

// Plays for `duration` and checks the position advances without errors or underruns.
EngineSnapshot playFor(PlaybackEngine& engine, std::chrono::milliseconds duration) {
    engine.waitIdle();
    const auto start = engine.snapshot();
    EXPECT_FALSE(start.error) << start.errorDetail;
    EXPECT_EQ(start.state, PlayState::Playing);
    std::this_thread::sleep_for(duration);
    const auto end = engine.snapshot();
    EXPECT_FALSE(end.error) << end.errorDetail;
    EXPECT_GT(end.position, start.position);
    EXPECT_EQ(end.underruns, 0u);
    return end;
}

std::string adjustments(const EngineSnapshot& s) {
    std::string text;
    for (const auto& a : s.outputStatus.adjustments) text += a + "; ";
    return text;
}

} // namespace

// Opt in with --gtest_also_run_disabled_tests --gtest_filter=OutputParamsHardware.*
TEST(OutputParamsHardware, DISABLED_WasapiPollingBufferAndCriticalMmcss) {
    Settings settings;
    ASSERT_TRUE(selectDave(settings, Backend::Wasapi));
    settings.wasapi.eventDriven = false;
    settings.wasapi.pollBufferMs = 100;
    settings.wasapi.mmcssTask = MmcssTask::ProAudio;
    settings.wasapi.mmcssPriority = MmcssPriority::Critical;
    PlaybackEngine engine(settings);
    engine.openAndPlay(testdata(kFlac96));
    const auto s = playFor(engine, 3s);
    EXPECT_NE(s.outputStatus.mode.find("polling"), std::string::npos) << s.outputStatus.mode;
    EXPECT_TRUE(s.outputStatus.adjustments.empty()) << adjustments(s);
    RecordProperty("buffer", std::to_string(s.outputStatus.bufferFrames));
    EXPECT_NEAR(double(s.outputStatus.bufferFrames), 9600.0, 960.0); // 100 ms at 96 kHz
    engine.stop();
    engine.waitIdle();
}

TEST(OutputParamsHardware, DISABLED_KsPlaybackMmcssAtNormalPriority) {
    Settings settings;
    ASSERT_TRUE(selectDave(settings, Backend::Ks));
    settings.ks.mmcssTask = MmcssTask::Playback;
    settings.ks.mmcssPriority = MmcssPriority::Normal;
    PlaybackEngine engine(settings);
    engine.openAndPlay(testdata(kFlac96));
    const auto s = playFor(engine, 3s);
    EXPECT_TRUE(s.outputStatus.adjustments.empty()) << adjustments(s);
    engine.stop();
    engine.waitIdle();
}

TEST(OutputParamsHardware, DISABLED_FixedRateResamplesTo192k) {
    Settings settings;
    ASSERT_TRUE(selectDave(settings, Backend::Wasapi));
    settings.wasapi.fixedRate = 192000;
    PlaybackEngine engine(settings);
    engine.openAndPlay(testdata(kFlac96));
    const auto s = playFor(engine, 3s);
    ASSERT_TRUE(s.output && s.plan);
    EXPECT_EQ(s.output->sampleRate, 192000u);
    EXPECT_TRUE(s.plan->resample);
    EXPECT_EQ(s.plan->state, PipelineState::Processed);
    EXPECT_TRUE(s.outputStatus.adjustments.empty()) << adjustments(s);
    engine.stop();
    engine.waitIdle();
}

TEST(OutputParamsHardware, DISABLED_SilenceAfterDopToPcmSwitch) {
    Settings settings;
    ASSERT_TRUE(selectDave(settings, Backend::Wasapi));
    settings.wasapi.timing.formatChangeSilenceMs = 1000;
    PlaybackEngine engine(settings);
    engine.openAndPlay(testdata(kDsf));
    engine.waitIdle();
    ASSERT_TRUE(engine.snapshot().plan);
    EXPECT_EQ(engine.snapshot().plan->dsd, DsdPath::Dop);
    std::this_thread::sleep_for(1000ms); // the first open is a format change too
    playFor(engine, 2s);
    engine.openAndPlay(testdata(kFlac96));
    engine.waitIdle();
    const auto opened = std::chrono::steady_clock::now();
    ASSERT_TRUE(engine.snapshot().plan);
    EXPECT_EQ(engine.snapshot().plan->dsd, DsdPath::None);
    while (engine.snapshot().position == 0 && std::chrono::steady_clock::now() - opened < 5s) std::this_thread::sleep_for(5ms);
    const auto silent = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - opened);
    RecordProperty("silenceMs", std::to_string(silent.count()));
    EXPECT_GE(silent.count(), 900);
    EXPECT_LT(silent.count(), 2000);
    playFor(engine, 2s);
    engine.stop();
    engine.waitIdle();
}

TEST(OutputParamsHardware, DISABLED_ReleaseAfterPauseAndResume) {
    Settings settings;
    ASSERT_TRUE(selectDave(settings, Backend::Wasapi));
    settings.wasapi.timing.releaseAfterPauseSeconds = 2;
    PlaybackEngine engine(settings);
    engine.openAndPlay(testdata(kFlac96));
    playFor(engine, 2s);
    engine.pause();
    engine.waitIdle();
    const auto paused = engine.snapshot();
    ASSERT_EQ(paused.state, PlayState::Paused);
    std::this_thread::sleep_for(1000ms);
    EXPECT_TRUE(engine.snapshot().output) << "released early";
    std::this_thread::sleep_for(1500ms);
    engine.waitIdle();
    const auto released = engine.snapshot();
    EXPECT_FALSE(released.output);
    EXPECT_EQ(released.state, PlayState::Paused);
    EXPECT_FALSE(released.error) << released.errorDetail;
    EXPECT_EQ(released.position, paused.position);
    engine.play();
    const auto resumed = playFor(engine, 2s);
    EXPECT_GT(resumed.position, paused.position);
    engine.stop();
    engine.waitIdle();
}
