// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <thread>

#include "FakeAirPlayReceiver.h"
#include "Server/Network/AirPlayFinder.h"
#include "Server/Network/AirPlayPairings.h"
#include "Server/Network/Rtsp.h"
#include "Server/Playback/LocalServer.h"
#include "Server/Playback/Outputs.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::server;
using namespace camel::server::airplay;
using namespace std::chrono_literals;
using testutil::FakeMdns;
using testutil::FakeReceiverRecord;
using testutil::FakeRtsp;

namespace {

bool waitFor(const std::function<bool()>& check, std::chrono::milliseconds limit = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + limit * testutil::kPairingSlowdown;
    while (std::chrono::steady_clock::now() < deadline) {
        if (check()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return check();
}

AirPlayFinderOptions optionsFor(const FakeMdns& mdns) {
    AirPlayFinderOptions options;
    options.queryHost = "127.0.0.1";
    options.queryPort = mdns.port();
    options.groupPort = 0;
    options.searchInterval = 100ms;
    options.retryInterval = 50ms;
    options.sessionTimeoutMs = 1000 * testutil::kPairingSlowdown;
    return options;
}

class AirPlayFinderTest : public ::testing::Test {
protected:
    // Pairing keeps the finder from reading answers for a moment, longer than the usual interval lets them go stale.
    void start(std::chrono::milliseconds interval = 100ms) {
        auto options = optionsFor(mdns_);
        options.searchInterval = interval;
        finder_ = std::make_unique<AirPlayFinder>(options, nullptr);
    }
    std::vector<Receiver> receivers() const { return finder_->receivers(); }
    bool found(size_t count) {
        return waitFor([&] { return receivers().size() == count; });
    }
    Support supportOf(size_t index = 0) const { return receivers().at(index).support; }

    FakeRtsp rtsp_;
    FakeMdns mdns_;
    std::unique_ptr<AirPlayFinder> finder_;
};

FakeReceiverRecord record(const FakeRtsp& rtsp, std::string deviceId = "800A805D016B", std::string name = "DMP-A6") {
    FakeReceiverRecord r;
    r.deviceId = std::move(deviceId);
    r.name = std::move(name);
    r.host = "receiver-" + r.deviceId;
    r.port = rtsp.port();
    return r;
}

} // namespace

TEST(AirPlayTest, QuestionsAskForAnAnswerToTheAskersPort) {
    const auto message = queryMessage({{serviceName("_raop._tcp"), DnsType::Ptr}});
    const std::string encoded = std::string("\x05_raop\x04_tcp\x05local", 17);
    EXPECT_EQ(message[5], 1);
    EXPECT_NE(message.find(encoded + '\0'), std::string::npos);
    EXPECT_EQ(message.substr(message.size() - 4), std::string("\x00\x0C\x80\x01", 4));
}

TEST(AirPlayTest, AGroupQuestionAsksForAnAnswerToTheGroup) {
    const auto message = queryMessage({{serviceName("_raop._tcp"), DnsType::Ptr}}, false);
    EXPECT_EQ(message.substr(message.size() - 4), std::string("\x00\x0C\x00\x01", 4));
}

// A receiver that answers only a query from the mDNS port, and only to the group, as the Chord Poly does, is found
// through the group: no answer comes to the finder's own port.
TEST(AirPlayFinderGroup, AReceiverThatAnswersOnlyTheGroupIsFound) {
    // A port no other test or responder uses, found free.
    uint16_t port = 0;
    {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        const SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        ASSERT_EQ(bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
        int length = sizeof(address);
        getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length);
        port = ntohs(address.sin_port);
        closesocket(probe);
        WSACleanup();
    }
    const std::string group = "239.255.53.53";
    FakeRtsp rtsp;
    FakeMdns mdns(group, port);
    mdns.add(record(rtsp, "C0FFEE000001", "Chord Poly"));
    AirPlayFinderOptions options;
    options.queryHost = "127.0.0.1";
    options.queryPort = 9; // discard: nothing answers there
    options.groupHost = group;
    options.groupPort = port;
    options.searchInterval = 100ms;
    options.retryInterval = 50ms;
    options.sessionTimeoutMs = 1000;
    AirPlayFinder finder(options, nullptr);
    ASSERT_TRUE(waitFor([&] { return finder.receivers().size() == 1; }));
    EXPECT_EQ(finder.receivers().front().name, "Chord Poly");
}

TEST(AirPlayTest, CompressedNamesAndDottedLabelsAreRead) {
    // A PTR whose target shares the service's name through a pointer, and an instance label holding a dot.
    std::string message("\x00\x00\x84\x00\x00\x00\x00\x01\x00\x00\x00\x00", 12);
    const std::string owner = std::string("\x05_raop\x04_tcp\x05local\x00", 18);
    const std::string target = std::string("\x08") + "a.b@Name" + std::string("\xC0\x0C", 2);
    message += owner + std::string("\x00\x0C\x00\x01\x00\x00\x00\x78\x00", 9) + char(target.size()) + target;

    const auto records = parseRecords(message);

    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0].type, DnsType::Ptr);
    EXPECT_EQ(records[0].target, (DnsName{"a.b@Name", "_raop", "_tcp", "local"}));
}

TEST(AirPlayTest, DamagedMessagesGiveWhatWasWholeAndNeverReadPastTheEnd) {
    std::string pointsAtItself("\x00\x00\x84\x00\x00\x00\x00\x01\x00\x00\x00\x00\xC0\x0C", 14);
    EXPECT_TRUE(parseRecords(pointsAtItself).empty());
    EXPECT_TRUE(parseRecords("").empty());
    EXPECT_TRUE(parseRecords(std::string("\x00\x00\x84\x00\x00\x00\x00\x05\x00\x00\x00\x00", 12)).empty());
    const std::string query = queryMessage({{serviceName("_raop._tcp"), DnsType::Ptr}});
    EXPECT_TRUE(parseRecords(query).empty());
}

TEST(AirPlayTest, TheRaopNameGivesTheDeviceIdAndName) {
    const auto name = parseRaopName("800a805d016b@DMP-A6");
    ASSERT_TRUE(name);
    EXPECT_EQ(name->first, "800A805D016B");
    EXPECT_EQ(name->second, "DMP-A6");
    EXPECT_FALSE(parseRaopName("no device id"));
    EXPECT_EQ(deviceIdFrom("32:FB:AA:AB:39:9D"), "32FBAAAB399D");
}

TEST(AirPlayTest, TheTxtRecordSaysWhatIsSupported) {
    EXPECT_EQ(supportFrom({{"et", "0,1"}, {"cn", "1"}, {"pw", "false"}, {"sr", "44100"}, {"ss", "16"}, {"ch", "2"}}),
              Support::Supported);
    EXPECT_EQ(supportFrom({{"et", "0,3,5"}, {"cn", "0,1,2,3"}}), Support::Supported);
    EXPECT_EQ(supportFrom({{"et", "1,3"}, {"cn", "1"}}), Support::EncryptedOnly);
    EXPECT_EQ(supportFrom({{"cn", "1"}}), Support::EncryptedOnly);
    EXPECT_EQ(supportFrom({{"et", "0"}, {"cn", "1"}, {"pw", "true"}}), Support::Password);
    EXPECT_EQ(supportFrom({{"et", "0"}, {"cn", "0,2"}}), Support::Format);
    EXPECT_EQ(supportFrom({{"et", "0"}, {"cn", "1"}, {"sr", "48000"}}), Support::Format);
    EXPECT_EQ(supportFrom({{"et", "0"}, {"cn", "1"}, {"ss", "24"}}), Support::Format);
}

