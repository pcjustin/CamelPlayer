// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>

#include "FakeAirPlayReceiver.h"
#include "Server/Network/AirPlayFinder.h"
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
    const auto deadline = std::chrono::steady_clock::now() + limit;
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
    options.searchInterval = 100ms;
    options.retryInterval = 50ms;
    options.sessionTimeoutMs = 1000;
    return options;
}

class AirPlayFinderTest : public ::testing::Test {
protected:
    void start() { finder_ = std::make_unique<AirPlayFinder>(optionsFor(mdns_), nullptr); }
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

TEST(AirPlayTest, OnlyApplesReceiversAreListed) {
    const auto modelled = [](const char* model) { return TxtRecord{{"am", model}, {"et", "0"}, {"cn", "1"}}; };
    const auto receivers = receiversFrom({{"A", "HomePod", "192.168.0.1", 7000, modelled("AudioAccessory5,1"), {}},
                                          {"B", "Mac", "192.168.0.2", 7000, modelled("Mac14,2"), {}},
                                          {"C", "Apple TV", "192.168.0.3", 7000, modelled("AppleTV11,1"), {}},
                                          {"D", "DMP-A6", "192.168.0.4", 5500, modelled("eversolo,1"), {}},
                                          {"E", "Bravo", "192.168.0.5", 7000, modelled("MUZO Cobblestone"), {}},
                                          {"F", "Unknown", "192.168.0.6", 7000, {{"et", "0"}, {"cn", "1"}}, {}}});
    ASSERT_EQ(receivers.size(), 3U);
    EXPECT_EQ(receivers[0].name, "Apple TV");
    EXPECT_EQ(receivers[1].name, "HomePod");
    EXPECT_EQ(receivers[2].name, "Mac");
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
    start();
    ASSERT_TRUE(found(1));
    EXPECT_TRUE(waitFor([&] { return supportOf() == Support::Refused; }));
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
    EXPECT_FALSE(row(after, "32FBAAAB399D").connected); // the speaker played alone keeps its own entry
}

// A pair saved before its speakers were recorded is known again by its name, and a speaker's own entry is not taken.
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
    ASSERT_EQ(rows.size(), 3u); // Windows default, the pair and the speaker
    EXPECT_EQ(rows[1].id, "F2C47A35");
    EXPECT_TRUE(rows[1].enabled);
    EXPECT_EQ(rows[2].id, "32FBAAAB399D");
    EXPECT_EQ(rows[2].name, "Left");
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

    EXPECT_TRUE(waitFor([&] {
        refresh();
        return !server.snapshot().devices.airplay.front().unsupported.empty();
    }));
    const auto snapshot = server.snapshot();
    const auto row = outputRows(nativeSettings(snapshot.settings), snapshot.devices).back();
    EXPECT_EQ(row.unsupported, "refused");
    EXPECT_FALSE(row.enabled);
}
