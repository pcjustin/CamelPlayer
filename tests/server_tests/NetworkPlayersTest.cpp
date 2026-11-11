// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "FakeRenderer.h"
#include "Server/Network/Firewall.h"
#include "Server/Network/PlayerFinder.h"
#include "Server/Network/PlayerLink.h"

#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")

using namespace camel::server;
using namespace camel::server::upnp;
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

FinderOptions optionsFor(const FakeRenderer& renderer, std::chrono::milliseconds interval = 100ms) {
    FinderOptions options;
    options.searchHost = "127.0.0.1";
    options.searchPort = renderer.ssdpPort();
    options.notifyPort = 0;
    options.searchInterval = interval;
    return options;
}

constexpr LinkOptions kQuick{50ms, 2, 1800, 500};

class Events {
public:
    void operator()(const PlayerEvent& event) {
        std::scoped_lock lock(mutex_);
        events_.push_back(event);
    }
    bool saw(PlayerEventKind kind) const {
        std::scoped_lock lock(mutex_);
        return std::ranges::any_of(events_, [&](const PlayerEvent& e) { return e.kind == kind; });
    }
    bool waitFor(PlayerEventKind kind) const {
        return ::waitFor([&] { return saw(kind); });
    }
    std::optional<PlayerEvent> find(PlayerEventKind kind) const {
        std::scoped_lock lock(mutex_);
        const auto found = std::ranges::find_if(events_, [&](const PlayerEvent& e) { return e.kind == kind; });
        return found == events_.end() ? std::nullopt : std::optional(*found);
    }
    size_t count(PlayerEventKind kind) const {
        std::scoped_lock lock(mutex_);
        return size_t(std::ranges::count_if(events_, [&](const PlayerEvent& e) { return e.kind == kind; }));
    }

private:
    mutable std::mutex mutex_;
    std::vector<PlayerEvent> events_;
};

std::function<void(const PlayerEvent&)> into(const std::shared_ptr<Events>& events) {
    return [events](const PlayerEvent& event) { (*events)(event); };
}

PlayerDescription describe(const FakeRenderer& renderer) {
    auto description = parseDescription(renderer.descriptionXml(), renderer.location());
    if (!description) throw std::logic_error("the fake's description does not parse");
    return *description;
}

} // namespace

TEST(UpnpMessages, ToldApartByTheServicesAPlayerOffers) {
    const FakeRenderer av(RendererKind::UpnpAv), openHome(RendererKind::OpenHome);
    EXPECT_EQ(describe(av).protocol, PlayerProtocol::UpnpAv);
    EXPECT_EQ(describe(openHome).protocol, PlayerProtocol::OpenHome);
    EXPECT_EQ(describe(av).name, "Fake Player");
    EXPECT_EQ(describe(av).udn, "uuid:fake-renderer");
    EXPECT_NE(describe(av).service("RenderingControl"), nullptr);
    EXPECT_EQ(describe(av).service("Playlist"), nullptr);
}

TEST(UpnpMessages, ADeviceWithoutPlaybackIsNotAPlayer) {
    EXPECT_FALSE(parseDescription(
        R"(<root><device><UDN>uuid:nas</UDN><serviceList><service><serviceType>urn:schemas-upnp-org:service:ContentDirectory:1</serviceType><controlURL>/c</controlURL></service></serviceList></device></root>)",
        "http://192.168.1.2/d.xml"));
    EXPECT_FALSE(parseDescription("not xml", "http://192.168.1.2/d.xml"));
}

TEST(UpnpMessages, FindsARendererEmbeddedInARootDevice) {
    const auto player = parseDescription(
        R"(<root><device><UDN>uuid:root</UDN><deviceList><device><UDN>uuid:inner</UDN><friendlyName>Inner</friendlyName><serviceList><service><serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType><controlURL>ctl</controlURL><eventSubURL>/evt</eventSubURL></service></serviceList></device></deviceList></device></root>)",
        "http://192.168.1.2:8080/dir/d.xml");
    ASSERT_TRUE(player);
    EXPECT_EQ(player->udn, "uuid:inner");
    EXPECT_EQ(player->service("AVTransport")->controlUrl, "http://192.168.1.2:8080/dir/ctl");
    EXPECT_EQ(player->service("AVTransport")->eventUrl, "http://192.168.1.2:8080/evt");
}

