// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

#include "Common/AudioTypes.h"
#include "FakeHQPlayer.h"
#include "Server/Network/HQPlayer.h"
#include "Server/Playback/Outputs.h"

using namespace camel;
using namespace camel::server;
using namespace std::chrono_literals;

namespace {

hqplayer::ClientOptions quick() {
    hqplayer::ClientOptions options;
    options.connectTimeout = 1000ms;
    options.firstReply = 3000ms;
    options.reply = 1000ms;
    return options;
}

template <class Condition> bool eventually(Condition condition, std::chrono::milliseconds limit = 5000ms) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
        if (condition()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return condition();
}

std::vector<std::string> from(std::vector<std::string> all, size_t first) {
    return {all.begin() + std::ptrdiff_t(std::min(first, all.size())), all.end()};
}

} // namespace

TEST(HQPlayerTest, DocumentsAreTakenWholeWhetherSplitOrJoined) {
    std::string buffer = R"(<?xml version="1.0" encoding="utf-8"?><Play result="OK"/>)"
                         "\n"
                         R"(<?xml version="1.0"?><PlaylistGet album="0"><PlaylistItem index="1" uri="a&gt;b"/></Playl)";
    auto documents = hqplayer::takeDocuments(buffer);
    ASSERT_EQ(documents.size(), 1U);
    EXPECT_EQ(documents[0], R"(<?xml version="1.0" encoding="utf-8"?><Play result="OK"/>)");
    buffer += R"(istGet><?xml version="1.0"?><Stop result="OK"/><?xml)";
    documents = hqplayer::takeDocuments(buffer);
    ASSERT_EQ(documents.size(), 2U);
    EXPECT_TRUE(documents[0].ends_with("</PlaylistGet>"));
    EXPECT_TRUE(documents[1].ends_with(R"(<Stop result="OK"/>)"));
    EXPECT_EQ(buffer, "<?xml");
    // A quoted '>' or "/>" does not end an element.
    buffer = R"(<?xml version="1.0"?><Status song="a/>b" state="2"/>)";
    documents = hqplayer::takeDocuments(buffer);
    ASSERT_EQ(documents.size(), 1U);
    EXPECT_TRUE(documents[0].ends_with(R"(state="2"/>)"));
}

TEST(HQPlayerTest, AddressesKeepTheirHostAndPort) {
    std::string host;
    uint16_t port = 0;
    EXPECT_TRUE(hqplayer::splitAddress("localhost:4321", host, port));
    EXPECT_EQ(host, "localhost");
    EXPECT_EQ(port, 4321);
    EXPECT_TRUE(hqplayer::splitAddress("[fe80::1]:4000", host, port));
    EXPECT_EQ(host, "fe80::1");
    EXPECT_EQ(hqplayer::addressOf("fe80::1", 4000), "[fe80::1]:4000");
    EXPECT_FALSE(hqplayer::splitAddress("host:0", host, port));
    EXPECT_FALSE(hqplayer::splitAddress("host:70000", host, port));
    EXPECT_FALSE(hqplayer::splitAddress(":4321", host, port));
    EXPECT_FALSE(hqplayer::splitAddress("host", host, port));
}

TEST(HQPlayerTest, ASlowFirstReplyIsWaitedForAndEveryConnectionStartsClean) {
    testutil::FakeHQPlayer fake;
    fake.firstReplyDelay = 1500ms;
    hqplayer::Client client(fake.address(), {}, {}, quick());
    const auto started = std::chrono::steady_clock::now();
    client.connect();
    EXPECT_GE(std::chrono::steady_clock::now() - started, 1400ms);
    EXPECT_TRUE(client.connected());
    // A restarted HQPlayer plays on from its last playlist: it is stopped and cleared before anything else.
    EXPECT_EQ(fake.commands(), (std::vector<std::string>{"GetInfo", "Stop", "PlaylistClear", "Status"}));
    EXPECT_NE(fake.raw()[3].find(R"(subscribe="1")"), std::string::npos);
    EXPECT_EQ(client.connections(), 1U);
}

