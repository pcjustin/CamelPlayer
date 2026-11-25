// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Network outputs in several zones (docs/Plan.md §5.12): players, each with its own stream, remote and volume, and
// AirPlay groups, each with its own session, a receiver in one group at a time.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <thread>

#include "Common/util/Text.h"
#include "FakeAirPlayReceiver.h"
#include "FakeRenderer.h"
#include "Server/Network/AirPlayFinder.h"
#include "Server/Playback/LocalServer.h"
#include "Server/Playback/Outputs.h"
#include "Server/Remote/ApiServer.h"
#include "Server/Remote/RemoteServer.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::server;
using namespace std::chrono_literals;
using testutil::FakeMdns;
using testutil::FakeReceiverRecord;
using testutil::FakeRenderer;
using testutil::FakeRtsp;
using testutil::RendererKind;

namespace {

bool waitFor(const std::function<bool()>& check, std::chrono::milliseconds time = 20s) {
    const auto deadline = std::chrono::steady_clock::now() + time * testutil::kSlowdown;
    while (std::chrono::steady_clock::now() < deadline) {
        if (check()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return false;
}

SavedOutput enabled(Backend backend, const std::string& id) {
    SavedOutput output;
    output.backend = backend;
    output.id = output.network.udn = output.airplay.deviceId = id;
    output.enabled = true;
    output.volumeMode = VolumeMode::Player;
    output.zone.autoplay = false;
    return output;
}

Command playIn(const std::string& zone, const std::filesystem::path& file) {
    Command c;
    c.kind = CommandKind::QueueReplace;
    c.zone = zone;
    c.sources.emplace_back(toUtf8(file.wstring()));
    return c;
}

Command refresh() {
    Command c;
    c.kind = CommandKind::RefreshDevices;
    return c;
}

const ZoneSnapshot& in(const Snapshot& s, const std::string& id) {
    const auto found = std::ranges::find(s.zones, id, &ZoneSnapshot::id);
    if (found == s.zones.end()) throw std::runtime_error("no zone " + id);
    return *found;
}

bool playing(const ZoneSnapshot& z) {
    return z.playback.state == PlaybackState::Playing && z.playback.position > 0;
}

} // namespace

TEST(NetworkZonesTest, ThreePlayersPlayTheirOwnTracksAndFollowTheirOwnRemotesAndVolumes) {
    FakeRenderer one(RendererKind::UpnpAv, "uuid:fake-1", "Player 1"), two(RendererKind::UpnpAv, "uuid:fake-2", "Player 2"),
        three(RendererKind::OpenHome, "uuid:fake-3", "Player 3");
    // At the speed they play: one that ran ahead of it would be reported, and no track ends before the test does.
    for (auto* renderer : {&one, &two, &three}) renderer->limitFetch(uint64_t{44100} * 4);
    Settings settings;
    settings.autoplay = false;
    settings.backend = Backend::Network;
    settings.network.udn = "uuid:fake-1";
    settings.volume.mode = VolumeMode::Player;
    settings.outputs = {enabled(Backend::Network, "uuid:fake-1"), enabled(Backend::Network, "uuid:fake-2"),
                        enabled(Backend::Network, "uuid:fake-3")};
    settings = fromJson(toJson(settings));
    LocalServer server(settings);
    FinderOptions options;
    options.searchHost = "127.0.0.1";
    options.searchPort = one.ssdpPort();
    options.moreSearchPorts = {two.ssdpPort(), three.ssdpPort()};
    options.notifyPort = 0;
    options.searchInterval = 100ms;
    server.setFinderOptions(options);
    ASSERT_TRUE(waitFor([&] {
        server.submit(refresh());
        server.waitIdle();
        return server.snapshot().devices.network.size() == 3;
    }));
    const auto zone = [](int number) { return zoneId(Backend::Network, "uuid:fake-" + std::to_string(number)); };
    for (int i = 1; i <= 3; ++i)
        server.submit(playIn(zone(i), testutil::generatedFile({"network-zone-" + std::to_string(i), 44100, 16, 2, 30.0})));
    server.waitIdle();
    ASSERT_TRUE(waitFor([&] {
        const auto s = server.snapshot();
        return playing(in(s, zone(1))) && playing(in(s, zone(2))) && playing(in(s, zone(3)));
    }));
    // Each player was given a stream of its own.
    EXPECT_NE(one.currentUri(), two.currentUri());
    EXPECT_NE(two.currentUri(), three.currentUri());
    EXPECT_NE(one.currentUri(), three.currentUri());

    // Each player's volume is followed for its own zone.
    one.changeVolume(30, false);
    two.changeVolume(70, true);
    EXPECT_TRUE(waitFor([&] {
        const auto s = server.snapshot();
        return in(s, zone(1)).playback.playerVolume == 30U && in(s, zone(2)).playback.playerVolume == 70U &&
               in(s, zone(2)).playback.playerMuted && !in(s, zone(1)).playback.playerMuted;
    }));

    // Its own remote pauses only its zone.
    two.emitLastChange(R"(<TransportState val="PAUSED_PLAYBACK"/>)");
    ASSERT_TRUE(waitFor([&] { return in(server.snapshot(), zone(2)).playback.state == PlaybackState::Paused; }));
    EXPECT_EQ(in(server.snapshot(), zone(1)).playback.state, PlaybackState::Playing);
    EXPECT_EQ(in(server.snapshot(), zone(3)).playback.state, PlaybackState::Playing);

    // Going offline stops only its zone.
    three.disappear();
    ASSERT_TRUE(waitFor([&] { return in(server.snapshot(), zone(3)).playback.state != PlaybackState::Playing; }, 40s));
    const auto s = server.snapshot();
    EXPECT_EQ(in(s, zone(1)).playback.state, PlaybackState::Playing)
        << in(s, zone(1)).playback.position << " " << in(s, zone(1)).playback.errorDetail
        << " zone 3: " << in(s, zone(3)).playback.errorDetail << " " << int(in(s, zone(3)).playback.state);
    EXPECT_FALSE(in(s, zone(1)).playback.error);
    server.shutdown();
}

namespace {

// Three fake receivers on one fake mDNS, as AirPlayStreamingTest has one.
class AirPlayZonesTest : public ::testing::Test {
protected:
    AirPlayZonesTest() {
        const char* ids[] = {"A10000000001", "B20000000002", "C30000000003"};
        for (size_t i = 0; i < 3; ++i) {
            FakeReceiverRecord record;
            record.deviceId = ids[i];
            record.name = std::string("Receiver ") + char('A' + i);
            record.host = std::string("receiver-") + char('a' + i);
            record.port = rtsp_[i].port();
            mdns_.add(record);
        }
    }

    std::unique_ptr<LocalServer> serverFor(Settings settings) {
        settings.autoplay = false;
        auto server = std::make_unique<LocalServer>(fromJson(toJson(settings)));
        AirPlayFinderOptions options;
        options.queryHost = "127.0.0.1";
        options.queryPort = mdns_.port();
        options.groupPort = 0;
        options.searchInterval = 100ms;
        options.retryInterval = 50ms;
        server->setAirPlayFinderOptions(options);
        EXPECT_TRUE(waitFor([&] {
            server->submit(refresh());
            server->waitIdle();
            return server->snapshot().devices.airplay.size() == 3;
        }));
        return server;
    }

    // Applies a change to the settings the Server shows now.
    static void change(LocalServer& server, const std::function<void(Settings&, const DeviceCatalog&)>& edit) {
        const auto now = server.snapshot();
        Settings settings = nativeSettings(now.settings);
        edit(settings, now.devices);
        Command apply;
        apply.kind = CommandKind::ApplySettings;
        apply.settings = settingsDto(settings);
        server.submit(apply);
        server.waitIdle();
    }

    static Command group(const std::string& zone, const std::vector<std::string>& members) {
        Command c;
        c.kind = CommandKind::GroupZones;
        c.zone = zone;
        c.zones = members;
        return c;
    }

    static Settings threeAlone() {
        Settings settings;
        settings.backend = Backend::AirPlay;
        settings.airplay.deviceId = "A10000000001";
        settings.outputs = {enabled(Backend::AirPlay, "A10000000001"), enabled(Backend::AirPlay, "B20000000002"),
                            enabled(Backend::AirPlay, "C30000000003")};
        settings.volume.mode = VolumeMode::Fixed;
        for (auto& output : settings.outputs) output.volumeMode = VolumeMode::Fixed;
        return settings;
    }

    const std::string zoneA = zoneId(Backend::AirPlay, "A10000000001"), zoneB = zoneId(Backend::AirPlay, "B20000000002"),
                      zoneC = zoneId(Backend::AirPlay, "C30000000003");

    // The loudest sample a receiver was sent.
    double peak(size_t receiver) const {
        int peakValue = 0;
        for (const auto& packet : rtsp_[receiver].packets()) {
            const auto pcm = testutil::pcmOfAlac(packet.alac);
            for (size_t i = 0; i + 1 < pcm.size(); i += 2)
                peakValue = std::max(peakValue, std::abs(int(int16_t(uint8_t(pcm[i]) | uint8_t(pcm[i + 1]) << 8))));
        }
        return peakValue / 32768.0;
    }

    FakeRtsp rtsp_[3];
    FakeMdns mdns_;
};

} // namespace

// Two groups at once, A with C and B alone, each a zone with its own session; C, which B's saved group also lists, plays
// only in the group in use.
TEST_F(AirPlayZonesTest, TwoGroupsPlayAtOnceAndAReceiverPlaysInOneOfThemOnly) {
    Settings settings;
    settings.backend = Backend::AirPlay;
    settings.airplay.deviceId = "A10000000001";
    settings.airplayGroup = {"A10000000001", "C30000000003"};
    SavedOutput b = enabled(Backend::AirPlay, "B20000000002");
    b.airplayGroup = {"B20000000002", "C30000000003"};
    settings.outputs = {enabled(Backend::AirPlay, "A10000000001"), b, enabled(Backend::AirPlay, "C30000000003")};
    settings.volume.mode = VolumeMode::Fixed;
    for (auto& output : settings.outputs) output.volumeMode = VolumeMode::Fixed;
    auto server = serverFor(settings);
    const auto a = zoneId(Backend::AirPlay, "A10000000001"), bZone = zoneId(Backend::AirPlay, "B20000000002");
    auto s = server->snapshot();
    ASSERT_EQ(s.zones.size(), 2U) << "C is in A's group, so not a zone";
    EXPECT_EQ(in(s, a).name, "Receiver A + 1");
    EXPECT_EQ(in(s, bZone).name, "Receiver B");
    // A plays a loud tone, B a quiet one, so each receiver's packets tell which zone they came from.
    server->submit(playIn(a, testutil::generatedFile({"airplay-zone-loud", 44100, 16, 2, 20.0, true, -3.0})));
    server->submit(playIn(bZone, testutil::generatedFile({"airplay-zone-quiet", 44100, 16, 2, 20.0, true, -30.0})));
    server->waitIdle();
    ASSERT_TRUE(waitFor([&] {
        const auto now = server->snapshot();
        return playing(in(now, a)) && playing(in(now, bZone)) && rtsp_[0].packetCount() > 50 && rtsp_[1].packetCount() > 50 &&
               rtsp_[2].packetCount() > 50;
    }));
    EXPECT_GT(peak(0), 0.5);
    EXPECT_LT(peak(1), 0.1);
    EXPECT_GT(peak(2), 0.5); // C plays A's
    EXPECT_EQ(in(server->snapshot(), a).playback.receivers.size(), 2U);
    EXPECT_EQ(in(server->snapshot(), bZone).playback.receivers.size(), 1U);
    // Stopping one group leaves the other playing.
    Command stop;
    stop.kind = CommandKind::Stop;
    stop.zone = bZone;
    server->submit(stop);
    server->waitIdle();
    const size_t before = rtsp_[0].packetCount();
    EXPECT_TRUE(waitFor([&] { return rtsp_[0].packetCount() > before + 50; }));
    EXPECT_EQ(in(server->snapshot(), a).playback.state, PlaybackState::Playing);
    server->shutdown();
}

// C, in B's group while B does not play, moves into A's group in use, leaving B alone.
TEST_F(AirPlayZonesTest, AReceiverMovesFromAStoppedGroupIntoAnother) {
    Settings settings;
    settings.backend = Backend::AirPlay;
    settings.airplay.deviceId = "A10000000001";
    SavedOutput b = enabled(Backend::AirPlay, "B20000000002");
    b.airplayGroup = {"B20000000002", "C30000000003"};
    settings.outputs = {enabled(Backend::AirPlay, "A10000000001"), b, enabled(Backend::AirPlay, "C30000000003")};
    settings.volume.mode = VolumeMode::Fixed;
    for (auto& output : settings.outputs) output.volumeMode = VolumeMode::Fixed;
    auto server = serverFor(settings);
    const auto a = zoneId(Backend::AirPlay, "A10000000001"), bZone = zoneId(Backend::AirPlay, "B20000000002");
    ASSERT_TRUE(waitFor([&] { return in(server->snapshot(), bZone).playback.receivers.size() == 2; }));
    change(*server, [](Settings& s, const DeviceCatalog& devices) {
        leaveAirPlayGroup(s, "B20000000002", "C30000000003");
        setInAirPlayGroup(s, "C30000000003", true, devices);
    });
    ASSERT_TRUE(waitFor([&] {
        const auto now = server->snapshot();
        return in(now, a).playback.receivers.size() == 2 && in(now, bZone).playback.receivers.size() == 1;
    }));
    auto s = server->snapshot();
    EXPECT_EQ(s.zones.size(), 2U);
    EXPECT_EQ(in(s, a).name, "Receiver A + 1");
    EXPECT_EQ(in(s, bZone).name, "Receiver B");
    server->submit(playIn(a, testutil::generatedFile({"airplay-move-loud", 44100, 16, 2, 20.0, true, -3.0})));
    server->waitIdle();
    EXPECT_TRUE(waitFor([&] { return playing(in(server->snapshot(), a)) && rtsp_[2].packetCount() > 50; }));
    EXPECT_GT(peak(2), 0.5);
    EXPECT_EQ(rtsp_[1].packetCount(), 0U);
    server->shutdown();
}

// A, the receiver the playing group was formed with, taken out of it: the zone, its queue, playback and DSP go on under
// C, and A becomes an empty zone of its own.
TEST_F(AirPlayZonesTest, TheFirstReceiverTakenOutOfAPlayingGroupLeavesItsZoneToTheOthers) {
    Settings settings;
    settings.backend = Backend::AirPlay;
    settings.airplay.deviceId = "A10000000001";
    settings.airplayGroup = {"A10000000001", "C30000000003"};
    settings.outputs = {enabled(Backend::AirPlay, "A10000000001"), enabled(Backend::AirPlay, "C30000000003")};
    settings.volume.mode = VolumeMode::Fixed;
    for (auto& output : settings.outputs) output.volumeMode = VolumeMode::Fixed;
    settings.eq.enabled = true;
    settings.eq.bands[2].gainDb = -6.0;
    auto server = serverFor(settings);
    const auto a = zoneId(Backend::AirPlay, "A10000000001"), c = zoneId(Backend::AirPlay, "C30000000003");
    server->submit(playIn(a, testutil::generatedFile({"airplay-group-owner", 44100, 16, 2, 20.0, true, -3.0})));
    server->waitIdle();
    ASSERT_TRUE(waitFor([&] { return playing(in(server->snapshot(), a)) && rtsp_[2].packetCount() > 50; }));
    const auto before = in(server->snapshot(), a);
    change(*server, [](Settings& s, const DeviceCatalog& devices) { setInAirPlayGroup(s, "A10000000001", false, devices); });
    ASSERT_TRUE(waitFor([&] {
        const auto now = server->snapshot();
        return now.selectedZone == c && std::ranges::any_of(now.zones, [&](const ZoneSnapshot& z) { return z.id == a; }) &&
               playing(in(now, c));
    }));
    const auto s = server->snapshot();
    EXPECT_EQ(in(s, c).queue.entries, before.queue.entries);
    EXPECT_EQ(in(s, c).queueEntryId, before.queueEntryId);
    EXPECT_EQ(in(s, c).name, "Receiver C");
    EXPECT_TRUE(in(s, a).queue.entries.empty());
    EXPECT_EQ(in(s, a).playback.state, PlaybackState::Stopped);
    EXPECT_TRUE(s.settings.eq.enabled) << "the zone keeps its DSP";
    ASSERT_TRUE(in(s, c).playback.plan);
    EXPECT_NE(std::ranges::find(in(s, c).playback.plan->dsp, audio::DspNode::Eq), in(s, c).playback.plan->dsp.end());
    // The receiver left gets no more of the group's stream; C does.
    const size_t aPackets = rtsp_[0].packetCount(), cPackets = rtsp_[2].packetCount();
    EXPECT_TRUE(waitFor([&] { return rtsp_[2].packetCount() > cPackets + 50; }));
    EXPECT_LT(rtsp_[0].packetCount(), aPackets + 10);
    server->shutdown();
}

// Zones grouped as in Roon (MZ-R24): A, playing, takes B and C, which play its queue in step, named "Receiver A + 2";
// ungrouped, A plays on with its queue and B and C are zones of their own again, stopped.
TEST_F(AirPlayZonesTest, ZonesGroupedPlayTheQueueOfTheFirstWhichKeepsItWhenUngrouped) {
    auto server = serverFor(threeAlone());
    ASSERT_EQ(server->snapshot().zones.size(), 3U);
    server->submit(playIn(zoneA, testutil::generatedFile({"airplay-grouped", 44100, 16, 2, 30.0, true, -3.0})));
    server->waitIdle();
    ASSERT_TRUE(waitFor([&] { return playing(in(server->snapshot(), zoneA)); }));
    const auto before = in(server->snapshot(), zoneA);
    server->submit(group(zoneA, {zoneB, zoneC}));
    server->waitIdle();
    auto s = server->snapshot();
    EXPECT_FALSE(s.commandError);
    ASSERT_EQ(s.zones.size(), 1U);
    EXPECT_EQ(in(s, zoneA).name, "Receiver A + 2");
    EXPECT_EQ(nativeSettings(s.settings).airplayGroup,
              (std::vector<std::string>{"A10000000001", "B20000000002", "C30000000003"}));
    EXPECT_TRUE(waitFor([&] {
        const auto now = server->snapshot();
        return in(now, zoneA).playback.receivers.size() == 3 && playing(in(now, zoneA)) && rtsp_[1].packetCount() > 50 &&
               rtsp_[2].packetCount() > 50;
    }));
    EXPECT_GT(peak(1), 0.5);
    EXPECT_GT(peak(2), 0.5);
    EXPECT_EQ(in(server->snapshot(), zoneA).queue.entries, before.queue.entries);
    EXPECT_EQ(in(server->snapshot(), zoneA).queueEntryId, before.queueEntryId);

    const uint64_t groupedAt = in(server->snapshot(), zoneA).playback.position;
    server->submit(group(zoneA, {}));
    server->waitIdle();
    s = server->snapshot();
    ASSERT_EQ(s.zones.size(), 3U);
    EXPECT_EQ(in(s, zoneA).name, "Receiver A");
    EXPECT_EQ(in(s, zoneA).queue.entries, before.queue.entries);
    EXPECT_EQ(in(s, zoneA).queueEntryId, before.queueEntryId);
    for (const auto& other : {zoneB, zoneC}) {
        EXPECT_EQ(in(s, other).playback.state, PlaybackState::Stopped);
        EXPECT_TRUE(in(s, other).queue.entries.empty());
    }
    const size_t aPackets = rtsp_[0].packetCount(), bPackets = rtsp_[1].packetCount();
    EXPECT_TRUE(waitFor([&] { return rtsp_[0].packetCount() > aPackets + 50; }));
    // The position is what the receiver has played, which stays 0 for the 2 s of its latency after the stream starts
    // (kSyncLatency and kDefaultAudioLatency): zone A plays on past where it was, rather than at once above 0.
    EXPECT_TRUE(waitFor([&] {
        const auto after = in(server->snapshot(), zoneA).playback;
        return after.state == PlaybackState::Playing && after.position > groupedAt;
    })) << int(in(server->snapshot(), zoneA).playback.state)
        << " at " << in(server->snapshot(), zoneA).playback.position << " after " << groupedAt;
    EXPECT_LT(rtsp_[1].packetCount(), bPackets + 10);
    server->shutdown();
}

// Each member keeps its own level and its channels in the group and after it; zoneA zone not in use groups too.
TEST_F(AirPlayZonesTest, EachMemberKeepsItsLevelAndChannels) {
    auto settings = threeAlone();
    for (auto& output : settings.outputs) output.volumeMode = VolumeMode::Player;
    settings.volume.mode = VolumeMode::Player;
    settings.outputs[2].airplay.channels = AirPlayChannels::Left;
    auto server = serverFor(settings);
    server->submit(group(zoneB, {zoneC}));
    server->waitIdle();
    ASSERT_TRUE(waitFor([&] { return in(server->snapshot(), zoneB).playback.receivers.size() == 2; }));
    EXPECT_EQ(server->snapshot().selectedZone, zoneA);
    Command level;
    level.kind = CommandKind::SetReceiverVolume;
    level.zone = zoneB;
    level.receiver = "C30000000003";
    level.playerVolume = 30;
    server->submit(level);
    server->waitIdle();
    const auto receiver = [&](const std::string& zone, const std::string& id) {
        const auto receivers = in(server->snapshot(), zone).playback.receivers;
        const auto found = std::ranges::find(receivers, id, &ReceiverState::id);
        return found == receivers.end() ? ReceiverState{} : *found;
    };
    EXPECT_TRUE(waitFor([&] { return receiver(zoneB, "C30000000003").level == 30U; }));
    EXPECT_EQ(receiver(zoneB, "C30000000003").channels, AirPlayChannels::Left);
    EXPECT_EQ(receiver(zoneB, "B20000000002").channels, AirPlayChannels::Stereo);
    server->submit(group(zoneB, {}));
    server->waitIdle();
    EXPECT_TRUE(waitFor([&] { return receiver(zoneC, "C30000000003").level == 30U; }));
    EXPECT_EQ(receiver(zoneC, "C30000000003").channels, AirPlayChannels::Left);
    server->shutdown();
}

// Zones group only with zones of their kind, and zoneA zone playing apart is not taken; nothing changes.
TEST_F(AirPlayZonesTest, AZoneOfAnotherKindOrPlayingApartIsNotGrouped) {
    auto server = serverFor(threeAlone());
    server->submit(group(zoneA, {zoneId(Backend::Wasapi, "speakers")}));
    server->waitIdle();
    ASSERT_TRUE(server->snapshot().commandError);
    EXPECT_NE(server->snapshot().commandError->detail.find("kinds"), std::string::npos);
    server->submit(playIn(zoneB, testutil::generatedFile({"airplay-apart", 44100, 16, 2, 30.0, true, -30.0})));
    server->waitIdle();
    ASSERT_TRUE(waitFor([&] { return playing(in(server->snapshot(), zoneB)); }));
    server->submit(group(zoneA, {zoneB, zoneC}));
    server->waitIdle();
    const auto s = server->snapshot();
    ASSERT_TRUE(s.commandError);
    EXPECT_EQ(s.commandError->code, audio::ErrorCode::DeviceInUse);
    EXPECT_EQ(s.commandError->detail, "in use by Receiver B");
    EXPECT_EQ(s.zones.size(), 3U);
    EXPECT_LT(nativeSettings(s.settings).airplayGroup.size(), 2U);
    server->shutdown();
}

// A queue moved onto zoneA group plays on all of its receivers, and moved from it plays on one alone, the group stopped.
TEST_F(AirPlayZonesTest, AQueueMovesOntoAGroupAndFromIt) {
    auto server = serverFor(threeAlone());
    server->submit(group(zoneA, {zoneC}));
    server->submit(playIn(zoneB, testutil::generatedFile({"airplay-transferred", 44100, 16, 2, 30.0, true, -3.0})));
    server->waitIdle();
    ASSERT_TRUE(waitFor([&] { return playing(in(server->snapshot(), zoneB)); }));
    Command onto;
    onto.kind = CommandKind::Transfer;
    onto.zone = zoneB;
    onto.target = zoneA;
    server->submit(onto);
    server->waitIdle();
    ASSERT_TRUE(waitFor([&] {
        const auto now = server->snapshot();
        return playing(in(now, zoneA)) && in(now, zoneB).queue.entries.empty() && rtsp_[2].packetCount() > 50;
    }));
    EXPECT_GT(peak(2), 0.5);
    Command from;
    from.kind = CommandKind::Transfer;
    from.zone = zoneA;
    from.target = zoneB;
    server->submit(from);
    server->waitIdle();
    ASSERT_TRUE(waitFor([&] {
        const auto now = server->snapshot();
        return playing(in(now, zoneB)) && in(now, zoneA).queue.entries.empty() &&
               in(now, zoneA).playback.state == PlaybackState::Stopped;
    }));
    const size_t cPackets = rtsp_[2].packetCount();
    std::this_thread::sleep_for(500ms);
    EXPECT_LT(rtsp_[2].packetCount(), cPackets + 10);
    server->shutdown();
}

// A Control point on another computer groups zones, and one that followed zoneA zone grouped into another follows the
// group.
TEST_F(AirPlayZonesTest, AControlPointFollowsTheGroupItsZoneJoined) {
    auto server = serverFor(threeAlone());
    const auto dir = testutil::tempDir() /
                     ("airplay-group-point-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    std::filesystem::create_directories(dir);
    ApiServer::Options options;
    options.port = 0;
    options.portsToTry = 1;
    options.loopbackOnly = true;
    options.serverId = "test-server";
    options.serverName = "Test PC";
    options.pairings = dir / "pairings.json";
    ApiServer api(*server, options);
    RemoteServer::Options remote;
    remote.host = "127.0.0.1";
    remote.port = api.port();
    remote.cache = dir / "cache";
    remote.token = RemoteServer::pair("127.0.0.1", api.port(), "Control 1", "").token;
    remote.ownSettings = dir / "Control 1.json";
    RemoteServer point(remote);
    auto settings = nativeSettings(point.snapshot().settings);
    selectOutput(settings, Backend::AirPlay, "B20000000002", point.snapshot().devices);
    Command choose;
    choose.kind = CommandKind::ApplySettings;
    choose.settings = settingsDto(settings);
    point.submit(choose);
    point.waitIdle();
    ASSERT_TRUE(waitFor([&] { return point.snapshot().selectedZone == zoneB; }));
    point.submit(group(zoneA, {zoneB}));
    point.waitIdle();
    EXPECT_TRUE(waitFor([&] {
        const auto now = point.snapshot();
        return now.selectedZone == zoneA && in(now, zoneA).name == "Receiver A + 1";
    }));
    EXPECT_EQ(server->snapshot().zones.size(), 2U);
    point.submit(group(zoneA, {}));
    point.waitIdle();
    EXPECT_TRUE(waitFor([&] { return server->snapshot().zones.size() == 3U; }));
    EXPECT_EQ(point.snapshot().selectedZone, zoneA);
    server->shutdown();
}

// A player back under a new UDN is known by its MAC address and keeps its zone, its queue and its settings (MZ-R14);
// another of its name and model with another MAC is not taken for it.
TEST(NetworkZonesTest, APlayerBackUnderANewUdnWithItsMacKeepsItsZone) {
    FakeRenderer inUse(RendererKind::UpnpAv, "uuid:in-use", "Other Player"), first(RendererKind::UpnpAv, "uuid:first", "DMP-A6"),
        lookalike(RendererKind::UpnpAv, "uuid:lookalike", "DMP-A6"), renewed(RendererKind::UpnpAv, "uuid:renewed", "DMP-A6");
    lookalike.goQuiet();
    renewed.goQuiet();
    Settings settings;
    settings.autoplay = false;
    settings.backend = Backend::Network;
    settings.network.udn = "uuid:in-use";
    settings.outputs = {enabled(Backend::Network, "uuid:in-use"), enabled(Backend::Network, "uuid:first")};
    settings.outputs[1].zone.crossfade = {true, 6.0};
    settings = fromJson(toJson(settings));
    LocalServer server(settings);
    FinderOptions options;
    options.searchHost = "127.0.0.1";
    options.searchPort = inUse.ssdpPort();
    options.moreSearchPorts = {first.ssdpPort(), lookalike.ssdpPort(), renewed.ssdpPort()};
    options.notifyPort = 0;
    options.searchInterval = 100ms;
    // The renewed player has the first one's MAC, the lookalike one of its own.
    options.macOf = [&](const std::string& location) {
        if (location == first.location() || location == renewed.location()) return std::string("aa:bb:cc:00:00:01");
        return location == lookalike.location() ? std::string("aa:bb:cc:00:00:02") : std::string();
    };
    server.setFinderOptions(options);
    const auto zoneOf = [](const std::string& udn) { return zoneId(Backend::Network, udn); };
    ASSERT_TRUE(waitFor([&] {
        server.submit(refresh());
        server.waitIdle();
        return server.snapshot().devices.network.size() == 2;
    }));
    server.submit(playIn(zoneOf("uuid:first"), testutil::generatedFile({"renewed-player", 44100, 16, 2, 20.0})));
    server.waitIdle();
    Command pause;
    pause.kind = CommandKind::Pause;
    pause.zone = zoneOf("uuid:first");
    server.submit(pause);
    server.waitIdle();
    const auto queue = in(server.snapshot(), zoneOf("uuid:first")).queue.entries;
    ASSERT_FALSE(queue.empty());

    first.disappear();
    lookalike.answerAgain();
    ASSERT_TRUE(waitFor(
        [&] {
            server.submit(refresh());
            server.waitIdle();
            const auto& found = server.snapshot().devices.network;
            return std::ranges::any_of(found, [](const DeviceInfo& d) { return d.id == "uuid:lookalike"; }) &&
                   std::ranges::none_of(found, [](const DeviceInfo& d) { return d.id == "uuid:first"; });
        },
        60s));
    auto s = server.snapshot();
    EXPECT_TRUE(std::ranges::any_of(s.zones, [&](const ZoneSnapshot& z) { return z.id == zoneOf("uuid:first"); }))
        << "the lookalike is not taken for it";
    EXPECT_TRUE(std::ranges::none_of(s.zones, [&](const ZoneSnapshot& z) { return z.id == zoneOf("uuid:lookalike"); }));

    renewed.answerAgain();
    ASSERT_TRUE(waitFor(
        [&] {
            server.submit(refresh());
            server.waitIdle();
            const auto now = server.snapshot();
            return std::ranges::any_of(now.zones, [&](const ZoneSnapshot& z) { return z.id == zoneOf("uuid:renewed"); });
        },
        30s));
    s = server.snapshot();
    EXPECT_EQ(in(s, zoneOf("uuid:renewed")).queue.entries, queue);
    EXPECT_TRUE(std::ranges::none_of(s.zones, [&](const ZoneSnapshot& z) { return z.id == zoneOf("uuid:first"); }));
    const auto saved = std::ranges::find(s.settings.outputs, std::string("uuid:renewed"), &SavedOutput::id);
    ASSERT_NE(saved, s.settings.outputs.end());
    EXPECT_EQ(saved->mac, "aa:bb:cc:00:00:01");
    EXPECT_EQ(saved->zone.crossfade, (CrossfadeSettings{true, 6.0}));
    server.shutdown();
}