TEST(UpnpMessages, AControlUrlOnAnotherHostIsNotUsed) {
    const auto player = parseDescription(
        R"(<root><device><UDN>uuid:x</UDN><serviceList><service><serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType><controlURL>http://203.0.113.9/ctl</controlURL></service></serviceList></device></root>)",
        "http://192.168.1.2/d.xml");
    EXPECT_FALSE(player);
}

TEST(UpnpMessages, ReadsSearchAnswersAndAnnouncements) {
    const auto answer = parseSsdp("HTTP/1.1 200 OK\r\nST: urn:schemas-upnp-org:device:MediaRenderer:1\r\nUSN: "
                                  "uuid:a::urn:x\r\nLocation: http://192.168.1.5/d.xml\r\n\r\n");
    ASSERT_TRUE(answer);
    EXPECT_EQ(answer->kind, SsdpKind::Response);
    EXPECT_EQ(answer->udn, "uuid:a");
    EXPECT_EQ(answer->location, "http://192.168.1.5/d.xml");
    const auto bye = parseSsdp(
        "NOTIFY * HTTP/1.1\r\nNT: urn:schemas-upnp-org:device:MediaRenderer:1\r\nNTS: ssdp:byebye\r\nUSN: uuid:a::urn:x\r\n\r\n");
    ASSERT_TRUE(bye);
    EXPECT_EQ(bye->kind, SsdpKind::ByeBye);
    EXPECT_FALSE(
        parseSsdp("HTTP/1.1 200 OK\r\nST: upnp:rootdevice\r\nUSN: uuid:a\r\nLOCATION: http://192.168.1.5/d.xml\r\n\r\n"));
}

TEST(UpnpMessages, ReadingAMessageKeepsNoCopyOfIt) {
    const auto privateBytes = [] {
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
        return int64_t(memory.PrivateUsage);
    };
    const std::string notXml(kMaxMessageBytes, 'x');
    const int64_t before = privateBytes();
    for (int i = 0; i < 1000; ++i) EXPECT_FALSE(parseXml(notXml));
    EXPECT_LT(privateBytes() - before, 500 * int64_t(kMaxMessageBytes));
}

TEST(UpnpMessages, ALocationOutsideTheLocalNetworkIsIgnored) {
    for (const char* location :
         {"http://203.0.113.9/d.xml", "https://192.168.1.5/d.xml", "http://example.com/d.xml", "file:///c:/x"})
        EXPECT_FALSE(parseSsdp(
            std::string("HTTP/1.1 200 OK\r\nST: urn:schemas-upnp-org:device:MediaRenderer:1\r\nUSN: uuid:a\r\nLOCATION: ") +
            location + "\r\n\r\n"))
            << location;
    for (const char* location : {"http://10.0.0.1/", "http://172.16.0.1/", "http://192.168.0.1:80/", "http://169.254.1.1/"})
        EXPECT_TRUE(isLanUrl(location)) << location;
    EXPECT_FALSE(isLanUrl("http://172.32.0.1/"));
}

// WinHTTP takes what comes before an '@' for a user name and password, and connects to the host after it.
TEST(UpnpMessages, ALocationWithUserInfoIsIgnored) {
    for (const char* location : {"http://192.168.1.5:80@example.com/desc.xml", "http://192.168.1.5@example.com/desc.xml",
                                 "http://user:secret@192.168.1.5/desc.xml"}) {
        EXPECT_FALSE(isLanUrl(location)) << location;
        EXPECT_FALSE(parseSsdp(
            std::string("HTTP/1.1 200 OK\r\nST: urn:schemas-upnp-org:device:MediaRenderer:1\r\nUSN: uuid:a\r\nLOCATION: ") +
            location + "\r\n\r\n"))
            << location;
    }
}

// A message on the network must not make CamelPlayer ask this machine's own services for anything.
TEST(UpnpMessages, ALoopbackLocationIsIgnored) {
    for (const char* location : {"http://127.0.0.1/", "http://127.1.2.3:8080/d.xml"}) {
        EXPECT_FALSE(isLanUrl(location)) << location;
        EXPECT_FALSE(parseSsdp(
            std::string("HTTP/1.1 200 OK\r\nST: urn:schemas-upnp-org:device:MediaRenderer:1\r\nUSN: uuid:a\r\nLOCATION: ") +
            location + "\r\n\r\n"))
            << location;
    }
}