TEST(HQPlayerTest, RepliesSplitAcrossReadsOrSeveralInOneReadAreMatched) {
    testutil::FakeHQPlayer fake;
    hqplayer::Client client(fake.address(), {}, {}, quick());
    client.connect();
    fake.splitReplies = true;
    EXPECT_TRUE(client.ok("<Play/>"));
    fake.splitReplies = false;
    fake.joinReplies = true;
    EXPECT_TRUE(client.ok("<Pause/>"));
    const auto info = client.send("<GetInfo/>");
    EXPECT_EQ(info.name, "GetInfo");
    EXPECT_EQ(info.attributes.at("engine"), "6.3.3");
}

TEST(HQPlayerTest, AStatusPushedBeforeAReplyIsFollowedAndNotTakenForIt) {
    testutil::FakeHQPlayer fake;
    std::atomic<int> pushes = 0;
    std::atomic<int> state = -1;
    hqplayer::Client client(
        fake.address(),
        [&](const hqplayer::Status& s) {
            ++pushes;
            state = s.state;
        },
        {}, quick());
    client.connect();
    const int before = pushes;
    fake.onCommand([](const std::string& name, const upnp::XmlNode&, testutil::FakeHQPlayer::State& s) {
        if (name == "Play") s.state = 2;
    });
    fake.pushBeforeReply = true;
    const auto reply = client.send("<Play/>");
    EXPECT_EQ(reply.name, "Play");
    EXPECT_EQ(reply.attributes.at("result"), "OK");
    EXPECT_TRUE(eventually([&] { return pushes > before && state == 2; }));
    ASSERT_TRUE(client.status());
    EXPECT_EQ(client.status()->state, 2);
    // Pushes come without any command too, and are followed.
    fake.pushBeforeReply = false;
    fake.change([](auto& s) { s.state = 1; });
    EXPECT_TRUE(eventually([&] { return state == 1; }));
}

TEST(HQPlayerTest, AMissingReplyConnectsAgainAndSendsItOnceMore) {
    testutil::FakeHQPlayer fake;
    hqplayer::Client client(fake.address(), {}, {}, quick());
    client.connect();
    fake.dropNext("Pause");
    const size_t before = fake.commands().size();
    EXPECT_TRUE(client.ok("<Pause/>"));
    EXPECT_EQ(client.connections(), 2U);
    EXPECT_EQ(fake.accepted(), 2U);
    EXPECT_EQ(from(fake.commands(), before),
              (std::vector<std::string>{"Pause", "GetInfo", "Stop", "PlaylistClear", "Status", "Pause"}));
}

TEST(HQPlayerTest, AConnectionNobodyTakesSaysHQPlayerIsNotRunning) {
    uint16_t port = 0;
    {
        testutil::FakeHQPlayer gone;
        port = gone.port();
    }
    hqplayer::Client client(hqplayer::addressOf("localhost", port), {}, {}, quick());
    try {
        client.connect();
        FAIL() << "connected to nothing";
    } catch (const audio::Error& e) {
        EXPECT_EQ(e.code(), audio::ErrorCode::DeviceNotFound);
    }
    EXPECT_FALSE(client.connected());
}

TEST(HQPlayerTest, AnHQPlayerThatNeverAnswersIsGivenUpAfterTheFirstReplyLimit) {
    testutil::FakeHQPlayer fake;
    fake.dropNext("GetInfo");
    auto options = quick();
    options.firstReply = 500ms;
    hqplayer::Client client(fake.address(), {}, {}, options);
    EXPECT_THROW(client.connect(), audio::Error);
    EXPECT_FALSE(client.connected());
    client.connect(); // it answers the next connection
    EXPECT_TRUE(client.connected());
}

