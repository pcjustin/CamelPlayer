// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Several Control points at once (docs/Plan.md §17.8): each follows a zone of its own, sees what the others do, and
// finds the Server again when it was lost.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <thread>

#include "Common/util/Text.h"
#include "OutputBridge/CaptureOutput.h"
#include "Server/Playback/LocalServer.h"
#include "Server/Playback/Outputs.h"
#include "Server/Remote/ApiServer.h"
#include "Server/Remote/RemoteServer.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::server;
using namespace std::chrono_literals;

namespace {

std::string zone(size_t number) {
    return zoneId(Backend::Wasapi, "zone-" + std::to_string(number));
}

class ControlPointsTest : public ::testing::Test {
protected:
    std::filesystem::path dir;
    std::unique_ptr<LocalServer> local;
    std::unique_ptr<ApiServer> api;
    uint16_t port = 0;
    std::vector<std::unique_ptr<RemoteServer>> points;

    void SetUp() override {
        dir = testutil::tempDir() / "control-points" /
              (std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()) + "-" +
               std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        std::filesystem::create_directories(dir);
        Settings s;
        s.autoplay = false;
        s.wasapi.deviceId = "zone-1";
        for (size_t i = 1; i <= 2; ++i) {
            SavedOutput output;
            output.backend = Backend::Wasapi;
            output.id = output.wasapi.deviceId = "zone-" + std::to_string(i);
            output.name = "Zone " + std::to_string(i);
            output.enabled = true;
            output.zone.autoplay = false;
            s.outputs.push_back(output);
        }
        local = std::make_unique<LocalServer>(fromJson(toJson(s)), std::filesystem::path{}, [this](const Settings& settings) {
            audio::CaptureOptions options;
            options.file = dir / (settings.wasapi.deviceId + ".raw");
            options.accepts = [](const audio::DeviceFormat& f) { return f.type == audio::SampleType::Int32; };
            options.realtime = true;
            return audio::createCaptureOutput(options);
        });
        local->waitIdle();
        serve(0);
    }

    void TearDown() override {
        points.clear();
        api.reset();
        if (local) local->shutdown();
        local.reset();
    }

    void serve(uint16_t at) {
        ApiServer::Options options;
        options.port = at;
        options.portsToTry = 1;
        options.loopbackOnly = true;
        options.serverId = "test-server";
        options.serverName = "Test PC";
        options.pairings = dir / "pairings.json";
        api = std::make_unique<ApiServer>(*local, options);
        port = api->port();
    }

    // The next Control point, "Control <n>", paired with a code after the first; it keeps its own settings.
    RemoteServer& connect() {
        const auto name = "Control " + std::to_string(points.size() + 1);
        const auto paired = RemoteServer::pair("127.0.0.1", port, name, points.empty() ? "" : api->pairingCode());
        RemoteServer::Options options;
        options.host = "127.0.0.1";
        options.port = port;
        options.cache = dir / "cache";
        options.token = paired.token;
        options.ownSettings = dir / (name + ".json");
        points.push_back(std::make_unique<RemoteServer>(options));
        return *points.back();
    }

    void play(IServer& server, size_t number) {
        Command c;
        c.kind = CommandKind::QueueReplace;
        c.zone = zone(number);
        c.sources.emplace_back(toUtf8(testutil::testFile("16-44.1k").wstring()));
        c.sources.emplace_back(toUtf8(testutil::testFile("16-44.1k").wstring()));
        server.submit(c);
        server.waitIdle();
    }

    static bool within(const std::function<bool()>& done, std::chrono::milliseconds time = 10s) {
        const auto deadline = std::chrono::steady_clock::now() + time * testutil::kSlowdown;
        while (std::chrono::steady_clock::now() < deadline) {
            if (done()) return true;
            std::this_thread::sleep_for(20ms);
        }
        return done();
    }