TEST(UpnpMessages, ADocumentTypeDefinitionIsRefused) {
    EXPECT_FALSE(parseXml(R"(<!DOCTYPE a [<!ENTITY b "c">]><a>&b;</a>)"));
}

TEST(UpnpMessages, BuildsAndReadsSoap) {
    const auto request = soapRequest("urn:x:service:AVTransport:1", "Seek", {{"Target", "a<b&c"}});
    EXPECT_NE(request.find("<Target>a&lt;b&amp;c</Target>"), std::string::npos);
    const auto parsed = parseXml(request);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->child("Body")->child("Seek")->childText("Target"), "a<b&c");

    const auto fault = parseSoapReply(
        500,
        R"(<s:Envelope xmlns:s="x"><s:Body><s:Fault><detail><UPnPError><errorCode>718</errorCode></UPnPError></detail></s:Fault></s:Body></s:Envelope>)");
    EXPECT_FALSE(fault.ok);
    EXPECT_EQ(fault.errorCode, 718);
    const auto good =
        parseSoapReply(200, R"(<s:Envelope xmlns:s="x"><s:Body><u:R xmlns:u="y"><Value>5</Value></u:R></s:Body></s:Envelope>)");
    EXPECT_TRUE(good.ok);
    EXPECT_EQ(good.values.at("Value"), "5");
}

TEST(UpnpMessages, OnlyTheMasterChannelOfAnEventIsRead) {
    const auto values = upnp::parseLastChange(
        R"(<Event><InstanceID val="0"><Volume channel="Master" val="40"/><Volume channel="LF" val="90"/><Volume Channel="RF" val="80"/><Mute channel="Master" val="1"/></InstanceID></Event>)");
    EXPECT_EQ(values.at("Volume"), "40");
    EXPECT_EQ(values.at("Mute"), "1");
}

TEST(UpnpMessages, ReadsTimesAsTheyAreWritten) {
    EXPECT_EQ(parseDuration("0:01:30"), 90000U);
    EXPECT_EQ(parseDuration("01:00:00.500"), 3600500U);
    EXPECT_EQ(parseDuration("0:00:01.5"), 1500U);
    EXPECT_FALSE(parseDuration("NOT_IMPLEMENTED"));
    EXPECT_EQ(formatDuration(3661000), "1:01:01");
}

TEST(FirewallTest, TheRuleIsForThisProgramOnPrivateNetworksOnly) {
    const auto arguments = firewallRuleArguments(L"C:\\Program Files\\Camel\\CamelPlayer.exe");
    EXPECT_NE(arguments.find(L"dir=in"), std::wstring::npos);
    EXPECT_NE(arguments.find(L"action=allow"), std::wstring::npos);
    EXPECT_NE(arguments.find(L"profile=private"), std::wstring::npos);
    EXPECT_NE(arguments.find(L"program=\"C:\\Program Files\\Camel\\CamelPlayer.exe\""), std::wstring::npos);
    EXPECT_EQ(arguments.find(L"profile=public"), std::wstring::npos);
}

// Started elevated, so it is never looked for where others could have put one of that name, the current directory first.
TEST(FirewallTest, NetshIsStartedFromTheSystemDirectory) {
    wchar_t system[MAX_PATH];
    const UINT length = GetSystemDirectoryW(system, MAX_PATH);
    ASSERT_GT(length, 0U);
    EXPECT_EQ(firewallTool(), std::wstring(system, length) + L"\\netsh.exe");
    EXPECT_EQ(GetFileAttributesW(firewallTool().c_str()) & FILE_ATTRIBUTE_DIRECTORY, 0U) << "it is there";
}

TEST(PlayerFollowerTest, FollowsThePlayersOwnPausePlayAndStop) {
    PlayerFollower follower;
    follower.expect({"a", "b", ""});
    PlayerObservation seen;
    seen.state = TransportState::Playing;
    EXPECT_EQ(follower.observe(seen, 0ms), (std::vector<PlayerEvent>{{PlayerEventKind::Playing}}));
    EXPECT_TRUE(follower.observe(seen, 10ms).empty());
    seen.state = TransportState::Paused;
    EXPECT_EQ(follower.observe(seen, 20ms), (std::vector<PlayerEvent>{{PlayerEventKind::Paused}}));
    seen.state = TransportState::Stopped;
    EXPECT_EQ(follower.observe(seen, 30ms), (std::vector<PlayerEvent>{{PlayerEventKind::Stopped}}));
}

