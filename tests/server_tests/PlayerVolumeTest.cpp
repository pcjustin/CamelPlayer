// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <thread>

#include "FakeRenderer.h"
#include "Server/Network/PlayerVolume.h"

using namespace camel::server;
using namespace std::chrono_literals;
using testutil::FakeRenderer;
using testutil::RendererKind;

namespace {

bool waitFor(const std::function<bool()>& check, std::chrono::milliseconds limit = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (check()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return check();
}

upnp::PlayerDescription describe(const FakeRenderer& renderer) {
    auto description = upnp::parseDescription(renderer.descriptionXml(), renderer.location());
    if (!description) throw std::logic_error("the fake's description does not parse");
    return *description;
}

const LinkOptions kFast{50ms, 2, 1800, 1000};
const LinkOptions kEventsOnly{1h, 2, 1800, 1000};

} // namespace

TEST(PlayerVolumeTest, ReadsTheLevelAndMuteAndWritesNothing) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.setVolume(37, true);
    PlayerVolume volume(describe(renderer), kFast);
    ASSERT_TRUE(waitFor([&] { return volume.current().has_value(); }));
    EXPECT_EQ(*volume.current(), (PlayerLevel{37, true}));
    std::this_thread::sleep_for(150ms);
    EXPECT_EQ(std::ranges::count(renderer.actions(), "SetVolume"), 0);
    EXPECT_EQ(std::ranges::count(renderer.actions(), "SetMute"), 0);
}

TEST(PlayerVolumeTest, FollowsTheEventsOfThePlayersRemote) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    PlayerVolume volume(describe(renderer), kEventsOnly);
    ASSERT_TRUE(waitFor([&] { return volume.current().has_value() && renderer.subscribers() == 1; }));
    renderer.changeVolume(71, true);
    EXPECT_TRUE(waitFor([&] { return volume.current() == PlayerLevel{71, true}; }));
}

TEST(PlayerVolumeTest, AnEventOfAnotherChannelMovesNothing) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.setVolume(20, false);
    PlayerVolume volume(describe(renderer), kEventsOnly);
    ASSERT_TRUE(waitFor([&] { return volume.current().has_value() && renderer.subscribers() == 1; }));
    renderer.emitVolume(R"(<Volume channel="LF" val="90"/>)");
    std::this_thread::sleep_for(150ms);
    EXPECT_EQ(volume.current()->level, 20U);
}

TEST(PlayerVolumeTest, ReadingAgainFollowsAPlayerThatSendsNoEvents) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    PlayerVolume volume(describe(renderer), kFast);
    ASSERT_TRUE(waitFor([&] { return volume.current().has_value(); }));
    renderer.setVolume(12, false);
    EXPECT_TRUE(waitFor([&] { return volume.current() == PlayerLevel{12, false}; }));
}

TEST(PlayerVolumeTest, WritesWhatIsAskedOnTheMasterChannel) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    PlayerVolume volume(describe(renderer), kFast);
    ASSERT_TRUE(waitFor([&] { return volume.current().has_value(); }));
    volume.setLevel(25);
    EXPECT_EQ(volume.current()->level, 25U); // at once, before the player answers
    volume.setMuted(true);
    EXPECT_TRUE(waitFor([&] { return std::ranges::count(renderer.actions(), "SetMute") == 1; }));
    EXPECT_EQ(std::ranges::count(renderer.actions(), "SetVolume"), 1);
}

TEST(PlayerVolumeTest, TheSameLevelIsNotWrittenAgain) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.setVolume(30, false);
    PlayerVolume volume(describe(renderer), kFast);
    ASSERT_TRUE(waitFor([&] { return volume.current().has_value(); }));
    volume.setLevel(30);
    volume.setMuted(false);
    std::this_thread::sleep_for(150ms);
    EXPECT_EQ(std::ranges::count(renderer.actions(), "SetVolume"), 0);
    EXPECT_EQ(std::ranges::count(renderer.actions(), "SetMute"), 0);
}

TEST(PlayerVolumeTest, AnEventFromBeforeAWriteDoesNotMoveTheLevelBack) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    PlayerVolume volume(describe(renderer), kEventsOnly);
    ASSERT_TRUE(waitFor([&] { return volume.current().has_value() && renderer.subscribers() == 1; }));
    volume.setLevel(60);
    ASSERT_TRUE(waitFor([&] { return std::ranges::count(renderer.actions(), "SetVolume") == 1; }));
    renderer.emitVolume(R"(<Volume channel="Master" val="55"/>)");
    std::this_thread::sleep_for(150ms);
    EXPECT_EQ(volume.current()->level, 60U);
}

TEST(PlayerVolumeTest, APlayerThatStopsAnsweringHasNoLevel) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    PlayerVolume volume(describe(renderer), kFast);
    ASSERT_TRUE(waitFor([&] { return volume.current().has_value(); }));
    renderer.disappear();
    EXPECT_TRUE(waitFor([&] { return !volume.current(); }));
}

TEST(PlayerVolumeTest, ARefusedReadLeavesNoLevelAndNothingToWrite) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.refuseSoap(true);
    PlayerVolume volume(describe(renderer), kFast);
    std::this_thread::sleep_for(200ms);
    EXPECT_FALSE(volume.current());
    volume.setLevel(80);
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(std::ranges::count(renderer.actions(), "SetVolume"), 0);
}

TEST(PlayerVolumeTest, EndingLeavesNoSubscription) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    {
        PlayerVolume volume(describe(renderer), kFast);
        ASSERT_TRUE(waitFor([&] { return renderer.subscribers() == 1; }));
    }
    EXPECT_EQ(renderer.subscribers(), 0U);
}
