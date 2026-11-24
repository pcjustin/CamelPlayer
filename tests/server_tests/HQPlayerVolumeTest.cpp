// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <thread>

#include "FakeHQPlayer.h"
#include "Server/Network/HQPlayerVolume.h"

using namespace camel::server;
using namespace std::chrono_literals;

namespace {

template <class Condition> bool eventually(Condition condition, std::chrono::milliseconds limit = 5000ms) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
        if (condition()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return condition();
}

HQPlayerVolumeOptions quick() {
    HQPlayerVolumeOptions options;
    options.client.connectTimeout = 1000ms;
    options.client.firstReply = 2000ms;
    options.client.reply = 2000ms;
    options.rangeEvery = 300ms;
    options.retryAfter = 300ms;
    options.settling = 300ms;
    return options;
}

// The fake pushes a Status every 100 ms once something plays it; a volume test plays nothing, so it is told to.
void enable(testutil::FakeHQPlayer& fake, double volume = -30.0) {
    fake.plays = true;
    fake.change([&](testutil::FakeHQPlayer::State& s) {
        s.volumeEnabled = true;
        s.volumeMin = -60.0;
        s.volumeMax = 0.0;
        s.volume = volume;
    });
}

} // namespace

TEST(HQPlayerVolumeTest, AVolumeHQPlayerHasNotEnabledIsNotOffered) {
    testutil::FakeHQPlayer fake;
    fake.plays = true;
    HQPlayerVolume volume(fake.address(), quick());
    ASSERT_TRUE(eventually([&] { return std::ranges::count(fake.commands(), "VolumeRange") >= 1; }));
    std::this_thread::sleep_for(300ms);
    EXPECT_FALSE(volume.current());
    volume.setLevel(50);
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(std::ranges::count(fake.commands(), "Volume"), 0);
}

TEST(HQPlayerVolumeTest, TheLevelSpansTheRangeAndFollowsHQPlayer) {
    testutil::FakeHQPlayer fake;
    enable(fake);
    HQPlayerVolume volume(fake.address(), quick());
    ASSERT_TRUE(eventually([&] { return volume.current().has_value(); }));
    EXPECT_EQ(volume.current()->level, 50U);
    EXPECT_EQ(volume.current()->db, -30.0);
    EXPECT_FALSE(volume.current()->muted);
    fake.change([](testutil::FakeHQPlayer::State& s) { s.volume = -12.0; }); // from HQPlayer's window
    EXPECT_TRUE(eventually([&] { return volume.current()->level == 80U; }));
}

TEST(HQPlayerVolumeTest, ALevelSetIsSentInDecibels) {
    testutil::FakeHQPlayer fake;
    enable(fake);
    HQPlayerVolume volume(fake.address(), quick());
    ASSERT_TRUE(eventually([&] { return volume.current().has_value(); }));
    volume.setLevel(25);
    EXPECT_TRUE(eventually([&] { return fake.state().volume == -45.0; }));
    EXPECT_EQ(volume.current()->level, 25U);
    std::this_thread::sleep_for(600ms); // past settling, the level stays as set
    EXPECT_EQ(volume.current()->level, 25U);
}

TEST(HQPlayerVolumeTest, MutingKeepsTheLevelThatUnmutingSetsAgain) {
    testutil::FakeHQPlayer fake;
    enable(fake);
    HQPlayerVolume volume(fake.address(), quick());
    ASSERT_TRUE(eventually([&] { return volume.current().has_value(); }));
    volume.setMuted(true);
    EXPECT_TRUE(eventually([&] { return fake.state().volume == -60.0; }));
    std::this_thread::sleep_for(600ms);
    ASSERT_TRUE(volume.current());
    EXPECT_TRUE(volume.current()->muted);
    EXPECT_EQ(volume.current()->level, 50U);
    volume.setMuted(false);
    EXPECT_TRUE(eventually([&] { return fake.state().volume == -30.0; }));
    EXPECT_FALSE(volume.current()->muted);
}

// A Status HQPlayer pushed before it took the mute, which arrives before the mute's reply, is not taken for a change made
// in HQPlayer's window: the level stays muted at the level unmuting sets again (OP-R64).
TEST(HQPlayerVolumeTest, AStatusFromBeforeTheMuteLeavesItMuted) {
    testutil::FakeHQPlayer fake;
    enable(fake);
    HQPlayerVolume volume(fake.address(), quick());
    ASSERT_TRUE(eventually([&] { return volume.current().has_value(); }));
    fake.replyTo("Volume", [](const auto& element) {
        return std::string(R"(<?xml version="1.0" encoding="utf-8"?><Status active_bits="0" active_channels="2" )"
                           R"(active_filter="0" active_mode="0" active_rate="0" active_shaper="0" length="0" position="0" )"
                           R"(state="0" track="0" track_serial="0" transport_serial="0" volume="-30"/>)") +
               "\n" + std::format(R"(<?xml version="1.0" encoding="utf-8"?><{} result="OK"/>)", element.name);
    });
    volume.setMuted(true);
    EXPECT_TRUE(eventually([&] { return fake.state().volume == -60.0; }));
    std::this_thread::sleep_for(600ms);
    ASSERT_TRUE(volume.current());
    EXPECT_TRUE(volume.current()->muted);
    EXPECT_EQ(volume.current()->level, 50U);
    volume.setMuted(false);
    EXPECT_TRUE(eventually([&] { return fake.state().volume == -30.0; }));
}

TEST(HQPlayerVolumeTest, AVolumeTurnedOnInHQPlayerLaterIsOffered) {
    testutil::FakeHQPlayer fake;
    fake.plays = true;
    HQPlayerVolume volume(fake.address(), quick());
    ASSERT_TRUE(eventually([&] { return std::ranges::count(fake.commands(), "VolumeRange") >= 1; }));
    EXPECT_FALSE(volume.current());
    enable(fake, -6.0);
    EXPECT_TRUE(eventually([&] { return volume.current() && volume.current()->level == 90U; }));
}

TEST(HQPlayerVolumeTest, ItsConnectionLeavesThePlaylistAlone) {
    testutil::FakeHQPlayer fake;
    enable(fake);
    HQPlayerVolume volume(fake.address(), quick());
    ASSERT_TRUE(eventually([&] { return volume.current().has_value(); }));
    const auto commands = fake.commands();
    EXPECT_EQ(std::ranges::count(commands, "Stop"), 0);
    EXPECT_EQ(std::ranges::count(commands, "PlaylistClear"), 0);
}