TEST(PlayerFollowerTest, ReportsASeekOnlyWhenThePositionJumps) {
    PlayerFollower follower;
    follower.expect({"a", "", ""});
    PlayerObservation seen;
    seen.state = TransportState::Playing;
    seen.positionMs = 10000;
    EXPECT_EQ(follower.observe(seen, 0ms).size(), 1U); // Playing
    seen.positionMs = 15000;
    EXPECT_TRUE(follower.observe(seen, 5000ms).empty());
    seen.positionMs = 60000;
    EXPECT_EQ(follower.observe(seen, 6000ms), (std::vector<PlayerEvent>{{PlayerEventKind::Seeked, 60000}}));
    seen.positionMs = 5000;
    EXPECT_EQ(follower.observe(seen, 7000ms), (std::vector<PlayerEvent>{{PlayerEventKind::Seeked, 5000}}));
}

TEST(PlayerFollowerTest, AStoppedPlayersPositionIsNotASeek) {
    PlayerFollower follower;
    follower.expect({"a", "", ""});
    PlayerObservation seen;
    seen.state = TransportState::Playing;
    seen.positionMs = 1000;
    follower.observe(seen, 0ms);
    seen.state = TransportState::Stopped; // a Linkplay module then reports the track's length
    seen.positionMs = 211000;
    const auto events = follower.observe(seen, 100ms);
    EXPECT_EQ(events, (std::vector<PlayerEvent>{{PlayerEventKind::Stopped}}));
    seen.state = TransportState::Playing;
    seen.positionMs = 0;
    EXPECT_EQ(follower.observe(seen, 200ms), (std::vector<PlayerEvent>{{PlayerEventKind::Playing}}));
}

TEST(PlayerFollowerTest, ThePositionRightAfterANewTrackIsNotASeek) {
    PlayerFollower follower;
    follower.expectLoaded({"a", "", ""}, 0ms);
    PlayerObservation seen;
    seen.state = TransportState::Playing;
    seen.positionMs = 200000; // of what it played before
    follower.observe(seen, 100ms);
    seen.positionMs = 0;
    EXPECT_TRUE(follower.observe(seen, 200ms).empty());
}

TEST(PlayerFollowerTest, ASeekAskedByCamelPlayerIsNotReportedBack) {
    PlayerFollower follower;
    follower.expect({"a", "", ""});
    PlayerObservation seen;
    seen.state = TransportState::Paused;
    seen.positionMs = 10000;
    follower.observe(seen, 0ms);
    follower.expectPosition(90000, 100ms);
    seen.positionMs = 90000;
    EXPECT_TRUE(follower.observe(seen, 200ms).empty());
}

TEST(PlayerFollowerTest, MovesThroughNextAndPrevious) {
    PlayerFollower follower;
    follower.expect({"a", "b", "z"});
    PlayerObservation seen;
    seen.track = "b";
    EXPECT_EQ(follower.observe(seen, 0ms), (std::vector<PlayerEvent>{{PlayerEventKind::NextTrack}}));
    seen.track = "a";
    EXPECT_EQ(follower.observe(seen, 10ms), (std::vector<PlayerEvent>{{PlayerEventKind::PreviousTrack}}));
    EXPECT_TRUE(follower.observe(seen, 20ms).empty());
}

TEST(PlayerFollowerTest, AfterAdvancingTheOldTrackIsNoLongerNewsAndTheNextStillIs) {
    PlayerFollower follower;
    follower.expect({"a", "b", ""});
    follower.advance();
    PlayerObservation seen;
    seen.track = "a"; // the DMP-A6 reports it long after the change
    EXPECT_TRUE(follower.observe(seen, 0ms).empty());
    seen.track = "b";
    EXPECT_TRUE(follower.observe(seen, 10ms).empty());
    follower.expectNext("c");
    seen.track = "c";
    EXPECT_EQ(follower.observe(seen, 20ms), (std::vector<PlayerEvent>{{PlayerEventKind::NextTrack}}));
    seen.track = "other";
    EXPECT_EQ(follower.observe(seen, 30ms), (std::vector<PlayerEvent>{{PlayerEventKind::TakenOver}}));
}