    static const ZoneSnapshot& of(const Snapshot& s, size_t number) {
        const auto found = std::ranges::find(s.zones, zone(number), &ZoneSnapshot::id);
        if (found == s.zones.end()) throw std::runtime_error("no zone " + zone(number));
        return *found;
    }

    static PlaybackState state(const IServer& server, size_t number) { return of(server.snapshot(), number).playback.state; }
};

} // namespace

// Ten Control points see every zone change, each following its own zone: the first zone before any played, and for one
// new to the Server, the zone playing.
TEST_F(ControlPointsTest, TenControlPointsSeeEveryZoneAndANewOneFollowsTheZonePlaying) {
    for (int i = 0; i < 10; ++i) connect();
    for (const auto& point : points) EXPECT_EQ(point->snapshot().selectedZone, zone(1));
    play(*local, 2);
    for (const auto& point : points)
        EXPECT_TRUE(within([&] { return state(*point, 2) == PlaybackState::Playing; })) << "every Control point sees it";
    auto& late = connect();
    EXPECT_EQ(late.snapshot().selectedZone, zone(2));
    EXPECT_EQ(late.snapshot().playback.state, PlaybackState::Playing) << "the zone followed is the one shown";
    EXPECT_EQ(points.front()->snapshot().selectedZone, zone(1)) << "the others keep theirs";
}

// A zone chosen on a Control point is followed there alone: the Server's own output in use stays, and the Control
// point's commands go to its zone.
TEST_F(ControlPointsTest, AControlPointFollowsTheZoneItChoosesAndCommandsGoThere) {
    auto& point = connect();
    auto settings = nativeSettings(point.snapshot().settings);
    selectOutput(settings, Backend::Wasapi, "zone-2", point.snapshot().devices);
    Command choose;
    choose.kind = CommandKind::ApplySettings;
    choose.settings = settingsDto(settings);
    point.submit(choose);
    point.waitIdle();
    EXPECT_EQ(point.snapshot().selectedZone, zone(2));
    EXPECT_EQ(local->snapshot().selectedZone, zone(1));
    EXPECT_EQ(local->snapshot().settings.wasapi.deviceId, "zone-1");

    play(*local, 1);
    play(*local, 2);
    Command pause;
    pause.kind = CommandKind::Pause;
    point.submit(pause);
    point.waitIdle();
    EXPECT_TRUE(within([&] { return state(*local, 2) == PlaybackState::Paused; }));
    EXPECT_EQ(state(*local, 1), PlaybackState::Playing);
    const auto change = of(local->snapshot(), 2).change;
    EXPECT_EQ(change.kind, "paused");
    EXPECT_EQ(change.name, "Control 1");
    EXPECT_EQ(change.device, point.device());
    EXPECT_TRUE(within([&] { return of(point.snapshot(), 2).change.name == "Control 1"; }));

    EXPECT_TRUE(within([&] {
        const auto devices = api->pairedDevices();
        return devices.size() == 1 && devices[0]["connected"] == true && devices[0]["zone"] == zone(2);
    })) << "the paired devices list shows the zone it follows";
}

// Transfer to… takes every Control point that followed the queue to its new zone, the one that moved it included.
TEST_F(ControlPointsTest, ATransferTakesTheControlPointsFollowingTheQueueAlong) {
    auto& mover = connect();
    auto& other = connect();
    play(*local, 1);
    Command transfer;
    transfer.kind = CommandKind::Transfer;
    transfer.target = zone(2);
    mover.submit(transfer);
    mover.waitIdle();
    for (auto* point : {&mover, &other}) EXPECT_TRUE(within([&] { return point->snapshot().selectedZone == zone(2); }));
    EXPECT_EQ(of(other.snapshot(), 1).change.kind, "transferred");
}

