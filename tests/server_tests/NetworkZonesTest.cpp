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
    // At about the speed they play, so no track ends before the test does.
    for (auto* renderer : {&one, &two, &three}) renderer->limitFetch(uint64_t{44100} * 4 * 2);
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
