// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include <gtest/gtest.h>

#include <winsock2.h>

#include <algorithm>
#include <atomic>
#include <string>

#include "Server/Network/AirPlay.h"
#include "Server/Remote/Discovery.h"

using namespace camel::server;

TEST(Discovery, AnnouncesTheServerAsDnsSdRecords) {
    const ServerAnnouncement announced{"server-1", "Study PC", 9440, 2, 3};
    const auto message = discovery::responseMessage(announced, "STUDY", {"192.168.0.10", "10.0.0.5"}, 120);
    const auto found = discovery::serversIn({message});
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].announced.id, "server-1");
    EXPECT_EQ(found[0].announced.name, "Study PC");
    EXPECT_EQ(found[0].announced.port, 9440);
    EXPECT_EQ(found[0].announced.apiMin, 2);
    EXPECT_EQ(found[0].announced.apiMax, 3);
    EXPECT_EQ(found[0].address, "192.168.0.10");
    // The same Server announced twice is one.
    EXPECT_EQ(discovery::serversIn({message, message}).size(), 1u);
    // Damaged or foreign messages find nothing.
    EXPECT_TRUE(discovery::serversIn({message.substr(0, 40), "garbage", ""}).empty());
}

TEST(Discovery, AnswersQuestionsForTheServiceItsInstanceAndItsHost) {
    using namespace camel::server::airplay;
    const auto service = queryMessage({{serviceName("_camelplayer._tcp"), DnsType::Ptr}});
    std::string question;
    EXPECT_TRUE(discovery::asksFor(service, "STUDY", &question));
    EXPECT_FALSE(question.empty());
    EXPECT_TRUE(discovery::asksFor(queryMessage({{{"Study PC", "_camelplayer", "_tcp", "local"}, DnsType::Srv}}), "STUDY"));
    EXPECT_TRUE(discovery::asksFor(queryMessage({{{"study", "local"}, DnsType::A}}), "STUDY"));
    EXPECT_FALSE(discovery::asksFor(queryMessage({{serviceName("_raop._tcp"), DnsType::Ptr}}), "STUDY"));
    EXPECT_FALSE(discovery::asksFor("", "STUDY"));
    std::string truncated(12, '\0');
    truncated[5] = 1; // one question, whose name is cut short
    EXPECT_FALSE(discovery::asksFor(truncated + char(5) + "abc", "STUDY"));
    // A response is no question, even for the service.
    EXPECT_FALSE(discovery::asksFor(discovery::responseMessage({"x", "y", 1}, "h", {"10.0.0.1"}, 1), "h"));
    // A legacy query gets its ID and question back.
    const auto reply = discovery::responseMessage({"x", "y", 9440}, "h", {"10.0.0.1"}, 10, 0x1234, question);
    EXPECT_EQ(uint8_t(reply[0]), 0x12);
    EXPECT_EQ(uint8_t(reply[5]), 1); // one question
    EXPECT_EQ(discovery::serversIn({reply}).size(), 1u);
}

// The announcer on this computer is found through multicast, as a Control point on the LAN finds it.
TEST(Discovery, AServerOnThisComputerIsFound) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    ServerAnnouncer announcer({"discovery-test-" + std::to_string(GetCurrentProcessId()), "Test Server", 9440});
    std::vector<FoundServer> found;
    for (int attempt = 0; attempt < 3; ++attempt) {
        found = findServers(std::chrono::milliseconds(800));
        if (std::ranges::any_of(found, [](const FoundServer& f) { return f.announced.name == "Test Server"; })) break;
    }
    const auto ours = std::ranges::find(found, "Test Server", [](const FoundServer& f) { return f.announced.name; });
    ASSERT_NE(ours, found.end());
    EXPECT_EQ(ours->announced.port, 9440);
    WSACleanup();
}

// A network that comes after the Server started, as Wi-Fi joined and the wired adapter unplugged, finds it too.
TEST(Discovery, AServerIsFoundOnANetworkThatCameAfterItStarted) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    std::atomic<bool> networkUp = false;
    ServerAnnouncer announcer({"discovery-late-" + std::to_string(GetCurrentProcessId()), "Late Server", 9441},
                              [&] { return networkUp ? lanAddresses() : std::vector<std::string>{}; });
    networkUp = true;
    std::vector<FoundServer> found;
    for (int attempt = 0; attempt < 8; ++attempt) {
        found = findServers(std::chrono::milliseconds(800));
        if (std::ranges::any_of(found, [](const FoundServer& f) { return f.announced.name == "Late Server"; })) break;
    }
    EXPECT_TRUE(std::ranges::any_of(found, [](const FoundServer& f) { return f.announced.name == "Late Server"; }));
    WSACleanup();
}