// A player just given a track can still report the source it was on, as it can the track it played.
TEST(PlayerFollowerTest, TheSourceAPlayerWasOnIsNoTakeoverWhileItSettles) {
    PlayerFollower follower;
    follower.expectLoaded({"a", "", ""}, 0ms);
    PlayerObservation seen;
    seen.otherSource = true;
    EXPECT_TRUE(follower.observe(seen, 100ms).empty());
    EXPECT_EQ(follower.observe(seen, 3100ms), (std::vector<PlayerEvent>{{PlayerEventKind::TakenOver}}));
}

TEST(PlayerFollowerTest, AnotherTrackIsATakeoverAndThenItIsSilent) {
    PlayerFollower follower;
    follower.expect({"a", "b", ""});
    PlayerObservation seen;
    seen.track = "spotify:track";
    EXPECT_EQ(follower.observe(seen, 0ms), (std::vector<PlayerEvent>{{PlayerEventKind::TakenOver}}));
    seen.state = TransportState::Playing;
    EXPECT_TRUE(follower.observe(seen, 10ms).empty());
    EXPECT_TRUE(follower.lost().empty());

    follower.expect({"c", "", ""});
    EXPECT_EQ(follower.lost(), (std::vector<PlayerEvent>{{PlayerEventKind::Lost}}));
}

TEST(PlayerFollowerTest, BeforeAnythingIsExpectedNoTrackIsATakeover) {
    PlayerFollower follower;
    PlayerObservation seen;
    seen.track = "what it played before";
    EXPECT_TRUE(follower.observe(seen, 0ms).empty());
}

TEST(PlayerFinderTest, FindsAPlayerThroughSsdpAndKnowsItsKind) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    std::atomic<int> changes = 0;
    PlayerFinder finder(optionsFor(renderer), [&] { ++changes; });
    ASSERT_TRUE(waitFor([&] { return finder.players().size() == 1; }));
    const auto player = finder.players().front();
    EXPECT_EQ(player.description.udn, "uuid:fake-renderer");
    EXPECT_EQ(player.description.name, "Fake Player");
    EXPECT_EQ(player.location, renderer.location());
    EXPECT_GE(changes.load(), 1);
}

TEST(PlayerFinderTest, FindsAnOpenHomePlayer) {
    FakeRenderer renderer(RendererKind::OpenHome);
    PlayerFinder finder(optionsFor(renderer), {});
    ASSERT_TRUE(waitFor([&] { return finder.players().size() == 1; }));
    EXPECT_EQ(finder.players().front().description.protocol, PlayerProtocol::OpenHome);
}

TEST(PlayerFinderTest, APlayerWithSeveralAddressesIsListedOnce) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.answerAlsoFrom("http://127.0.0.1:1/unreachable.xml");
    PlayerFinder finder(optionsFor(renderer), {});
    ASSERT_TRUE(waitFor([&] { return !finder.players().empty(); }));
    ASSERT_TRUE(waitFor([&] { return renderer.searches() >= 6; }));
    ASSERT_EQ(finder.players().size(), 1U);
    EXPECT_EQ(finder.players().front().location, renderer.location());
}

TEST(PlayerFinderTest, ByeByeForgetsAPlayerAndAliveFindsItAgain) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.goQuiet();
    PlayerFinder finder(optionsFor(renderer, 60s), {});
    renderer.announce(finder.notifyPort(), "ssdp:alive");
    ASSERT_TRUE(waitFor([&] { return finder.players().size() == 1; }));
    renderer.announce(finder.notifyPort(), "ssdp:byebye");
    ASSERT_TRUE(waitFor([&] { return finder.players().empty(); }));
    renderer.announce(finder.notifyPort(), "ssdp:alive");
    EXPECT_TRUE(waitFor([&] { return finder.players().size() == 1; }));
}

TEST(PlayerFinderTest, APlayerThatDisappearsIsForgotten) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    PlayerFinder finder(optionsFor(renderer, 50ms), {});
    ASSERT_TRUE(waitFor([&] { return finder.players().size() == 1; }));
    renderer.disappear();
    EXPECT_TRUE(waitFor([&] { return finder.players().empty(); }));
}

TEST(PlayerFinderTest, APlayerThatOnlyStoppedAnsweringSearchesStays) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    PlayerFinder finder(optionsFor(renderer, 30ms), {});
    ASSERT_TRUE(waitFor([&] { return finder.players().size() == 1; }));
    renderer.goQuiet();
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(finder.players().size(), 1U);
}