TEST(AirPlayTest, TheSessionDescriptionCarriesNoKeys) {
    const auto description = sessionDescription("192.168.0.2", "192.168.0.71", "4242");
    EXPECT_NE(description.find("a=fmtp:96 352 0 16 40 10 14 2 255 0 0 44100\r\n"), std::string::npos);
    EXPECT_NE(description.find("o=CamelPlayer 4242 0 IN IP4 192.168.0.2\r\n"), std::string::npos);
    EXPECT_NE(description.find("c=IN IP4 192.168.0.71\r\n"), std::string::npos);
    EXPECT_EQ(description.find("aeskey"), std::string::npos);
    EXPECT_EQ(description.find("aesiv"), std::string::npos);
}

// Every maker's receivers are listed, whatever model they announce, as the owner's DMP-A6, KECES and Chord Poly.
TEST(AirPlayTest, EveryMakersReceiversAreListed) {
    const auto modelled = [](const char* model) { return TxtRecord{{"am", model}, {"et", "0"}, {"cn", "1"}}; };
    const auto receivers = receiversFrom({{"A", "HomePod", "192.168.0.1", 7000, modelled("AudioAccessory5,1"), {}},
                                          {"B", "Mac", "192.168.0.2", 7000, modelled("Mac14,2"), {}},
                                          {"C", "Apple TV", "192.168.0.3", 7000, modelled("AppleTV11,1"), {}},
                                          {"D", "DMP-A6", "192.168.0.4", 5500, modelled("eversolo,1"), {}},
                                          {"E", "Bravo", "192.168.0.5", 7000, modelled("MUZO Cobblestone"), {}},
                                          {"F", "Unknown", "192.168.0.6", 7000, {{"et", "0"}, {"cn", "1"}}, {}}});
    ASSERT_EQ(receivers.size(), 6U);
    std::vector<std::string> names(receivers.size());
    std::ranges::transform(receivers, names.begin(), &Receiver::name);
    EXPECT_EQ(names, (std::vector<std::string>{"Apple TV", "Bravo", "DMP-A6", "HomePod", "Mac", "Unknown"}));
    for (const auto& receiver : receivers) EXPECT_EQ(receiver.support, Support::Supported) << receiver.name;
}

// The owner's Sony TV announces _airplay._tcp alone, and a Mac takes an AirPlay 1 session it never plays (2026-11-18):
// both play over AirPlay 2. Every other receiver stays on AirPlay 1.
TEST(AirPlayTest, ReceiversWithoutRaopAndMacsTakeAirPlay2) {
    const TxtRecord tv = {{"deviceid", "AA:BB:CC:DD:EE:01"}, {"model", "Y-50S30"}, {"flags", "0x204"}, {"srcvers", "377.40.00"}};
    const TxtRecord macRaop = {{"am", "Mac14,2"}, {"et", "0,3,5"}, {"cn", "0,1,2,3"}, {"vs", "1005.13.5"}, {"sf", "0x4"}};
    const TxtRecord homePod = {{"am", "AudioAccessory5,1"}, {"et", "0,3,5"}, {"cn", "0,1,2,3"}, {"vs", "1005.13.2"}};
    const auto receivers = receiversFrom({{"AABBCCDDEE01", "客廳的電視", "192.168.0.18", 7000, {}, tv},
                                          {"B2C58E1E713E", "MacBook Air", "192.168.0.23", 7000, macRaop, {{"model", "Mac14,2"}}},
                                          {"32FBAAAB399D", "主臥", "192.168.0.220", 7000, homePod, {}}});
    ASSERT_EQ(receivers.size(), 3U);
    EXPECT_EQ(receivers[0].name, "MacBook Air");
    EXPECT_TRUE(receivers[0].airplay2);
    EXPECT_EQ(receivers[0].support, Support::Supported);
    EXPECT_EQ(receivers[1].name, "主臥");
    EXPECT_FALSE(receivers[1].airplay2);
    EXPECT_EQ(receivers[2].name, "客廳的電視");
    EXPECT_TRUE(receivers[2].airplay2);
    EXPECT_EQ(receivers[2].support, Support::Supported);
    EXPECT_EQ(receivers[2].version, "377.40.00");
    EXPECT_TRUE(receivers[2].asksCode);
    // A Mac's refusal names its own setting.
    EXPECT_TRUE(receivers[0].mac);
    EXPECT_FALSE(receivers[1].mac);
    EXPECT_FALSE(receivers[2].mac);
}

// Another maker's stereo pair is one receiver, as Apple's is.
TEST(AirPlayTest, AnotherMakersStereoPairIsOneReceiver) {
    const TxtRecord raop = {{"am", "Some Maker Speaker"}, {"et", "0"}, {"cn", "1"}};
    const auto receivers =
        receiversFrom({{"AA", "Left", "192.168.0.10", 7000, raop, {{"tsid", "PAIR"}, {"gpn", "Pair"}, {"igl", "1"}}},
                       {"BB", "Right", "192.168.0.11", 7000, raop, {{"tsid", "PAIR"}, {"gpn", "Pair"}, {"igl", "0"}}}});
    ASSERT_EQ(receivers.size(), 1U);
    EXPECT_EQ(receivers[0].name, "Pair");
    EXPECT_EQ(receivers[0].address, "192.168.0.10");
    ASSERT_EQ(receivers[0].others.size(), 1U);
}

// The Home app's "Only People Sharing This Home" announces acl=1, "Anyone On the Same Network" and "Everyone" acl=0, as
// the owner's HomePods did on 2026-11-15.
TEST(AirPlayTest, AReceiverForItsHomeOnlyIsNotSupported) {
    const TxtRecord raop = {{"am", "AudioAccessory5,1"}, {"et", "0,3,5"}, {"cn", "0,1,2,3"}};
    const auto receivers = receiversFrom({{"A", "Home", "192.168.0.1", 7000, raop, {{"acl", "1"}}},
                                          {"B", "Network", "192.168.0.2", 7000, raop, {{"acl", "0"}}},
                                          {"C", "Older", "192.168.0.3", 7000, raop, {}}});
    ASSERT_EQ(receivers.size(), 3U);
    EXPECT_EQ(receivers[0].support, Support::HomeOnly);
    EXPECT_STREQ(supportId(receivers[0].support), "home");
    EXPECT_EQ(receivers[1].support, Support::Supported);
    EXPECT_EQ(receivers[2].support, Support::Supported);
    const TxtRecord password = {{"am", "AudioAccessory5,1"}, {"et", "0"}, {"cn", "1"}, {"pw", "true"}};
    EXPECT_EQ(receiversFrom({{"D", "Both", "192.168.0.4", 7000, password, {{"acl", "1"}}}}).front().support, Support::Password);
}

TEST(AirPlayTest, AStereoPairIsOneReceiverReachedThroughItsLeader) {
    const TxtRecord raop = {{"am", "AudioAccessory5,1"}, {"et", "0"}, {"cn", "1"}, {"vs", "1005.13.2"}};
    const TxtRecord follower = {{"tsid", "GROUP"}, {"gpn", "主臥"}, {"igl", "0"}};
    const TxtRecord leader = {{"tsid", "GROUP"}, {"gpn", "主臥"}, {"igl", "1"}};
    const auto receivers = receiversFrom({{"32FBAAAB399D", "主臥", "192.168.0.220", 7000, raop, follower},
                                          {"FAE2708FA125", "主臥 (2)", "192.168.0.222", 7000, raop, leader},
                                          {"800A805D016B", "Other", "192.168.0.71", 5500, raop, {}}});
    ASSERT_EQ(receivers.size(), 2U);
    EXPECT_EQ(receivers[0].id, "800A805D016B");
    EXPECT_EQ(receivers[1].id, "GROUP");
    EXPECT_EQ(receivers[1].name, "主臥");
    EXPECT_EQ(receivers[1].address, "192.168.0.222");
    EXPECT_EQ(receivers[1].others, (std::vector<Speaker>{{"192.168.0.220", 7000}}));
    EXPECT_EQ(receivers[1].speakers, (std::vector<std::string>{"32FBAAAB399D", "FAE2708FA125"}));
    EXPECT_TRUE(receivers[0].others.empty());
    EXPECT_TRUE(receivers[0].speakers.empty());
}

