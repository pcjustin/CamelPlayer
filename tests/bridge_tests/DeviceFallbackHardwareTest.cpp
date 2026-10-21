// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Saved devices and parameters on real drivers (docs/Plan.md M2-R10). Opt in with
// --gtest_also_run_disabled_tests --gtest_filter=DeviceFallbackHardware.*
#include <gtest/gtest.h>

#include <algorithm>
#include <thread>

#include "Engine/PlaybackEngine.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::audio;
using namespace std::chrono_literals;

namespace {

std::filesystem::path sample() {
    return testutil::tempDir().parent_path().parent_path() / "testdata" / "Sample_BeeMoved_96kHz24bit.flac";
}

std::string chordAsio() {
    for (const auto& d : listAsioDrivers())
        if (d.name.find("Chord") != std::string::npos && d.name.find("unavailable") == std::string::npos) return d.id;
    return {};
}

} // namespace

// An ASIO buffer the driver cannot take and output channels it does not have: the driver's preferred buffer and
// channels 1-2 play, the status reports both, and the settings keep the requested values.
TEST(DeviceFallbackHardware, DISABLED_AsioInvalidSavedParametersUseDriverDefaults) {
    Settings settings;
    settings.backend = Backend::Asio;
    settings.asio.driverClsid = chordAsio();
    ASSERT_FALSE(settings.asio.driverClsid.empty());
    settings.asio.bufferSize = 100;
    settings.asio.outLeft = 40;
    settings.asio.outRight = 41;
    ASSERT_TRUE(std::filesystem::exists(sample()));
    PlaybackEngine engine(settings);
    engine.openAndPlay(sample());
    engine.waitIdle();
    std::this_thread::sleep_for(1s);
    const auto snap = engine.snapshot();
    ASSERT_FALSE(snap.error) << snap.errorDetail;
    EXPECT_EQ(snap.state, PlayState::Playing);
    EXPECT_EQ(snap.underruns, 0u);
    const auto& adjusted = snap.outputStatus.adjustments;
    ASSERT_EQ(adjusted.size(), 2u);
    EXPECT_NE(adjusted[0].find("ASIO buffer: requested 100"), std::string::npos) << adjusted[0];
    EXPECT_NE(adjusted[1].find("ASIO output channels: requested 41/42, using 1/2"), std::string::npos) << adjusted[1];
    for (const auto& line : adjusted) RecordProperty("adjustment", line);
    engine.stop();
    engine.waitIdle();
}

// A saved device that is not there fails with a notice; nothing else plays in its place.
TEST(DeviceFallbackHardware, DISABLED_MissingSavedDevicesAreNotReplaced) {
    for (const Backend backend : {Backend::Wasapi, Backend::Asio, Backend::Ks}) {
        Settings settings;
        settings.backend = backend;
        settings.wasapi.deviceId = "{0.0.0.00000000}.{00000000-0000-0000-0000-000000000000}";
        settings.asio.driverClsid = "{00000000-0000-0000-0000-000000000001}";
        settings.ks.deviceId = "\\\\?\\usb#vid_0000&pid_0000#0#{65e8773e-8f56-11d0-a3b9-00a0c9223196}\\wave|0";
        PlaybackEngine engine(settings);
        engine.openAndPlay(sample());
        engine.waitIdle();
        const auto snap = engine.snapshot();
        EXPECT_EQ(snap.state, PlayState::Stopped) << backendName(backend);
        ASSERT_TRUE(snap.error) << backendName(backend);
        EXPECT_EQ(*snap.error, ErrorCode::DeviceNotFound) << backendName(backend) << ": " << snap.errorDetail;
        EXPECT_NE(snap.errorDetail.find("not connected"), std::string::npos) << snap.errorDetail;
        EXPECT_FALSE(snap.output) << backendName(backend);
    }
}