// Two Control points changing one setting at once: the last to arrive is kept, and both show it.
TEST_F(ControlPointsTest, ASettingChangedFromTwoKeepsTheLast) {
    auto& first = connect();
    auto& second = connect();
    for (auto [point, preset] : {std::pair{&first, "First"}, std::pair{&second, "Second"}}) {
        auto settings = point->snapshot().settings;
        settings.eq.enabled = true;
        settings.eq.preset = preset;
        Command command;
        command.kind = CommandKind::ApplySettings;
        command.settings = settings;
        point->submit(command);
    }
    local->waitIdle();
    const auto kept = local->snapshot().settings.eq.preset;
    EXPECT_EQ(kept, "Second");
    for (auto* point : {&first, &second}) EXPECT_TRUE(within([&] { return point->snapshot().settings.eq.preset == kept; }));
}

// Two Control points removing entries of one queue at once: each removes its own, by entry ID.
TEST_F(ControlPointsTest, TwoControlPointsEditingOneQueueEachKeepTheirChange) {
    auto& first = connect();
    auto& second = connect();
    Command fill;
    fill.kind = CommandKind::QueueAdd;
    fill.zone = zone(1);
    for (int i = 0; i < 4; ++i) fill.sources.emplace_back(toUtf8(testutil::testFile("16-44.1k").wstring()));
    local->submit(fill);
    local->waitIdle();
    const auto entries = local->snapshot().queue.entries;
    ASSERT_EQ(entries.size(), 4U);
    std::thread a([&] {
        Command remove;
        remove.kind = CommandKind::QueueRemove;
        remove.entryIds = {entries[1].id};
        first.submit(remove);
    });
    std::thread b([&] {
        Command remove;
        remove.kind = CommandKind::QueueRemove;
        remove.entryIds = {entries[2].id};
        second.submit(remove);
    });
    a.join();
    b.join();
    local->waitIdle();
    const auto left = of(local->snapshot(), 1).queue.entries;
    ASSERT_EQ(left.size(), 2U);
    EXPECT_EQ(left[0].id, entries[0].id);
    EXPECT_EQ(left[1].id, entries[3].id);
}

// A Server gone and back: the Control point says it is lost, then connects again by itself with the Server's state.
TEST_F(ControlPointsTest, AControlPointConnectsAgainOnceTheServerIsBack) {
    auto& point = connect();
    EXPECT_EQ(point.link(), IServer::Link::Connected);
    const auto at = port;
    api.reset();
    EXPECT_TRUE(within([&] { return point.link() == IServer::Link::Lost; }));
    EXPECT_THROW(point.libraryRoots(), std::runtime_error);
    play(*local, 2);
    serve(at);
    EXPECT_TRUE(within([&] { return point.link() == IServer::Link::Connected; }, 15s));
    EXPECT_TRUE(within([&] { return state(point, 2) == PlaybackState::Playing; }));
    EXPECT_TRUE(within([&] {
        const auto devices = api->pairedDevices();
        return devices.size() == 1 && devices[0]["zone"] == point.snapshot().selectedZone;
    })) << "the Server it connected to again knows the zone it follows";
}

// A revoked Control point is told so rather than trying again.
TEST_F(ControlPointsTest, ARevokedControlPointStopsTrying) {
    auto& point = connect();
    api->revoke(point.device());
    EXPECT_TRUE(within([&] { return point.link() == IServer::Link::Refused; }, 15s));
}

// A Control point's new name may not be another paired device's.
TEST_F(ControlPointsTest, ARenameToATakenNameIsAskedToChange) {
    auto& first = connect();
    connect();
    const auto taken = first.renameDevice("Control 2");
    EXPECT_EQ(taken["error"], "nameTaken");
    EXPECT_EQ(taken["suggestion"], "Control 2 (2)");
    EXPECT_EQ(first.renameDevice("Study PC")["name"], "Study PC");
    const auto devices = api->pairedDevices();
    EXPECT_TRUE(std::ranges::any_of(devices, [](const nlohmann::json& d) { return d["name"] == "Study PC"; }));
    Command stop;
    stop.kind = CommandKind::Stop;
    play(*local, 1);
    first.submit(stop);
    first.waitIdle();
    EXPECT_EQ(of(local->snapshot(), 1).change.name, "Study PC") << "its later changes carry the new name";
}
