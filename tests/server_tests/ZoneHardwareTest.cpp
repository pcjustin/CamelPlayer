// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// One device, one zone on the DAVE and the Cirrus (docs/Task-breakdown.md MZ-R04): the DAVE's backends are one device,
// and its ASIO driver plays beside FlexASIO on the Cirrus. Quiet tones, -50 dBFS.
// Opt in with --gtest_also_run_disabled_tests --gtest_filter=ZoneHardware.*
#include <gtest/gtest.h>

#include <algorithm>
#include <thread>

#include "Common/util/Text.h"
#include "OutputBridge/IOutput.h"
#include "Server/Playback/LocalServer.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::audio;
using namespace camel::server;
using namespace std::chrono_literals;

namespace {

std::optional<DeviceEntry> named(const std::vector<DeviceEntry>& devices, const char* part) {
    const auto found = std::ranges::find_if(devices, [&](const DeviceEntry& d) {
        return d.name.find(part) != std::string::npos && d.name.find("unavailable") == std::string::npos;
    });
    return found == devices.end() ? std::nullopt : std::optional(*found);
}

SavedOutput enabled(Backend backend, const std::string& id) {
    SavedOutput output;
    output.backend = backend;
    output.id = output.wasapi.deviceId = output.asio.driverClsid = output.ks.deviceId = id;
    output.enabled = true;
    output.zone.autoplay = false;
    return output;
}

const ZoneSnapshot& in(const Snapshot& s, const std::string& id) {
    const auto found = std::ranges::find(s.zones, id, &ZoneSnapshot::id);
    if (found == s.zones.end()) throw std::runtime_error("no zone " + id);
    return *found;
}

Command playIn(const std::string& zone, const std::filesystem::path& file) {
    Command c;
    c.kind = CommandKind::QueueReplace;
    c.zone = zone;
    c.sources.emplace_back(toUtf8(file.wstring()));
    return c;
}

bool waitFor(LocalServer& server, const std::function<bool(const Snapshot&)>& check, std::chrono::seconds time = 15s) {
    const auto deadline = std::chrono::steady_clock::now() + time;
    while (std::chrono::steady_clock::now() < deadline) {
        if (check(server.snapshot())) return true;
        std::this_thread::sleep_for(20ms);
    }
    return false;
}

double seconds(const ZoneSnapshot& z) {
    return z.playback.source ? double(z.playback.position) / z.playback.source->sampleRate : 0.0;
}

} // namespace

TEST(ZoneHardware, DISABLED_TheDavesBackendsAreOneDeviceAndItsAsioDriverPlaysBesideFlexAsio) {
    const auto aDave = named(listAsioDrivers(), "Chord"), aFlex = named(listAsioDrivers(), "FlexASIO");
    const auto wDave = named(listWasapiDevices(), "Chord"), kDave = named(listKsPins(), "Chord");
    const auto cirrus = named(listWasapiDevices(), "Cirrus");
    if (!aDave || !wDave || !kDave || !cirrus) GTEST_SKIP() << "the DAVE or the Cirrus is not connected";
    if (!aFlex) GTEST_SKIP() << "FlexASIO is not installed";
    Settings settings;
    settings.autoplay = false;
    settings.wasapi.deviceId = cirrus->id;
    settings.outputs = {enabled(Backend::Asio, aDave->id), enabled(Backend::Wasapi, wDave->id), enabled(Backend::Ks, kDave->id),
                        enabled(Backend::Asio, aFlex->id), enabled(Backend::Wasapi, cirrus->id)};
    settings = fromJson(toJson(settings));
    const auto toneA = testutil::generatedFile({"zone-hardware-440", 96000, 24, 2, 30.0, true, -50.0});
    const auto toneB = testutil::generatedFile({"zone-hardware-48k", 48000, 24, 2, 30.0, true, -50.0});
    LocalServer server(settings);
    Command refresh;
    refresh.kind = CommandKind::RefreshDevices;
    server.submit(refresh);
    server.waitIdle();
    const auto asioDave = zoneId(Backend::Asio, aDave->id), wasapiDave = zoneId(Backend::Wasapi, wDave->id),
               ksDave = zoneId(Backend::Ks, kDave->id), flex = zoneId(Backend::Asio, aFlex->id);

    server.submit(playIn(asioDave, toneA));
    ASSERT_TRUE(waitFor(server, [&](const Snapshot& s) { return seconds(in(s, asioDave)) > 1.0; }));
    ASSERT_TRUE(waitFor(server, [&](const Snapshot& s) { return in(s, wasapiDave).inUseBy == asioDave; }));
    EXPECT_EQ(in(server.snapshot(), ksDave).inUseBy, asioDave);
    for (const auto& other : {wasapiDave, ksDave}) {
        server.submit(playIn(other, toneB));
        server.waitIdle();
        const auto s = server.snapshot();
        ASSERT_TRUE(s.commandError) << other;
        EXPECT_EQ(s.commandError->code, ErrorCode::DeviceInUse) << other;
        EXPECT_NE(s.commandError->detail.find("in use by"), std::string::npos) << s.commandError->detail;
    }

    // Two ASIO drivers at once, each with callbacks of its own.
    server.submit(playIn(flex, toneB));
    ASSERT_TRUE(waitFor(server, [&](const Snapshot& s) { return seconds(in(s, flex)) > 1.0; }));
    const auto before = server.snapshot();
    std::this_thread::sleep_for(10s);
    const auto after = server.snapshot();
    for (const auto& zone : {asioDave, flex}) {
        EXPECT_EQ(in(after, zone).playback.state, PlaybackState::Playing) << zone;
        EXPECT_FALSE(in(after, zone).playback.error) << zone << ": " << in(after, zone).playback.errorDetail;
        EXPECT_NEAR(seconds(in(after, zone)) - seconds(in(before, zone)), 10.0, 1.0) << zone;
        EXPECT_EQ(in(after, zone).playback.underruns, 0U) << zone;
    }
    for (const auto& zone : {asioDave, flex}) {
        Command stop;
        stop.kind = CommandKind::Stop;
        stop.zone = zone;
        server.submit(stop);
    }
    server.waitIdle();
}