TEST(PlayerFinderTest, APlayerThatComesBackAtANewAddressIsTheSamePlayer) {
    auto first = std::make_unique<FakeRenderer>(RendererKind::UpnpAv);
    FakeRenderer quiet(RendererKind::UpnpAv, "uuid:other");
    FinderOptions options = optionsFor(*first, 60s);
    PlayerFinder finder(options, {});
    ASSERT_TRUE(waitFor([&] { return finder.players().size() == 1; }));
    const auto before = finder.players().front();
    first.reset();

    FakeRenderer moved(RendererKind::UpnpAv);
    moved.goQuiet();
    moved.announce(finder.notifyPort(), "ssdp:alive");
    ASSERT_TRUE(waitFor([&] { return finder.players().front().location == moved.location(); }));
    ASSERT_EQ(finder.players().size(), 1U);
    EXPECT_EQ(finder.players().front().description.udn, before.description.udn);
    EXPECT_NE(moved.location(), before.location);
}

TEST(PlayerFinderTest, ADescriptionWithAnotherUdnIsNotBelieved) {
    FakeRenderer renderer(RendererKind::UpnpAv, "uuid:real");
    renderer.goQuiet();
    PlayerFinder finder(optionsFor(renderer, 60s), {});
    const SOCKET out = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    target.sin_port = htons(finder.notifyPort());
    const std::string text = "NOTIFY * HTTP/1.1\r\nNT: urn:schemas-upnp-org:device:MediaRenderer:1\r\nNTS: ssdp:alive\r\nUSN: "
                             "uuid:forged::x\r\nLOCATION: " +
                             renderer.location() + "\r\n\r\n";
    sendto(out, text.data(), int(text.size()), 0, reinterpret_cast<sockaddr*>(&target), sizeof(target));
    closesocket(out);
    ASSERT_TRUE(waitFor([&] { return !renderer.requests().empty(); }));
    std::this_thread::sleep_for(100ms);
    EXPECT_TRUE(finder.players().empty());
}

TEST(PlayerLinkTest, SubscribesAndEndsItsSubscriptionWhenClosed) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    {
        auto events = std::make_shared<Events>();
        PlayerLink link(describe(renderer), into(events), kQuick);
        EXPECT_EQ(renderer.subscribers(), 1U);
    }
    EXPECT_EQ(renderer.subscribers(), 0U);
}

TEST(PlayerLinkTest, RefusedByAPlayerThatDoesNotAnswer) {
    auto description = [] {
        FakeRenderer renderer(RendererKind::UpnpAv);
        return describe(renderer);
    }();
    auto events = std::make_shared<Events>();
    EXPECT_THROW(PlayerLink(description, into(events), kQuick), std::runtime_error);
}

TEST(PlayerLinkTest, FollowsThePlayersOwnPauseAndPlayThroughEvents) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.set("track", "http://camel/a");
    renderer.set("state", "PLAYING");
    auto events = std::make_shared<Events>();
    PlayerLink link(describe(renderer), into(events), {60s, 2, 1800, 500});
    link.expect({"http://camel/a", "http://camel/b", ""});
    ASSERT_TRUE(events->waitFor(PlayerEventKind::Playing));
    renderer.emitLastChange(R"(<TransportState val="PAUSED_PLAYBACK"/>)");
    EXPECT_TRUE(events->waitFor(PlayerEventKind::Paused));
}

TEST(PlayerLinkTest, FollowsASeekFromThePlayersRemoteThroughThePoll) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.set("track", "http://camel/a");
    renderer.set("state", "PLAYING");
    renderer.set("time", "0:00:10");
    auto events = std::make_shared<Events>();
    PlayerLink link(describe(renderer), into(events), kQuick);
    link.expect({"http://camel/a", "", ""});
    ASSERT_TRUE(events->waitFor(PlayerEventKind::Playing));
    std::this_thread::sleep_for(150ms);
    renderer.set("time", "0:02:00");
    ASSERT_TRUE(events->waitFor(PlayerEventKind::Seeked));
    EXPECT_EQ(events->find(PlayerEventKind::Seeked)->positionMs, 120000U);
}