TEST(HQPlayerTest, AnHQPlayerThatDisappearsIsReportedLost) {
    testutil::FakeHQPlayer fake;
    std::atomic<int> lost = 0;
    hqplayer::Client client(fake.address(), {}, [&] { ++lost; }, quick());
    client.connect();
    fake.dropConnections();
    EXPECT_TRUE(eventually([&] { return lost == 1; }));
    EXPECT_FALSE(client.connected());
    // Closing it on purpose is not news.
    client.connect();
    client.disconnect();
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(lost, 1);
}

TEST(HQPlayerTest, TheDiscoveryAnswerNamesTheHQPlayer) {
    const auto found =
        hqplayer::parseDiscoverReply(R"(<?xml version="1.0" encoding="utf-8"?><discover name="STUDIO" result="OK" )"
                                     R"(version="Signalyst HQPlayer Embedded 5">hqplayer</discover>)",
                                     "192.168.0.71", 4321);
    ASSERT_TRUE(found);
    EXPECT_EQ(found->address, "192.168.0.71:4321");
    EXPECT_EQ(found->name, "STUDIO");
    EXPECT_EQ(found->version, "Signalyst HQPlayer Embedded 5");
    EXPECT_FALSE(hqplayer::parseDiscoverReply(R"(<discover result="NA">other</discover>)", "192.168.0.71", 4321));
    EXPECT_FALSE(hqplayer::parseDiscoverReply("garbage", "192.168.0.71", 4321));
    // This computer's own is localhost, the address an HQPlayer added for it has.
    EXPECT_EQ(hqplayer::parseDiscoverReply(R"(<discover result="OK">hqplayer</discover>)", "127.0.0.1", 4321)->address,
              "localhost:4321");
}

TEST(HQPlayerTest, OneFoundIsListedAndOneGoneIsNot) {
    testutil::FakeHQPlayer fake;
    hqplayer::FinderOptions options;
    options.group = "127.0.0.1";
    options.port = fake.port();
    options.searchInterval = 100ms;
    options.forgetAfter = 400ms;
    options.localPort = 0;
    std::atomic<int> changes = 0;
    hqplayer::Finder finder(options, [&] { ++changes; });
    ASSERT_TRUE(eventually([&] { return finder.found().size() == 1; }));
    EXPECT_EQ(finder.found()[0], (hqplayer::Found{fake.address(), "FAKE-HQP", "Signalyst HQPlayer Desktop 6"}));
    fake.answerDiscovery = false;
    EXPECT_TRUE(eventually([&] { return finder.found().empty(); }));
    EXPECT_GE(changes, 2);
}

TEST(HQPlayerTest, OneAddedIsListedWhileItListensThoughItAnswersNoSearch) {
    hqplayer::FinderOptions options;
    options.group = "127.0.0.1";
    options.searchInterval = 100ms;
    options.probeTimeout = 300ms;
    options.localPort = 0;
    std::string address;
    {
        testutil::FakeHQPlayer fake;
        fake.answerDiscovery = false;
        address = fake.address();
        options.port = uint16_t(fake.port() + 1); // nobody answers the search
        hqplayer::Finder finder(options, {});
        finder.watch({address});
        ASSERT_TRUE(eventually([&] { return finder.found().size() == 1; }));
        EXPECT_EQ(finder.found()[0].address, address);
        EXPECT_TRUE(finder.found()[0].name.empty());
    }
    hqplayer::Finder finder(options, {});
    finder.watch({address});
    std::this_thread::sleep_for(500ms);
    EXPECT_TRUE(finder.found().empty());
}

TEST(HQPlayerTest, ThisComputersOwnIsListedWithoutBeingAddedOrAnsweringSearches) {
    testutil::FakeHQPlayer fake;
    fake.answerDiscovery = false; // "Allow control from network" off
    hqplayer::FinderOptions options;
    options.group = "127.0.0.1";
    options.port = uint16_t(fake.port() + 1);
    options.searchInterval = 100ms;
    options.probeTimeout = 300ms;
    options.localPort = fake.port();
    hqplayer::Finder finder(options, {});
    ASSERT_TRUE(eventually([&] { return finder.found().size() == 1; }));
    EXPECT_EQ(finder.found()[0].address, fake.address());
    // Added as well, it is still one.
    finder.watch({fake.address()});
    finder.search();
    std::this_thread::sleep_for(400ms);
    EXPECT_EQ(finder.found().size(), 1U);
    finder.watch({});
    std::this_thread::sleep_for(400ms);
    EXPECT_EQ(finder.found().size(), 1U);
}