TEST(AirPlayTest, APairMissingAMemberKeepsItsIdentity) {
    const TxtRecord raop = {{"am", "AudioAccessory5,1"}, {"et", "0"}, {"cn", "1"}};
    const auto receivers = receiversFrom(
        {{"32FBAAAB399D", "主臥", "192.168.0.220", 7000, raop, {{"tsid", "GROUP"}, {"gpn", "主臥"}, {"igl", "0"}}}});
    ASSERT_EQ(receivers.size(), 1U);
    EXPECT_EQ(receivers[0].id, "GROUP");
    EXPECT_EQ(receivers[0].address, "192.168.0.220");
    EXPECT_TRUE(receivers[0].others.empty());
    EXPECT_TRUE(receivers[0].speakers.empty());
}

TEST_F(AirPlayFinderTest, FindsAReceiverAndAsksOnlyOptions) {
    mdns_.add(record(rtsp_));
    start();
    ASSERT_TRUE(found(1));
    const auto receiver = receivers().front();
    EXPECT_EQ(receiver.id, "800A805D016B");
    EXPECT_EQ(receiver.name, "DMP-A6");
    EXPECT_EQ(receiver.address, "127.0.0.1");
    EXPECT_EQ(receiver.port, rtsp_.port());
    EXPECT_EQ(receiver.version, "105.1");
    EXPECT_TRUE(waitFor([&] { return !rtsp_.requests().empty(); }));
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(rtsp_.methods(), (std::vector<std::string>{"OPTIONS"}));
    EXPECT_EQ(supportOf(), Support::Supported);
}

TEST_F(AirPlayFinderTest, AsksForWhatAResponderLeftOut) {
    auto terse = record(rtsp_);
    terse.terse = true;
    mdns_.add(terse);
    start();
    EXPECT_TRUE(found(1));
}

TEST_F(AirPlayFinderTest, AReceiverComesBackAtANewAddressAsTheSameOne) {
    mdns_.add(record(rtsp_));
    start();
    ASSERT_TRUE(found(1));
    mdns_.moveTo("800A805D016B", "127.0.0.2");
    EXPECT_TRUE(waitFor([&] { return receivers().size() == 1 && receivers().front().address == "127.0.0.2"; }));
    EXPECT_EQ(receivers().front().id, "800A805D016B");
}

