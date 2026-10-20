#include <gtest/gtest.h>

#include <algorithm>
#include <thread>
#include <tuple>

#include "Engine/PlaybackEngine.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::audio;
using namespace std::chrono_literals;

namespace {
struct Route {
    Backend backend;
    const char* device;
};

const Route routes[] = {{Backend::Wasapi, "Chord"},
                        {Backend::Asio, "Chord"},
                        {Backend::Ks, "Chord"},
                        {Backend::Wasapi, "Cirrus"},
                        {Backend::Ks, "Cirrus"}};

bool selectDevice(Settings& settings, Backend backend, const char* name) {
    const auto devices = backend == Backend::Wasapi ? listWasapiDevices()
                         : backend == Backend::Asio ? listAsioDrivers()
                                                    : listKsPins();
    const auto it = std::find_if(devices.begin(), devices.end(), [&](const DeviceEntry& device) {
        return device.name.find(name) != std::string::npos && device.name.find("unavailable") == std::string::npos;
    });
    if (it == devices.end()) return false;
    settings.backend = backend;
    if (backend == Backend::Wasapi)
        settings.wasapi.deviceId = it->id;
    else if (backend == Backend::Asio)
        settings.asio.driverClsid = it->id;
    else
        settings.ks.deviceId = it->id;
    return true;
}

// When playback does not advance, a 3 s timeline shows whether it starts late or stays stuck.
std::string stallTrace(PlaybackEngine& engine) {
    std::string trace;
    for (int i = 0; i < 30; i++) {
        std::this_thread::sleep_for(100ms);
        const auto s = engine.snapshot();
        trace += " " + std::to_string(s.position) + (s.error ? "!" : "") + "/u" + std::to_string(s.underruns);
    }
    const auto s = engine.snapshot();
    return "positions every 100 ms:" + trace + "; " + s.outputStatus.mode + ", " + s.outputStatus.detail + ", buffer " +
           std::to_string(s.outputStatus.bufferFrames) + (s.error ? ", error " + s.errorDetail : "");
}

void expectPlaying(PlaybackEngine& engine, Backend backend, DecoderKind decoder, const char* phase) {
    SCOPED_TRACE(phase);
    engine.waitIdle();
    auto snap = engine.snapshot();
    ASSERT_FALSE(snap.error) << snap.errorDetail;
    ASSERT_EQ(snap.state, PlayState::Playing);
    ASSERT_TRUE(snap.output);
    ASSERT_TRUE(snap.source);
    EXPECT_EQ(snap.backend, backend);
    EXPECT_EQ(snap.decoder, decoder);
    EXPECT_EQ(snap.output->sampleRate, snap.source->sampleRate);
    EXPECT_TRUE(snap.bitPerfect());
    const auto position = snap.position;
    std::this_thread::sleep_for(350ms);
    snap = engine.snapshot();
    EXPECT_FALSE(snap.error) << snap.errorDetail;
    EXPECT_GT(snap.position, position) << stallTrace(engine);
    EXPECT_EQ(snap.underruns, 0u);
}
} // namespace

class HardwarePlayback : public ::testing::TestWithParam<std::tuple<int, DecoderKind>> {};

std::string hardwareCaseName(const ::testing::TestParamInfo<HardwarePlayback::ParamType>& info) {
    const auto [routeIndex, decoder] = info.param;
    return std::string(routes[routeIndex].device) + "_" + backendId(routes[routeIndex].backend) + "_" + decoderId(decoder);
}

// Opt in with --gtest_also_run_disabled_tests --gtest_filter=M0_21/*.
TEST_P(HardwarePlayback, DISABLED_PlayPauseSeekStopAndSwitch) {
    const auto [routeIndex, decoder] = GetParam();
    const auto route = routes[routeIndex];
    Settings settings;
    settings.decoderByFormat[AudioFormat::Flac] = decoder;
    ASSERT_TRUE(selectDevice(settings, route.backend, route.device));
    const auto file = testutil::tempDir().parent_path().parent_path() / "testdata" / "Sample_BeeMoved_96kHz24bit.flac";
    ASSERT_TRUE(std::filesystem::exists(file));
    RecordProperty("device", route.device);
    RecordProperty("backend", backendName(route.backend));
    RecordProperty("decoder", decoderName(decoder));
    PlaybackEngine engine(settings);
    engine.openAndPlay(file);
    ASSERT_NO_FATAL_FAILURE(
        expectPlaying(engine, settings.backend, decoderFor(settings.decoderByFormat, AudioFormat::Flac), "start"));
    engine.pause();
    engine.waitIdle();
    ASSERT_EQ(engine.snapshot().state, PlayState::Paused);
    std::this_thread::sleep_for(150ms);
    const auto pausedPosition = engine.snapshot().position;
    std::this_thread::sleep_for(150ms);
    EXPECT_EQ(engine.snapshot().position, pausedPosition);
    engine.play();
    ASSERT_NO_FATAL_FAILURE(
        expectPlaying(engine, settings.backend, decoderFor(settings.decoderByFormat, AudioFormat::Flac), "resume"));
    const uint64_t target = 10ull * engine.snapshot().source->sampleRate;
    engine.seek(target);
    engine.waitIdle();
    ASSERT_NO_FATAL_FAILURE(
        expectPlaying(engine, settings.backend, decoderFor(settings.decoderByFormat, AudioFormat::Flac), "seek"));
    EXPECT_GE(engine.snapshot().position, target);
    EXPECT_LT(engine.snapshot().position, target + 2ull * engine.snapshot().source->sampleRate);
    for (const auto nextDecoder : decodersFor(AudioFormat::Flac)) {
        settings.decoderByFormat[AudioFormat::Flac] = nextDecoder;
        const auto before = engine.snapshot().position;
        engine.applySettings(settings);
        ASSERT_NO_FATAL_FAILURE(
            expectPlaying(engine, settings.backend, decoderFor(settings.decoderByFormat, AudioFormat::Flac), "decoder switch"));
        EXPECT_GE(engine.snapshot().position, before);
    }
    for (const auto nextRoute : routes) {
        if (std::string(nextRoute.device) != route.device || nextRoute.backend == route.backend) continue;
        ASSERT_TRUE(selectDevice(settings, nextRoute.backend, route.device));
        const auto before = engine.snapshot().position;
        engine.applySettings(settings);
        ASSERT_NO_FATAL_FAILURE(
            expectPlaying(engine, settings.backend, decoderFor(settings.decoderByFormat, AudioFormat::Flac), "backend switch"));
        EXPECT_GE(engine.snapshot().position, before);
    }
    engine.stop();
    engine.waitIdle();
    EXPECT_EQ(engine.snapshot().state, PlayState::Stopped);
    EXPECT_EQ(engine.snapshot().position, 0u);
    EXPECT_FALSE(engine.snapshot().output);
    engine.play();
    ASSERT_NO_FATAL_FAILURE(
        expectPlaying(engine, settings.backend, decoderFor(settings.decoderByFormat, AudioFormat::Flac), "replay"));
    engine.stop();
    engine.waitIdle();
}

INSTANTIATE_TEST_SUITE_P(M0_21, HardwarePlayback,
                         ::testing::Combine(::testing::Values(0, 1, 2, 3, 4),
                                            ::testing::ValuesIn(decodersFor(AudioFormat::Flac))),
                         hardwareCaseName);