TEST(HQPlayerTest, OneFoundAtAnAddressAlreadyAddedIsOneEntry) {
    Settings s;
    DeviceCatalog devices;
    devices.revision = 1;
    EXPECT_TRUE(addHQPlayer(s, "localhost:4321", "localhost", devices));
    EXPECT_FALSE(addHQPlayer(s, "localhost:4321", "localhost", devices));
    devices.hqplayer = {{"localhost:4321", "STUDIO"}, {"192.168.0.71:4321", "DEN"}};
    syncOutputs(s, devices);
    const auto rows = outputRows(s, devices);
    std::vector<OutputRow> hqplayers;
    std::ranges::copy_if(rows, std::back_inserter(hqplayers), [](const OutputRow& r) { return r.backend == Backend::HQPlayer; });
    ASSERT_EQ(hqplayers.size(), 2U);
    EXPECT_EQ(hqplayers[0].id, "localhost:4321");
    EXPECT_EQ(hqplayers[0].name, "STUDIO");
    EXPECT_TRUE(hqplayers[0].enabled);
    EXPECT_TRUE(hqplayers[0].connected);
    EXPECT_EQ(hqplayers[1].id, "192.168.0.71:4321");
    EXPECT_FALSE(hqplayers[1].enabled);
    // Gone from the network, the one added stays, offline; the one only found goes.
    devices.hqplayer.clear();
    ++devices.revision;
    syncOutputs(s, devices);
    hqplayers.clear();
    std::ranges::copy_if(outputRows(s, devices), std::back_inserter(hqplayers),
                         [](const OutputRow& r) { return r.backend == Backend::HQPlayer; });
    ASSERT_EQ(hqplayers.size(), 1U);
    EXPECT_FALSE(hqplayers[0].connected);
    EXPECT_EQ(hqplayers[0].name, "STUDIO");
}

TEST(HQPlayerTest, AnAddedHQPlayerSurvivesTheSettingsFile) {
    Settings s;
    DeviceCatalog devices;
    devices.revision = 1;
    addHQPlayer(s, "192.168.0.71:4321", "192.168.0.71", devices);
    selectOutput(s, Backend::HQPlayer, "192.168.0.71:4321", devices);
    const Settings read = fromJson(toJson(s));
    EXPECT_EQ(read.backend, Backend::HQPlayer);
    EXPECT_EQ(read.hqplayer.address, "192.168.0.71:4321");
    ASSERT_EQ(read.outputs.size(), s.outputs.size());
    EXPECT_EQ(read.outputs, s.outputs);
}

TEST(HQPlayerTest, ItsDsdParameterAndVolumeModeSurviveTheSettingsFile) {
    Settings s;
    DeviceCatalog devices;
    devices.revision = 1;
    addHQPlayer(s, "192.168.0.71:4321", "192.168.0.71", devices);
    selectOutput(s, Backend::HQPlayer, "192.168.0.71:4321", devices);
    s.hqplayer.dsdOutput = DsdOutput::Pcm;
    s.hqplayer.dsdPcmRate = 352800;
    s.volume.mode = VolumeMode::Player;
    const Settings read = fromJson(toJson(s));
    EXPECT_EQ(read.hqplayer.dsdOutput, DsdOutput::Pcm);
    EXPECT_EQ(read.hqplayer.dsdPcmRate, 352800U);
    EXPECT_EQ(read.volume.mode, VolumeMode::Player);
    EXPECT_EQ(Settings{}.hqplayer.dsdOutput, DsdOutput::Native);
}