// A receiver on two networks answers from each with its own address: the one in use stays, rather than following each
// answer and being asked again each time.
TEST_F(AirPlayFinderTest, AReceiverWithTwoAddressesKeepsTheOneInUse) {
    auto twoNetworks = record(rtsp_);
    twoNetworks.otherAddresses = {"127.0.0.2"};
    mdns_.add(twoNetworks);
    start();
    ASSERT_TRUE(found(1));
    const auto first = receivers().front().address;
    bool moved = false;
    const auto until = std::chrono::steady_clock::now() + 1500ms;
    while (std::chrono::steady_clock::now() < until) {
        moved = moved || receivers().front().address != first;
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_FALSE(moved);
}

TEST_F(AirPlayFinderTest, ARenamedReceiverIsTheSameOneUnderItsNewName) {
    mdns_.add(record(rtsp_));
    start();
    ASSERT_TRUE(found(1));
    mdns_.rename("800A805D016B", "Living Room");
    // The old name's record stays until it ages out, and the two never show as two receivers.
    EXPECT_TRUE(waitFor([&] {
        const auto now = receivers();
        EXPECT_EQ(now.size(), 1u);
        return now.size() == 1 && now.front().name == "Living Room";
    }));
    EXPECT_EQ(receivers().front().id, "800A805D016B");
}

TEST_F(AirPlayFinderTest, AReceiverThatStopsAnsweringGoes) {
    mdns_.add(record(rtsp_));
    start();
    ASSERT_TRUE(found(1));
    mdns_.clear();
    EXPECT_TRUE(found(0));
}

TEST_F(AirPlayFinderTest, APasswordIsNotSupportedAndNothingAsksIt) {
    auto locked = record(rtsp_);
    locked.raopTxt = {"am=AudioAccessory5,1", "cn=1", "et=0", "pw=true"};
    mdns_.add(locked);
    start();
    ASSERT_TRUE(found(1));
    EXPECT_EQ(supportOf(), Support::Password);
    std::this_thread::sleep_for(200ms);
    EXPECT_TRUE(rtsp_.requests().empty());
}

// The owner's Mac, judged with its password off, went on being offered as playable once it was turned on. A flag about
// something else, as the one a Mac sets while it plays, is no reason to ask again.
TEST_F(AirPlayFinderTest, AReceiverThatTurnsItsPasswordOnAsksForItAndIsAskedAgainOnceItIsOff) {
    const auto announce = [&](const std::string& flags) {
        mdns_.setRaop("C0FFEE000003", {"am=Mac14,2", "cn=0,1", "et=0,3,5", "sf=" + flags, "vs=1005.13.5"});
        mdns_.announce("C0FFEE000003", {"deviceid=C0:FF:EE:00:00:03", "flags=" + flags, "acl=0"});
    };
    mdns_.add(record(rtsp_, "C0FFEE000003", "MacBook Air"));
    announce("0x4");
    start();
    ASSERT_TRUE(found(1));
    ASSERT_TRUE(waitFor([&] { return rtsp_.count("OPTIONS") == 1; }));
    EXPECT_EQ(supportOf(), Support::Supported);

    announce("0x20004");
    std::this_thread::sleep_for(400ms);
    EXPECT_EQ(rtsp_.count("OPTIONS"), 1);
    EXPECT_EQ(supportOf(), Support::Supported);

    announce("0x284");
    EXPECT_TRUE(waitFor([&] { return supportOf() == Support::Password; }));

    announce("0x4");
    // Asked on a thread of its own, every 100 ms once it is free: a busy computer takes longer.
    EXPECT_TRUE(waitFor([&] { return rtsp_.count("OPTIONS") == 2; }, 20s));
    EXPECT_TRUE(waitFor([&] { return supportOf() == Support::Supported; }));
}

// The receivers are asked one after another on a thread of their own, which one that does not answer holds until it
// times out, a second here: a password turned on and off again meanwhile is a setting announced all the same, so the
// receiver is asked again once the thread is free.
TEST_F(AirPlayFinderTest, APasswordTurnedOnAndOffWhileAnotherIsAskedIsAskedAgain) {
    FakeRtsp silent;
    silent.stopAnswering();
    const auto announce = [&](const std::string& flags) {
        mdns_.setRaop("C0FFEE000004", {"am=Mac14,2", "cn=0,1", "et=0,3,5", "sf=" + flags, "vs=1005.13.5"});
        mdns_.announce("C0FFEE000004", {"deviceid=C0:FF:EE:00:00:04", "flags=" + flags, "acl=0"});
    };
    const auto supportOfMac = [&] {
        const auto all = receivers();
        const auto mac = std::ranges::find(all, std::string("C0FFEE000004"), &Receiver::id);
        return mac == all.end() ? Support::Refused : mac->support;
    };
    mdns_.add(record(rtsp_, "C0FFEE000004", "MacBook Pro"));
    mdns_.add(record(silent, "C0FFEE000005", "Silent"));
    announce("0x4");
    start();
    ASSERT_TRUE(found(2));
    ASSERT_TRUE(waitFor([&] { return rtsp_.count("OPTIONS") == 1 && silent.count("OPTIONS") == 1; }));
    std::this_thread::sleep_for(1500ms); // past the silent one's first ask
    mdns_.setRaop("C0FFEE000005", {"am=AudioAccessory5,1", "cn=0,1", "et=0,4", "sf=0x4", "vs=999.1"});
    ASSERT_TRUE(waitFor([&] { return silent.count("OPTIONS") == 2; })) << "the silent one asked again, for a second";
    announce("0x284");
    ASSERT_TRUE(waitFor([&] { return supportOfMac() == Support::Password; }));
    announce("0x4");
    // Asked on a thread of its own, every 100 ms once it is free: a busy computer takes longer.
    EXPECT_TRUE(waitFor([&] { return rtsp_.count("OPTIONS") == 2; }, 20s));
}

TEST_F(AirPlayFinderTest, AReceiverWithAirPlayAloneIsFound) {
    auto tv = record(rtsp_, "AABBCCDDEE01", "Living Room TV");
    tv.raop = false;
    tv.airplayTxt = {"deviceid=AA:BB:CC:DD:EE:01", "model=Y-50S30", "flags=0x4", "srcvers=377.40.00", "acl=0"};
    mdns_.add(tv);
    start();
    ASSERT_TRUE(found(1));
    const auto receiver = receivers().front();
    EXPECT_EQ(receiver.id, "AABBCCDDEE01");
    EXPECT_EQ(receiver.name, "Living Room TV");
    EXPECT_EQ(receiver.port, rtsp_.port());
    EXPECT_TRUE(receiver.airplay2);
    EXPECT_TRUE(waitFor([&] { return rtsp_.count("OPTIONS") == 1; }));
    EXPECT_EQ(supportOf(), Support::Supported);
}

// A Mac holds ANNOUNCE until its user accepts the sender, for up to 15 s, past the time a request waits.
TEST_F(AirPlayFinderTest, AReceiverThatAsksItsUserIsWaitedForWhenTurnedOn) {
    rtsp_.askUser(1500ms);
    mdns_.add(record(rtsp_));
    start(1s);
    ASSERT_TRUE(found(1));
    ASSERT_TRUE(waitFor([&] { return rtsp_.count("OPTIONS") == 1; }));
    finder_->check("800A805D016B");
    EXPECT_TRUE(waitFor([&] { return rtsp_.count("TEARDOWN") == 1; }));
    EXPECT_EQ(supportOf(), Support::Supported);
}

// A receiver that holds its answer, as a Mac waiting for its user does, holds up only its own check: the search goes on.
TEST_F(AirPlayFinderTest, AReceiverWaitingForItsUserHoldsUpNoSearch) {
    rtsp_.askUser(4000ms);
    mdns_.add(record(rtsp_));
    start();
    ASSERT_TRUE(found(1));
    ASSERT_TRUE(waitFor([&] { return rtsp_.count("OPTIONS") == 1; }));
    finder_->check("800A805D016B");
    ASSERT_TRUE(waitFor([&] { return rtsp_.count("ANNOUNCE") == 1; }));
    FakeRtsp other;
    mdns_.add(record(other, "0123456789AB", "Other"));
    EXPECT_TRUE(waitFor([&] { return receivers().size() == 2; }, 2s));
}

TEST_F(AirPlayFinderTest, EncryptionOnlyIsNotSupported) {
    auto encrypted = record(rtsp_);
    encrypted.raopTxt = {"am=AudioAccessory5,1", "cn=1", "et=1,3,5"};
    mdns_.add(encrypted);
    start();
    ASSERT_TRUE(found(1));
    EXPECT_EQ(supportOf(), Support::EncryptedOnly);
}

TEST_F(AirPlayFinderTest, AReceiverThatRefusesOptionsIsNotSupportedThoughItDeclaresEtZero) {
    rtsp_.answer("OPTIONS", 403);
    mdns_.add(record(rtsp_));
    start(1s * testutil::kPairingSlowdown);
    ASSERT_TRUE(found(1));
    EXPECT_TRUE(waitFor([&] { return supportOf() == Support::Refused; }));
}

// One that refuses every request until paired, with the code transient pairing uses, is played paired.
TEST_F(AirPlayFinderTest, AReceiverThatRefusesUntilPairedIsSupportedPaired) {
    rtsp_.demandPairing();
    mdns_.add(record(rtsp_));
    start(1s * testutil::kPairingSlowdown);
    ASSERT_TRUE(found(1));
    const auto pairedNow = [&] {
        const auto list = receivers();
        return !list.empty() && list.front().transient;
    };
    EXPECT_TRUE(waitFor([&] { return pairedNow(); }));
    EXPECT_EQ(supportOf(), Support::Supported);
    finder_->check("800A805D016B");
    EXPECT_TRUE(waitFor([&] { return std::ranges::count(rtsp_.methods(), "TEARDOWN") == 1; }));
    EXPECT_EQ(supportOf(), Support::Supported);
    EXPECT_TRUE(waitFor(pairedNow));
}

// A Mac refuses every plain request and transient pairing too.
TEST_F(AirPlayFinderTest, AReceiverThatRefusesAndDoesNotPairIsRefused) {
    rtsp_.demandPairing("1234");
    mdns_.add(record(rtsp_));
    start(1s * testutil::kPairingSlowdown);
    ASSERT_TRUE(found(1));
    EXPECT_TRUE(waitFor([&] { return supportOf() == Support::Refused; }));
    EXPECT_FALSE(receivers().at(0).transient);
    EXPECT_EQ(rtsp_.count("ANNOUNCE"), 0);
}

TEST_F(AirPlayFinderTest, AnEnabledReceiverIsAskedForASessionThatItRefuses) {
    rtsp_.answer("ANNOUNCE", 403);
    mdns_.add(record(rtsp_));
    start();
    ASSERT_TRUE(found(1));
    EXPECT_EQ(supportOf(), Support::Supported);
    finder_->check("800A805D016B");
    EXPECT_TRUE(waitFor([&] { return supportOf() == Support::Refused; }));
    const auto methods = rtsp_.methods();
    EXPECT_EQ(std::ranges::count(methods, "ANNOUNCE"), 1);
    EXPECT_EQ(std::ranges::count(methods, "TEARDOWN"), 0);
}

// A HomePod that refused while it let only people of the home play is asked again when it is turned on again.
TEST_F(AirPlayFinderTest, AReceiverThatRefusedIsAskedAgainWhenTheUserTurnsItOn) {
    rtsp_.answer("ANNOUNCE", 403);
    mdns_.add(record(rtsp_));
    start();
    ASSERT_TRUE(found(1));
    finder_->check("800A805D016B");
    ASSERT_TRUE(waitFor([&] { return supportOf() == Support::Refused; }));
    rtsp_.answer("ANNOUNCE", 200);
    finder_->check("800A805D016B");
    EXPECT_TRUE(waitFor([&] { return supportOf() == Support::Supported; }));
    EXPECT_EQ(std::ranges::count(rtsp_.methods(), "ANNOUNCE"), 2);
}

TEST_F(AirPlayFinderTest, ASessionThatIsTakenIsClosedAtOnceAndNeverStarted) {
    mdns_.add(record(rtsp_));
    start();
    ASSERT_TRUE(found(1));
    finder_->check("800A805D016B");
    EXPECT_TRUE(waitFor([&] { return std::ranges::count(rtsp_.methods(), "TEARDOWN") == 1; }));
    const auto methods = rtsp_.methods();
    EXPECT_EQ(std::ranges::count(methods, "SETUP"), 0);
    EXPECT_EQ(std::ranges::count(methods, "RECORD"), 0);
    EXPECT_EQ(supportOf(), Support::Supported);
}

TEST_F(AirPlayFinderTest, ABusyReceiverIsNotCalledUnsupported) {
    rtsp_.answer("ANNOUNCE", 453);
    mdns_.add(record(rtsp_));
    start();
    ASSERT_TRUE(found(1));
    finder_->check("800A805D016B");
    EXPECT_TRUE(waitFor([&] { return std::ranges::count(rtsp_.methods(), "ANNOUNCE") == 1; }));
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(supportOf(), Support::Supported);
}

TEST_F(AirPlayFinderTest, NoRequestAsksForEncryption) {
    mdns_.add(record(rtsp_));
    start();
    ASSERT_TRUE(found(1));
    finder_->check("800A805D016B");
    EXPECT_TRUE(waitFor([&] { return std::ranges::count(rtsp_.methods(), "TEARDOWN") == 1; }));
    for (const auto& request : rtsp_.requests()) {
        EXPECT_EQ(request.find("Apple-Challenge"), std::string::npos);
        EXPECT_EQ(request.find("rsaaeskey"), std::string::npos);
        EXPECT_EQ(request.find("fpaeskey"), std::string::npos);
    }
}

TEST_F(AirPlayFinderTest, AStereoPairIsListedOnce) {
    FakeRtsp second;
    auto follower = record(rtsp_, "32FBAAAB399D", "Bedroom");
    follower.airplayTxt = {"deviceid=32:FB:AA:AB:39:9D", "tsid=GROUP", "gpn=Pair", "igl=0"};
    auto leader = record(second, "FAE2708FA125", "Bedroom (2)");
    leader.airplayTxt = {"deviceid=FA:E2:70:8F:A1:25", "tsid=GROUP", "gpn=Pair", "igl=1"};
    mdns_.add(follower);
    mdns_.add(leader);
    start();
    EXPECT_TRUE(waitFor([&] { return receivers().size() == 1 && receivers().front().id == "GROUP"; }));
    EXPECT_TRUE(waitFor([&] { return receivers().front().port == second.port(); }));
    EXPECT_EQ(receivers().front().name, "Pair");
    EXPECT_EQ(receivers().front().speakers, (std::vector<std::string>{"32FBAAAB399D", "FAE2708FA125"}));
}

TEST_F(AirPlayFinderTest, OneReceiverHasNoSpeakersListed) {
    mdns_.add(record(rtsp_));
    start();
    ASSERT_TRUE(found(1));
    EXPECT_TRUE(receivers().front().speakers.empty());
}

TEST(RtspClientTest, AReceiverThatDoesNotAnswerThrows) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    const SOCKET closed = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(::bind(closed, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    int length = sizeof(address);
    getsockname(closed, reinterpret_cast<sockaddr*>(&address), &length);
    const uint16_t port = ntohs(address.sin_port);
    closesocket(closed);
    EXPECT_THROW(RtspClient("127.0.0.1", port, 500), std::runtime_error);
    EXPECT_THROW(RtspClient("not an address", port, 500), std::runtime_error);
    WSACleanup();
}

// The long wait for a receiver's user to show a code holds for that request alone, also when it fails.
TEST(RtspClientTest, TheWaitForACodeEndsWithItsRequest) {
    FakeRtsp rtsp;
    RtspClient client("127.0.0.1", rtsp.port(), 300);
    rtsp.stopAnswering();
    EXPECT_THROW(client.showPin(1500), std::runtime_error);
    const auto before = std::chrono::steady_clock::now();
    EXPECT_THROW(client.request("OPTIONS", "*"), std::runtime_error);
    EXPECT_LT(std::chrono::steady_clock::now() - before, 1000ms);
}

TEST(AirPlayOutputsTest, ReceiversAreListedWithPlayersAndKeptByDeviceId) {
    DeviceCatalog devices;
    devices.revision = 1;
    devices.airplay = {{"800A805D016B", "DMP-A6"}};
    Settings s;
    s.adoptOutputSets = false;
    setOutputEnabled(s, Backend::AirPlay, "800A805D016B", true, devices);
    selectOutput(s, Backend::AirPlay, "800A805D016B", devices);
    EXPECT_EQ(s.backend, Backend::AirPlay);
    EXPECT_EQ(s.airplay.deviceId, "800A805D016B");
    EXPECT_EQ(s.volume.mode, VolumeMode::Player);
    s.airplay.timing.releaseAfterPauseSeconds = 30;
    syncOutputs(s, devices);

    auto gone = devices;
    gone.revision = 2;
    gone.airplay.clear();
    const auto missing = outputRows(s, gone).back();
    EXPECT_EQ(missing.backend, Backend::AirPlay);
    EXPECT_FALSE(missing.connected);
    EXPECT_TRUE(missing.selected);
    EXPECT_EQ(missing.name, "DMP-A6");

    Settings read = fromJson(toJson(s));
    EXPECT_EQ(read.backend, Backend::AirPlay);
    EXPECT_EQ(read.airplay.deviceId, "800A805D016B");
    selectOutput(read, Backend::Wasapi, "", devices);
    selectOutput(read, Backend::AirPlay, "800A805D016B", devices);
    EXPECT_EQ(read.airplay.timing.releaseAfterPauseSeconds, 30);
}

TEST(AirPlayOutputsTest, ARenamedReceiverKeepsItsSettings) {
    DeviceCatalog devices;
    devices.revision = 1;
    devices.airplay = {{"800A805D016B", "DMP-A6"}};
    Settings s;
    s.adoptOutputSets = false;
    setOutputEnabled(s, Backend::AirPlay, "800A805D016B", true, devices);
    selectOutput(s, Backend::AirPlay, "800A805D016B", devices);
    s.airplay.timing.releaseAfterPauseSeconds = 30;
    syncOutputs(s, devices);

    auto renamed = devices;
    renamed.revision = 2;
    renamed.airplay = {{"800A805D016B", "Living Room"}};
    syncOutputs(s, renamed);
    const auto row = outputRows(s, renamed).back();
    EXPECT_EQ(row.backend, Backend::AirPlay);
    EXPECT_EQ(row.name, "Living Room");
    EXPECT_TRUE(row.connected);
    EXPECT_TRUE(row.selected);
    EXPECT_EQ(s.airplay.deviceId, "800A805D016B");
    EXPECT_EQ(s.airplay.timing.releaseAfterPauseSeconds, 30);
}

// The Home app gives a stereo pair a new ID each time it pairs it.
TEST(AirPlayOutputsTest, AStereoPairPairedAgainKeepsItsSettings) {
    const std::vector<std::string> speakers{"32FBAAAB399D", "FAE2708FA125"};
    DeviceCatalog paired;
    paired.revision = 1;
    paired.airplay = {{"EB1DD2A4", "主臥", "", speakers}};
    Settings s;
    s.adoptOutputSets = false;
    setOutputEnabled(s, Backend::AirPlay, "EB1DD2A4", true, paired);
    renameOutput(s, Backend::AirPlay, "EB1DD2A4", "Bedroom", paired);
    selectOutput(s, Backend::AirPlay, "EB1DD2A4", paired);
    s.airplay.timing.releaseAfterPauseSeconds = 30;
    s.airplayLevels["EB1DD2A4"] = {50, false};
    syncOutputs(s, paired);
    s = fromJson(toJson(s));

    DeviceCatalog separated;
    separated.revision = 2;
    separated.airplay = {{"32FBAAAB399D", "主臥"}, {"FAE2708FA125", "主臥 (2)"}};
    selectOutput(s, Backend::AirPlay, "32FBAAAB399D", separated);
    const auto rows = outputRows(s, separated);
    const auto row = [&](const std::vector<OutputRow>& list, const std::string& id) {
        return *std::ranges::find_if(list, [&](const OutputRow& r) { return r.id == id; });
    };
    EXPECT_FALSE(row(rows, "EB1DD2A4").connected); // the pair waits for its speakers
    EXPECT_TRUE(row(rows, "32FBAAAB399D").selected);
    selectOutput(s, Backend::AirPlay, "EB1DD2A4", separated);

    DeviceCatalog again;
    again.revision = 3;
    again.airplay = {{"F2C47A35", "主臥", "", speakers}};
    syncOutputs(s, again);
    const auto after = outputRows(s, again);
    EXPECT_EQ(std::ranges::count_if(after, [](const OutputRow& r) { return r.id == "EB1DD2A4"; }), 0);
    const auto pair = row(after, "F2C47A35");
    EXPECT_TRUE(pair.connected);
    EXPECT_TRUE(pair.selected);
    EXPECT_TRUE(pair.enabled);
    EXPECT_EQ(pair.name, "Bedroom");
    EXPECT_EQ(s.airplay.deviceId, "F2C47A35");
    EXPECT_EQ(s.airplay.timing.releaseAfterPauseSeconds, 30);
    EXPECT_EQ(s.airplayLevels.count("EB1DD2A4"), 0u);
    EXPECT_EQ(s.airplayLevels.at("F2C47A35").level, 50u);
    // The speaker played alone went into the pair (the owner, 2026-11-15).
    EXPECT_EQ(std::ranges::count_if(after, [](const OutputRow& r) { return r.id == "32FBAAAB399D"; }), 0);
}

// A pair not in use paired again reads back as it is, as the Server takes settings back from the window.
TEST(AirPlayOutputsTest, APairNotInUsePairedAgainReadsBackAsItIs) {
    const std::vector<std::string> speakers{"32FBAAAB399D", "FAE2708FA125"};
    DeviceCatalog paired;
    paired.revision = 1;
    paired.airplay = {{"EB1DD2A4", "主臥", "", speakers}};
    Settings s;
    s.adoptOutputSets = false;
    setOutputEnabled(s, Backend::AirPlay, "EB1DD2A4", true, paired);
    DeviceCatalog again;
    again.revision = 2;
    again.airplay = {{"F2C47A35", "主臥", "", speakers}};
    syncOutputs(s, again);
    ASSERT_TRUE(std::ranges::any_of(s.outputs, [](const SavedOutput& o) { return o.id == "F2C47A35"; }));
    EXPECT_EQ(settingsDto(fromJson(toJson(s))), settingsDto(s));
}

// A pair saved before its speakers were recorded is known again by its name; a speaker's own entry goes into the pair.
TEST(AirPlayOutputsTest, APairSavedWithoutItsSpeakersIsKnownByItsName) {
    DeviceCatalog before;
    before.revision = 1;
    before.airplay = {{"EB1DD2A4", "主臥"}, {"32FBAAAB399D", "主臥"}};
    Settings s;
    s.adoptOutputSets = false;
    setOutputEnabled(s, Backend::AirPlay, "EB1DD2A4", true, before);
    setOutputEnabled(s, Backend::AirPlay, "32FBAAAB399D", true, before);
    renameOutput(s, Backend::AirPlay, "32FBAAAB399D", "Left", before);
    DeviceCatalog again;
    again.revision = 2;
    again.airplay = {{"F2C47A35", "主臥", "", {"32FBAAAB399D", "FAE2708FA125"}}};
    syncOutputs(s, again);
    const auto rows = outputRows(s, again);
    ASSERT_EQ(rows.size(), 2u); // Windows default and the pair
    EXPECT_EQ(rows[1].id, "F2C47A35");
    EXPECT_TRUE(rows[1].enabled);
}

// Two speakers paired in the Home app are one receiver from then on: what was saved for each alone goes into the pair,
// which is in use and in the group where either was, rather than staying as two receivers that are never connected.
TEST(AirPlayOutputsTest, SpeakersPairedGoIntoThePair) {
    DeviceCatalog alone;
    alone.revision = 1;
    alone.airplay = {{"32FBAAAB399D", "主臥"}, {"FAE2708FA125", "主臥 (2)"}};
    Settings s;
    s.adoptOutputSets = false;
    selectOutput(s, Backend::AirPlay, "32FBAAAB399D", alone);
    setOutputEnabled(s, Backend::AirPlay, "FAE2708FA125", true, alone);
    setInAirPlayGroup(s, "FAE2708FA125", true, alone);
    s.airplayLevels["32FBAAAB399D"] = {40, false};
    ASSERT_EQ(s.airplayGroup, (std::vector<std::string>{"32FBAAAB399D", "FAE2708FA125"}));

    DeviceCatalog paired;
    paired.revision = 2;
    paired.airplay = {{"0F6771AD", "主臥", "", {"32FBAAAB399D", "FAE2708FA125"}}};
    syncOutputs(s, paired);
    const auto rows = outputRows(s, paired);
    EXPECT_EQ(std::ranges::count_if(rows, [](const OutputRow& r) { return r.id == "32FBAAAB399D" || r.id == "FAE2708FA125"; }),
              0);
    const auto pair = std::ranges::find_if(rows, [](const OutputRow& r) { return r.id == "0F6771AD"; });
    ASSERT_NE(pair, rows.end());
    EXPECT_TRUE(pair->selected);
    EXPECT_TRUE(pair->enabled);
    EXPECT_EQ(s.airplay.deviceId, "0F6771AD");
    EXPECT_EQ(s.airplayGroup, (std::vector<std::string>{"0F6771AD"}));
    EXPECT_EQ(s.airplayLevels.at("0F6771AD").level, 40u);
    EXPECT_EQ(s.airplayLevels.count("32FBAAAB399D"), 0u);
}

TEST(AirPlayOutputsTest, AnUnsupportedReceiverSaysWhyAndDoesNotStayEnabled) {
    DeviceCatalog devices;
    devices.revision = 1;
    devices.airplay = {{"FAE2708FA125", "主臥", "refused"}};
    Settings s;
    s.adoptOutputSets = false;
    setOutputEnabled(s, Backend::AirPlay, "FAE2708FA125", true, devices);
    const auto row = outputRows(s, devices).back();
    EXPECT_EQ(row.unsupported, "refused");
    EXPECT_FALSE(row.enabled);
}

TEST(AirPlayServerTest, EnablingAReceiverAsksItAndARefusalTakesItBackOff) {
    FakeRtsp rtsp;
    FakeMdns mdns;
    rtsp.answer("ANNOUNCE", 403);
    mdns.add(record(rtsp));
    Settings settings;
    settings.adoptOutputSets = false;
    settings.autoplay = false;
    LocalServer server(settings);
    server.setAirPlayFinderOptions(optionsFor(mdns));
    const auto refresh = [&] {
        Command command;
        command.kind = CommandKind::RefreshDevices;
        server.submit(std::move(command));
        server.waitIdle();
    };
    ASSERT_TRUE(waitFor([&] {
        refresh();
        return !server.snapshot().devices.airplay.empty();
    }));
    EXPECT_TRUE(server.snapshot().devices.airplay.front().unsupported.empty());
    EXPECT_EQ(std::ranges::count(rtsp.methods(), "ANNOUNCE"), 0);

    auto chosen = nativeSettings(server.snapshot().settings);
    setOutputEnabled(chosen, Backend::AirPlay, "800A805D016B", true, server.snapshot().devices);
    Command apply;
    apply.kind = CommandKind::ApplySettings;
    apply.settings = settingsDto(chosen);
    server.submit(std::move(apply));
    server.waitIdle();

    // Refused, it is not offered any more; its saved output stays, off and not connected.
    EXPECT_TRUE(waitFor([&] {
        refresh();
        return server.snapshot().devices.airplay.empty();
    }));
    const auto snapshot = server.snapshot();
    const auto rows = outputRows(nativeSettings(snapshot.settings), snapshot.devices);
    const auto row =
        *std::ranges::find_if(rows, [](const OutputRow& r) { return r.backend == Backend::AirPlay && r.id == "800A805D016B"; });
    EXPECT_FALSE(row.connected);
    EXPECT_FALSE(row.enabled);
}

// A receiver CamelPlayer cannot play, here one that takes only encrypted sessions, is not in the catalog the Control
// point gets; one that becomes playable appears, and a saved one that becomes unplayable shows as not connected.
TEST(AirPlayServerTest, OnlyReceiversThatCanBePlayedAreOffered) {
    FakeRtsp rtsp;
    FakeMdns mdns;
    mdns.add(record(rtsp, "800A805D016B", "DMP-A6"));
    auto encrypted = record(rtsp, "C0FFEE000002", "Encrypted");
    encrypted.raopTxt = {"am=Other", "cn=1", "et=1,3,5"};
    mdns.add(encrypted);
    Settings settings;
    settings.adoptOutputSets = false;
    settings.autoplay = false;
    LocalServer server(settings);
    server.setAirPlayFinderOptions(optionsFor(mdns));
    const auto offered = [&] {
        Command command;
        command.kind = CommandKind::RefreshDevices;
        server.submit(std::move(command));
        server.waitIdle();
        std::vector<std::string> names;
        for (const auto& device : server.snapshot().devices.airplay) names.push_back(device.name);
        return names;
    };
    ASSERT_TRUE(waitFor([&] { return offered() == std::vector<std::string>{"DMP-A6"}; }));

    mdns.setRaop("C0FFEE000002", {"am=Other", "cn=0,1", "et=0,1", "pw=false", "vs=105.1"});
    ASSERT_TRUE(waitFor([&] { return offered() == (std::vector<std::string>{"DMP-A6", "Encrypted"}); }));

    auto chosen = nativeSettings(server.snapshot().settings);
    setOutputEnabled(chosen, Backend::AirPlay, "C0FFEE000002", true, server.snapshot().devices);
    Command apply;
    apply.kind = CommandKind::ApplySettings;
    apply.settings = settingsDto(chosen);
    server.submit(std::move(apply));
    server.waitIdle();
    mdns.setRaop("C0FFEE000002", {"am=Other", "cn=1", "et=1,3,5"});
    ASSERT_TRUE(waitFor([&] { return offered() == std::vector<std::string>{"DMP-A6"}; }));
    const auto snapshot = server.snapshot();
    const auto rows = outputRows(nativeSettings(snapshot.settings), snapshot.devices);
    const auto row = std::ranges::find_if(rows, [](const OutputRow& r) { return r.id == "C0FFEE000002"; });
    ASSERT_NE(row, rows.end());
    EXPECT_FALSE(row->connected);
}

// The receivers of this network as the finder lists them, the Chord Poly among them, which answers only the group:
// opt in with --gtest_also_run_disabled_tests --gtest_filter=AirPlayFinderHardware.*
TEST(AirPlayFinderHardware, DISABLED_ListsTheReceiversOnThisNetwork) {
    AirPlayFinder finder(AirPlayFinderOptions{}, nullptr);
    std::this_thread::sleep_for(15s);
    const auto found = finder.receivers();
    for (const auto& receiver : found)
        std::printf("%s at %s:%u, version %s, support %s\n", receiver.name.c_str(), receiver.address.c_str(),
                    unsigned(receiver.port), receiver.version.c_str(), supportId(receiver.support));
    EXPECT_FALSE(found.empty());
}

namespace {

// A Server finding the receivers `mdns` announces, with the device catalog asked for again on each call.
class PairingServer {
public:
    explicit PairingServer(const FakeMdns& mdns) : server_(serverSettings()) {
        auto options = optionsFor(mdns);
        options.searchInterval = 1s * testutil::kPairingSlowdown;
        server_.setAirPlayFinderOptions(options);
    }

    std::string supportOf(const std::string& id) {
        Command command;
        command.kind = CommandKind::RefreshDevices;
        server_.submit(std::move(command));
        server_.waitIdle();
        for (const auto& device : server_.snapshot().devices.airplay)
            if (device.id == id) return device.unsupported;
        return "absent";
    }

    // Showing a code and pairing end beside the command thread, and say how in `finished`.
    std::optional<Failure> run(Command command) {
        const auto stamp = server_.submit(std::move(command));
        server_.waitIdle();
        const auto now = server_.snapshot();
        return now.finished.command == stamp.id ? now.finished.failure : now.commandError;
    }

    std::optional<Failure> pair(const std::string& id, const std::string& code) {
        Command command;
        command.kind = CommandKind::PairAirPlay;
        command.receiver = id;
        command.code = code;
        return run(std::move(command));
    }

    LocalServer& server() { return server_; }

private:
    static Settings serverSettings() {
        Settings settings;
        settings.adoptOutputSets = false;
        settings.autoplay = false;
        return settings;
    }

    LocalServer server_;
};

} // namespace

// A Mac with a password set says so in its flags; the password typed pairs it, a wrong one says so.
TEST(AirPlayPairingServerTest, AReceiverThatAsksForAPasswordIsPairedWithIt) {
    FakeRtsp rtsp;
    rtsp.demandPairing("hunter2");
    FakeMdns mdns;
    auto mac = record(rtsp, "C0FFEE000003", "MacBook Air");
    mac.raopTxt = {"am=Mac14,2", "cn=0,1", "et=0,3,5", "sf=0x284", "vs=1005.13.5"};
    mac.airplayTxt = {"deviceid=C0:FF:EE:00:00:03", "flags=0x284", "acl=0"};
    mdns.add(mac);
    PairingServer server(mdns);
    ASSERT_TRUE(waitFor([&] { return server.supportOf("C0FFEE000003") == "password"; }));
    EXPECT_EQ(rtsp.count("OPTIONS"), 0) << "a receiver that asks for a password is not asked before it has one";

    const auto wrong = server.pair("C0FFEE000003", "hunter3");
    ASSERT_TRUE(wrong);
    EXPECT_EQ(wrong->code, audio::ErrorCode::AccessDenied);
    EXPECT_EQ(server.supportOf("C0FFEE000003"), "password");

    EXPECT_FALSE(server.pair("C0FFEE000003", "hunter2"));
    EXPECT_EQ(server.supportOf("C0FFEE000003"), "");
    // Asked again over the paired connection; a Mac, played over AirPlay 2, is asked for a session only when it plays.
    EXPECT_TRUE(waitFor([&] { return rtsp.encryptedRequests() > 0 && rtsp.count("OPTIONS") >= 1; }));
    EXPECT_EQ(rtsp.count("ANNOUNCE"), 0);
}

// A receiver that shows a code: asked to show it, then paired with it, it keeps keys that later sessions verify alone.
TEST(AirPlayPairingServerTest, AReceiverThatShowsACodeIsPairedWithItAndVerifiedAfter) {
    FakeRtsp rtsp;
    rtsp.demandPairing("4821");
    rtsp.pairOnlyWithKeys();
    FakeMdns mdns;
    auto tv = record(rtsp, "C0FFEE000004", "TV");
    tv.airplayTxt = {"deviceid=C0:FF:EE:00:00:04", "flags=0x8", "acl=0"};
    mdns.add(tv);
    PairingServer server(mdns);
    ASSERT_TRUE(waitFor([&] { return server.supportOf("C0FFEE000004") == "code"; }));

    Command show;
    show.kind = CommandKind::ShowAirPlayCode;
    show.receiver = "C0FFEE000004";
    EXPECT_FALSE(server.run(std::move(show)));
    EXPECT_EQ(rtsp.pinsShown(), 1);
    const auto wrong = server.pair("C0FFEE000004", "1111");
    ASSERT_TRUE(wrong);
    EXPECT_EQ(wrong->code, audio::ErrorCode::AccessDenied);
    EXPECT_FALSE(server.pair("C0FFEE000004", "4821"));
    EXPECT_EQ(rtsp.controllers().size(), 1U);
    EXPECT_EQ(server.supportOf("C0FFEE000004"), "");

    // Turned on, it is asked for a session, which pair-verify opens.
    auto chosen = nativeSettings(server.server().snapshot().settings);
    setOutputEnabled(chosen, Backend::AirPlay, "C0FFEE000004", true, server.server().snapshot().devices);
    Command apply;
    apply.kind = CommandKind::ApplySettings;
    apply.settings = settingsDto(chosen);
    EXPECT_FALSE(server.run(std::move(apply)));
    EXPECT_TRUE(waitFor([&] { return rtsp.count("TEARDOWN") >= 1; }));
    EXPECT_TRUE(std::ranges::any_of(rtsp.requests(), [](const std::string& r) { return r.starts_with("POST /pair-verify "); }));
    EXPECT_EQ(server.supportOf("C0FFEE000004"), "");

    // Deleted in Settings, it is no longer paired and asks for its code again.
    chosen = nativeSettings(server.server().snapshot().settings);
    deleteOutput(chosen, Backend::AirPlay, "C0FFEE000004", server.server().snapshot().devices);
    Command remove;
    remove.kind = CommandKind::ApplySettings;
    remove.settings = settingsDto(chosen);
    EXPECT_FALSE(server.run(std::move(remove)));
    EXPECT_TRUE(waitFor([&] { return server.supportOf("C0FFEE000004") == "code"; }));
}

// A receiver whose user is still deciding whether to show a code holds up no other command.
TEST(AirPlayPairingServerTest, ShowingACodeHoldsUpNoOtherCommand) {
    FakeRtsp rtsp;
    rtsp.demandPairing("4821");
    rtsp.pairOnlyWithKeys();
    rtsp.askUser(3000ms);
    FakeMdns mdns;
    auto tv = record(rtsp, "C0FFEE000006", "TV");
    tv.airplayTxt = {"deviceid=C0:FF:EE:00:00:06", "flags=0x8", "acl=0"};
    mdns.add(tv);
    PairingServer server(mdns);
    ASSERT_TRUE(waitFor([&] { return server.supportOf("C0FFEE000006") == "code"; }));
    Command show;
    show.kind = CommandKind::ShowAirPlayCode;
    show.receiver = "C0FFEE000006";
    server.server().submit(std::move(show));
    ASSERT_TRUE(waitFor([&] {
        return std::ranges::any_of(rtsp.requests(), [](const std::string& r) { return r.starts_with("POST /pair-pin-start "); });
    }));
    Command shuffle;
    shuffle.kind = CommandKind::SetShuffle;
    shuffle.shuffle = true;
    const auto stamp = server.server().submit(std::move(shuffle));
    EXPECT_TRUE(waitFor([&] { return server.server().snapshot().command.id >= stamp.id; }, 1000ms));
    server.server().waitIdle();
}

// A receiver reset forgets this computer: it asks for its code again rather than being refused for good.
TEST(AirPlayPairingServerTest, AReceiverThatForgotThePairingAsksForItsCodeAgain) {
    FakeRtsp rtsp;
    rtsp.demandPairing("4821");
    rtsp.pairOnlyWithKeys();
    FakeMdns mdns;
    auto tv = record(rtsp, "C0FFEE000004", "TV");
    tv.airplayTxt = {"deviceid=C0:FF:EE:00:00:04", "flags=0x8", "acl=0"};
    mdns.add(tv);
    PairingServer server(mdns);
    ASSERT_TRUE(waitFor([&] { return server.supportOf("C0FFEE000004") == "code"; }));
    EXPECT_FALSE(server.pair("C0FFEE000004", "4821"));
    EXPECT_EQ(server.supportOf("C0FFEE000004"), "");
    rtsp.forgetPairings();
    auto chosen = nativeSettings(server.server().snapshot().settings);
    setOutputEnabled(chosen, Backend::AirPlay, "C0FFEE000004", true, server.server().snapshot().devices);
    Command apply;
    apply.kind = CommandKind::ApplySettings;
    apply.settings = settingsDto(chosen);
    server.run(std::move(apply));
    EXPECT_TRUE(waitFor([&] { return server.supportOf("C0FFEE000004") == "code"; }));
}

// A pairings file that cannot be read is never written over: held by another program, it stays for the next start and
// this session's pairings are kept in memory; damaged, it is kept aside as .bad and a new one is begun.
TEST(AirPlayPairingsTest, AFileThatCannotBeReadIsNotWrittenOver) {
    const auto file = testutil::tempDir() / L"airplay-pairings-kept.dat";
    const auto bad = std::filesystem::path(file).concat(L".bad");
    const auto read = [](const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string{std::istreambuf_iterator<char>(in), {}};
    };
    for (const auto& f : {file, bad}) std::filesystem::remove(f);
    AirPlayPairings(file).save("C0FFEE000003", {"hunter2", std::nullopt});
    const std::string saved = read(file);
    {
        const HANDLE held = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        ASSERT_NE(held, INVALID_HANDLE_VALUE);
        AirPlayPairings pairings(file);
        CloseHandle(held);
        pairings.identity();
        pairings.save("C0FFEE000005", {"other", std::nullopt});
        EXPECT_EQ(pairings.find("C0FFEE000005"), (AirPlayCredentials{"other", std::nullopt}));
    }
    EXPECT_EQ(read(file), saved);
    EXPECT_EQ(AirPlayPairings(file).find("C0FFEE000003"), (AirPlayCredentials{"hunter2", std::nullopt}));

    std::ofstream(file, std::ios::binary | std::ios::trunc) << "not sealed";
    {
        AirPlayPairings pairings(file);
        pairings.save("C0FFEE000006", {"new", std::nullopt});
    }
    EXPECT_EQ(read(bad), "not sealed");
    EXPECT_EQ(AirPlayPairings(file).find("C0FFEE000006"), (AirPlayCredentials{"new", std::nullopt}));
    for (const auto& f : {file, bad}) std::filesystem::remove(f);
}

TEST(AirPlayPairingsTest, PairingsSurviveInTheirProtectedFile) {
    const auto file = testutil::tempDir() / L"airplay-pairings.dat";
    std::filesystem::remove(file);
    airplay::PairingIdentity identity;
    {
        AirPlayPairings pairings(file);
        identity = pairings.identity();
        pairings.save("C0FFEE000003", {"hunter2", std::nullopt});
        pairings.save("C0FFEE000004", {{}, airplay::PairedReceiver{"FA:KE", std::string(32, '\x7f')}});
    }
    std::ifstream in(file, std::ios::binary);
    const std::string stored{std::istreambuf_iterator<char>(in), {}};
    in.close();
    EXPECT_EQ(stored.find("hunter2"), std::string::npos) << "kept in the clear";
    AirPlayPairings read(file);
    EXPECT_EQ(read.identity(), identity);
    EXPECT_EQ(read.find("C0FFEE000003"), (AirPlayCredentials{"hunter2", std::nullopt}));
    EXPECT_EQ(read.find("C0FFEE000004")->keys->publicKey, std::string(32, '\x7f'));
    read.forget("C0FFEE000003");
    EXPECT_FALSE(AirPlayPairings(file).find("C0FFEE000003"));
    std::filesystem::remove(file);
}