TEST(PlayerLinkTest, FollowsNextAndPreviousFromThePlayersRemote) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.set("track", "http://camel/a");
    auto events = std::make_shared<Events>();
    PlayerLink link(describe(renderer), into(events), {60s, 2, 1800, 500});
    link.expect({"http://camel/a", "http://camel/b", "http://camel/z"});
    renderer.emitLastChange(R"(<CurrentTrackURI val="http://camel/b"/>)");
    ASSERT_TRUE(events->waitFor(PlayerEventKind::NextTrack));
    renderer.emitLastChange(R"(<CurrentTrackURI val="http://camel/a"/>)");
    EXPECT_TRUE(events->waitFor(PlayerEventKind::PreviousTrack));
}

TEST(PlayerLinkTest, AnotherSourceTakingAUpnpPlayerOverEndsTheLink) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.set("track", "http://camel/a");
    auto events = std::make_shared<Events>();
    PlayerLink link(describe(renderer), into(events), {60s, 2, 1800, 500});
    link.expect({"http://camel/a", "", ""});
    renderer.emitLastChange(R"(<CurrentTrackURI val="http://other-app/song"/><TransportState val="PLAYING"/>)");
    ASSERT_TRUE(events->waitFor(PlayerEventKind::TakenOver));
    renderer.emitLastChange(R"(<TransportState val="PAUSED_PLAYBACK"/>)");
    std::this_thread::sleep_for(100ms);
    EXPECT_FALSE(events->saw(PlayerEventKind::Paused));
}

TEST(PlayerLinkTest, APlayerThatDisappearsIsReportedLostOnce) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    auto events = std::make_shared<Events>();
    PlayerLink link(describe(renderer), into(events), kQuick);
    link.expect({"http://camel/a", "", ""});
    renderer.disappear();
    ASSERT_TRUE(events->waitFor(PlayerEventKind::Lost));
    std::this_thread::sleep_for(150ms);
    EXPECT_EQ(events->count(PlayerEventKind::Lost), 1U);
}

TEST(PlayerLinkTest, RenewsSubscriptionsBeforeTheyRunOut) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.grantTimeout("Second-2");
    auto events = std::make_shared<Events>();
    PlayerLink link(describe(renderer), into(events), kQuick);
    const auto subscribes = [&] {
        const auto requests = renderer.requests();
        return std::ranges::count(requests, std::string("SUBSCRIBE /evt/AVTransport"));
    };
    EXPECT_TRUE(waitFor([&] { return subscribes() >= 2; }, 4s));
}

TEST(PlayerLinkTest, SubscribesAgainWhenThePlayerForgotAfterARestart) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.grantTimeout("Second-2");
    auto events = std::make_shared<Events>();
    PlayerLink link(describe(renderer), into(events), kQuick);
    renderer.forgetSubscriptions();
    EXPECT_EQ(renderer.subscribers(), 0U);
    EXPECT_TRUE(waitFor([&] { return renderer.subscribers() == 1; }, 4s));
    EXPECT_FALSE(events->saw(PlayerEventKind::Lost));
}

TEST(PlayerLinkTest, CommandsReachTheTransport) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    auto events = std::make_shared<Events>();
    PlayerLink link(describe(renderer), into(events), {60s, 2, 1800, 500});
    EXPECT_TRUE(link.play());
    EXPECT_TRUE(link.pause());
    EXPECT_TRUE(link.seek(61000));
    EXPECT_TRUE(link.stop());
    const auto actions = renderer.actions();
    for (const char* action : {"Play", "Pause", "Seek", "Stop"})
        EXPECT_NE(std::ranges::find(actions, action), actions.end()) << action;
    renderer.refuseSoap(true);
    EXPECT_FALSE(link.play());
}

TEST(PlayerLinkTest, FollowsAnOpenHomePlayer) {
    FakeRenderer renderer(RendererKind::OpenHome);
    renderer.set("track", "7");
    renderer.set("state", "Playing");
    auto events = std::make_shared<Events>();
    PlayerLink link(describe(renderer), into(events), {60s, 2, 1800, 500});
    EXPECT_EQ(renderer.subscribers(), 3U);
    link.expect({"7", "8", "6"});
    ASSERT_TRUE(events->waitFor(PlayerEventKind::Playing));
    renderer.emitVariable("Playlist", "TransportState", "Paused");
    ASSERT_TRUE(events->waitFor(PlayerEventKind::Paused));
    renderer.emitVariable("Playlist", "Id", "8");
    ASSERT_TRUE(events->waitFor(PlayerEventKind::NextTrack));
    renderer.emitVariable("Playlist", "Id", "7");
    ASSERT_TRUE(events->waitFor(PlayerEventKind::PreviousTrack));
    renderer.emitVariable("Time", "Seconds", "10");
    renderer.emitVariable("Time", "Seconds", "200");
    ASSERT_TRUE(events->waitFor(PlayerEventKind::Seeked));
    EXPECT_EQ(events->find(PlayerEventKind::Seeked)->positionMs, 200000U);
}

