// M2 formats and DSD paths on the connected devices (docs/Plan.md §16.7): each case plays a local testdata file for a
// second on Chord/DAVE or the built-in Cirrus codec and checks the negotiated path. Cirrus has no DSD mode, so it only
// receives DSD converted to PCM, never DoP or native DSD.
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <thread>
#include <vector>

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

enum class Expect : uint8_t { BitPerfect, Lossy, Native, Dop, DsdToPcm };

struct FormatCase {
    const char* name;
    const char* file; // in testdata/
    DsdOutput dsd = DsdOutput::Dop;
    Expect expect = Expect::BitPerfect;
};

struct HardwareCase {
    Route route;
    FormatCase format;
};

const Route kRoutes[] = {{Backend::Wasapi, "Chord"},
                         {Backend::Asio, "Chord"},
                         {Backend::Ks, "Chord"},
                         {Backend::Wasapi, "Cirrus"},
                         {Backend::Ks, "Cirrus"}};

const FormatCase kFormats[] = {
    {"Wav", "sample-6s.wav"},
    {"Alac", "01 - Dont Know Why.m4a"},
    {"Aac", "Sample_BeeMoved_48kHz16bit.m4a", DsdOutput::Dop, Expect::Lossy},
    {"Mp3", "sample-6s.mp3", DsdOutput::Dop, Expect::Lossy},
    {"DsdNative", "Dayan Kai - 15 November 2012 - 01 - When You Know - 2.8MHz-DSF.dsf", DsdOutput::Native, Expect::Native},
    {"DsdDop", "Dayan Kai - 15 November 2012 - 01 - When You Know - 2.8MHz-DSF.dsf", DsdOutput::Dop, Expect::Dop},
    {"DsdToPcm", "Dayan Kai - 15 November 2012 - 01 - When You Know - 2.8MHz-DSF.dsf", DsdOutput::Pcm, Expect::DsdToPcm},
};

// Raw DSD only where the device decodes it: native DSD needs ASIO on DAVE, DoP needs DAVE.
std::vector<HardwareCase> cases() {
    std::vector<HardwareCase> out;
    for (const auto& route : kRoutes)
        for (const auto& format : kFormats) {
            const bool dave = std::string(route.device) == "Chord";
            if (format.expect == Expect::Native && !(dave && route.backend == Backend::Asio)) continue;
            if (format.expect == Expect::Dop && !dave) continue;
            out.push_back({route, format});
        }
    return out;
}

bool selectDevice(Settings& settings, const Route& route) {
    const auto devices = route.backend == Backend::Wasapi ? listWasapiDevices()
                         : route.backend == Backend::Asio ? listAsioDrivers()
                                                          : listKsPins();
    const auto it = std::find_if(devices.begin(), devices.end(), [&](const DeviceEntry& device) {
        return device.name.find(route.device) != std::string::npos && device.name.find("unavailable") == std::string::npos;
    });
    if (it == devices.end()) return false;
    settings.backend = route.backend;
    if (route.backend == Backend::Wasapi)
        settings.wasapi.deviceId = it->id;
    else if (route.backend == Backend::Asio)
        settings.asio.driverClsid = it->id;
    else
        settings.ks.deviceId = it->id;
    return true;
}

} // namespace

class FormatHardware : public ::testing::TestWithParam<HardwareCase> {};

std::string formatHardwareName(const ::testing::TestParamInfo<HardwareCase>& info) {
    return std::string(info.param.route.device) + "_" + backendId(info.param.route.backend) + "_" + info.param.format.name;
}

// Opt in with --gtest_also_run_disabled_tests --gtest_filter=M2/FormatHardware.*
TEST_P(FormatHardware, DISABLED_PlaysTheNegotiatedPath) {
    const auto& [route, format] = GetParam();
    Settings settings;
    ASSERT_TRUE(selectDevice(settings, route)) << route.device;
    settings.wasapi.dsdOutput = format.dsd == DsdOutput::Native ? DsdOutput::Dop : format.dsd;
    settings.asio.dsdOutput = settings.ks.dsdOutput = format.dsd;
    const auto file = testutil::tempDir().parent_path().parent_path() / "testdata" / format.file;
    ASSERT_TRUE(std::filesystem::exists(file)) << file;
    RecordProperty("device", route.device);
    RecordProperty("backend", backendName(route.backend));
    RecordProperty("format", format.name);
    PlaybackEngine engine(settings);
    engine.openAndPlay(file);
    engine.waitIdle();
    auto snap = engine.snapshot();
    ASSERT_FALSE(snap.error) << snap.errorDetail;
    ASSERT_EQ(snap.state, PlayState::Playing);
    ASSERT_TRUE(snap.plan && snap.output);
    switch (format.expect) {
    case Expect::BitPerfect:
        EXPECT_TRUE(snap.bitPerfect());
        break;
    case Expect::Lossy:
        EXPECT_EQ(snap.plan->state, PipelineState::LossySource);
        break;
    case Expect::Native:
        EXPECT_EQ(snap.plan->dsd, DsdPath::Native);
        break;
    case Expect::Dop:
        EXPECT_EQ(snap.plan->dsd, DsdPath::Dop);
        break;
    case Expect::DsdToPcm:
        EXPECT_EQ(snap.plan->dsd, DsdPath::Pcm);
        EXPECT_EQ(snap.output->sampleRate, 176400u);
        break;
    }
    const auto position = snap.position;
    std::this_thread::sleep_for(1s);
    snap = engine.snapshot();
    EXPECT_FALSE(snap.error) << snap.errorDetail;
    EXPECT_GT(snap.position, position);
    EXPECT_EQ(snap.underruns, 0u);
    engine.stop();
    engine.waitIdle();
    EXPECT_EQ(engine.snapshot().state, PlayState::Stopped);
}

INSTANTIATE_TEST_SUITE_P(M2, FormatHardware, ::testing::ValuesIn(cases()), formatHardwareName);