// A connection reset before it is read, as a port probe's, leaves the player's events coming.
TEST(PlayerLinkTest, AConnectionResetBeforeItIsReadLeavesEventsComing) {
    FakeRenderer renderer(RendererKind::OpenHome);
    renderer.set("track", "7");
    renderer.set("state", "Playing");
    auto events = std::make_shared<Events>();
    PlayerLink link(describe(renderer), into(events), {60s, 2, 1800, 500});
    ASSERT_TRUE(waitFor([&] { return renderer.subscribers() == 3; }));
    link.expect({"7", "8", "6"});
    ASSERT_TRUE(events->waitFor(PlayerEventKind::Playing));
    for (int i = 0; i < 20; ++i) FakeRenderer::resetConnectionTo(renderer.callback("Playlist"));
    renderer.emitVariable("Playlist", "TransportState", "Paused");
    EXPECT_TRUE(events->waitFor(PlayerEventKind::Paused));
}

TEST(PlayerLinkTest, ASourceChangeOnAnOpenHomePlayerIsATakeover) {
    FakeRenderer renderer(RendererKind::OpenHome);
    auto events = std::make_shared<Events>();
    PlayerLink link(describe(renderer), into(events), {60s, 2, 1800, 500});
    renderer.emitVariable("Product", "SourceIndex", "1");
    ASSERT_TRUE(waitFor([&] { return renderer.subscribers() == 3; }));
    std::this_thread::sleep_for(100ms);
    link.expect({"7", "", ""});
    renderer.emitVariable("Product", "SourceIndex", "3");
    EXPECT_TRUE(events->waitFor(PlayerEventKind::TakenOver));
}

TEST(PlayerLinkTest, OpenHomeCommandsUseThePlaylist) {
    FakeRenderer renderer(RendererKind::OpenHome);
    auto events = std::make_shared<Events>();
    PlayerLink link(describe(renderer), into(events), {60s, 2, 1800, 500});
    EXPECT_TRUE(link.play());
    EXPECT_TRUE(link.seek(30000));
    const auto actions = renderer.actions();
    EXPECT_NE(std::ranges::find(actions, "SeekSecondAbsolute"), actions.end());
}

TEST(PlayerLinkTest, AForgedEventIsIgnored) {
    FakeRenderer renderer(RendererKind::UpnpAv);
    renderer.set("track", "http://camel/a");
    auto events = std::make_shared<Events>();
    PlayerLink link(describe(renderer), into(events), {60s, 2, 1800, 500});
    link.expect({"http://camel/a", "", ""});
    FakeRenderer stranger(RendererKind::UpnpAv);
    stranger.emitLastChange(R"(<TransportState val="PAUSED_PLAYBACK"/>)");
    std::this_thread::sleep_for(100ms);
    EXPECT_FALSE(events->saw(PlayerEventKind::Paused));
}

// Reads what the players on this network offer and what they are doing, and sends them nothing else.
TEST(NetworkPlayerHardwareTest, DISABLED_ReadsThePlayersOnThisNetwork) {
    PlayerFinder finder(FinderOptions{"239.255.255.250", 1900, 0}, {});
    std::this_thread::sleep_for(6s);
    const auto players = finder.players();
    ASSERT_FALSE(players.empty());
    for (const auto& player : players) {
        const auto& d = player.description;
        printf("%s (%s %s) %s %s\n", d.name.c_str(), d.manufacturer.c_str(), d.model.c_str(), d.udn.c_str(),
               d.protocol == PlayerProtocol::OpenHome ? "OpenHome" : "UPnP AV");
        for (const auto& service : d.services) printf("  %s\n", service.type.c_str());
        auto events = std::make_shared<Events>();
        PlayerLink link(d, into(events), {100ms, 2, 300, 2000});
        std::this_thread::sleep_for(1s);
        EXPECT_FALSE(events->saw(PlayerEventKind::Lost)) << d.name;
        printf("  playing=%d paused=%d\n", int(events->saw(PlayerEventKind::Playing)), int(events->saw(PlayerEventKind::Paused)));
    }
}
