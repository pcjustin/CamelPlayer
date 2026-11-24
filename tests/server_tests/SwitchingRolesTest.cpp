// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Switching back and swapping (docs/Plan.md §17.8): two computers over loopback, each with its own data folder, a
// Server and the Server API, one turning into a Control point of the other and back.
#include <gtest/gtest.h>

#include <winsock2.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <thread>

#include "Common/util/Text.h"
#include "OutputBridge/CaptureOutput.h"
#include "Server/Playback/LocalServer.h"
#include "Server/Remote/Pairings.h"
#include "Server/Remote/RemoteAccess.h"
#include "Server/Remote/RemoteServer.h"
#include "Server/Remote/ServerTokens.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::server;
using namespace std::chrono_literals;

namespace {

std::string zone(size_t number) {
    return zoneId(Backend::Wasapi, "zone-" + std::to_string(number));
}

const ZoneSnapshot& of(const Snapshot& snapshot, size_t number) {
    const auto found = std::ranges::find(snapshot.zones, zone(number), &ZoneSnapshot::id);
    if (found == snapshot.zones.end()) throw std::logic_error("no zone " + std::to_string(number));
    return *found;
}

bool within(const std::function<bool()>& done, std::chrono::milliseconds time = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + time * testutil::kSlowdown;
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return done();
}

// A port free now, so that a computer serves on the same one each time it opens, as on 9440.
uint16_t freePort() {
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int length = sizeof(address);
    const bool bound = bind(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
                       getsockname(s, reinterpret_cast<sockaddr*>(&address), &length) == 0;
    closesocket(s);
    WSACleanup();
    return bound ? ntohs(address.sin_port) : 0;
}

std::string codeOf(const std::function<void()>& pairing) {
    try {
        pairing();
    } catch (const PairingRefused& refused) {
        return refused.code;
    }
    return "paired";
}

// A computer that plays: its data folder, its Server with two zones, and the Server API other computers control it by,
// on the same port each time it opens.
struct Computer {
    std::filesystem::path data;
    std::string name;
    uint16_t port = 0;
    std::unique_ptr<LocalServer> local;
    std::unique_ptr<RemoteAccess> access;

    void open() {
        const auto file = data / "settings.json";
        Settings settings;
        if (std::filesystem::exists(file)) {
            settings = loadSettings(file);
        } else {
            settings.autoplay = false;
            settings.allowControl = true;
            settings.wasapi.deviceId = "zone-1";
            for (size_t i = 1; i <= 2; ++i) {
                SavedOutput output;
                output.backend = Backend::Wasapi;
                output.id = output.wasapi.deviceId = "zone-" + std::to_string(i);
                output.name = "Zone " + std::to_string(i);
                output.enabled = true;
                output.zone.autoplay = false;
                settings.outputs.push_back(output);
            }
            settings = fromJson(toJson(settings));
        }
        local = std::make_unique<LocalServer>(settings, file, [this](const Settings& s) {
            audio::CaptureOptions options;
            options.file = data / (s.wasapi.deviceId + ".raw");
            options.accepts = [](const audio::DeviceFormat& f) { return f.type == audio::SampleType::Int32; };
            options.realtime = true;
            return audio::createCaptureOutput(options);
        });
        local->waitIdle();
        RemoteAccess::Options options;
        options.data = data;
        options.name = name;
        if (!port) port = freePort();
        options.port = port;
        options.loopbackOnly = true;
        options.announce = false;
        access = std::make_unique<RemoteAccess>(*local, options);
        ASSERT_TRUE(within([&] { return access->port().has_value(); }));
        port = *access->port();
    }

    // As CamelPlayer restarting into another role does: the Server API closes, then the Server, its queues saved.
    void close() {
        access->stop();
        access.reset();
        local->shutdown();
        local.reset();
    }

    std::string id() const { return serverIdIn(data); }
    ServerTokens tokens() const { return ServerTokens(data / L"server-tokens"); }
};

class SwitchingRolesTest : public ::testing::Test {
protected:
    std::filesystem::path dir;
    Computer a, b;

    void SetUp() override {
        dir = testutil::tempDir() / "switching-roles" /
              (std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()) + "-" +
               std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        for (auto* computer : {&a, &b}) {
            computer->name = computer == &a ? "Study PC" : "Living room";
            computer->data = dir / computer->name;
            std::filesystem::create_directories(computer->data);
            computer->open();
        }
    }

    void TearDown() override {
        for (auto* computer : {&a, &b})
            if (computer->local) computer->close();
    }

    std::unique_ptr<RemoteServer> controlPoint(const Computer& server, const std::string& token,
                                               const std::filesystem::path& ownSettings = {}) const {
        RemoteServer::Options options;
        options.host = "127.0.0.1";
        options.port = server.port;
        options.cache = dir / "cache";
        options.token = token;
        options.ownSettings = ownSettings;
        return std::make_unique<RemoteServer>(options);
    }

    // `from` pairs with `to` as the window's pairing does, introducing itself, and keeps the token.
    static std::string pairWith(const Computer& from, const Computer& to, const std::string& code = {}) {
        const auto paired =
            RemoteServer::pair("127.0.0.1", to.port, from.name, code, RemoteServer::introduction(from.data, to.id()));
        from.tokens().set(to.id(), paired.token);
        return paired.token;
    }

    static void play(IServer& server, size_t number) {
        Command c;
        c.kind = CommandKind::QueueReplace;
        c.zone = zone(number);
        const auto track = testutil::generatedFile({"switching-roles-20s", 44100, 16, 2, 20.0});
        c.sources.emplace_back(toUtf8(track.wstring()));
        c.sources.emplace_back(toUtf8(track.wstring()));
        server.submit(c);
        server.waitIdle();
    }

    static bool playing(const IServer& server, size_t number) {
        const auto& z = of(server.snapshot(), number);
        return z.playback.state == PlaybackState::Playing && z.playback.position > 0;
    }
};

} // namespace

// A computer that turns into a Control point keeps its queues with its library, and finds them paused where they were
// once it plays here again; the computer it controlled meanwhile plays on (docs/Plan.md §17.8).
TEST_F(SwitchingRolesTest, AComputerBackFromControllingAnotherFindsItsQueuesPausedWhereTheyWere) {
    play(*a.local, 1);
    ASSERT_TRUE(within([&] { return playing(*a.local, 1); }));
    ASSERT_TRUE(within([&] { return of(a.local->snapshot(), 1).playback.position > 13230; }));
    const auto before = of(a.local->snapshot(), 1);

    a.close();
    {
        const auto point = controlPoint(b, pairWith(a, b), a.data / "settings.json");
        play(*point, 2);
        EXPECT_TRUE(within([&] { return playing(*b.local, 2); }));
    }
    a.open();

    const auto after = of(a.local->snapshot(), 1);
    ASSERT_EQ(after.queue.entries.size(), before.queue.entries.size());
    for (size_t i = 0; i < after.queue.entries.size(); ++i)
        EXPECT_EQ(after.queue.entries[i].source.pathUtf8(), before.queue.entries[i].source.pathUtf8());
    EXPECT_EQ(after.queueEntryId, before.queueEntryId);
    EXPECT_EQ(after.playback.state, PlaybackState::Paused);
    EXPECT_GE(after.playback.position + 4410, before.playback.position);
    EXPECT_TRUE(playing(*b.local, 2));
}

// Two computers that paired trust each other both ways: once the one that controlled turns into the one controlled,
// the other pairs with it without a code, which a computer without that proof is still asked for.
TEST_F(SwitchingRolesTest, TwoComputersThatPairedSwapWithoutACode) {
    RemoteServer::pair("127.0.0.1", a.port, "Laptop", "");
    pairWith(a, b);

    EXPECT_EQ(codeOf([&] { RemoteServer::pair("127.0.0.1", a.port, b.name, ""); }), "codeNeeded");
    EXPECT_EQ(codeOf([&] { RemoteServer::pair("127.0.0.1", a.port, "Stranger", "", {b.id(), std::string(64, '0')}); }),
              "codeNeeded");
    EXPECT_EQ(codeOf([&] { RemoteServer::pair("127.0.0.1", a.port, "Stranger", "", {"0123", Pairings::proofOf("x")}); }),
              "codeNeeded");

    std::string token;
    EXPECT_EQ(codeOf([&] { token = pairWith(b, a); }), "paired");
    const auto point = controlPoint(a, token);
    EXPECT_EQ(point->serverName(), a.name);
    const auto devices = a.access->pairedDevices();
    EXPECT_TRUE(std::ranges::any_of(devices, [&](const auto& d) { return d.value("name", std::string()) == b.name; }));
}

// The trust ends with the pairing: a computer the other revoked, or whose token it no longer keeps, needs a code.
TEST_F(SwitchingRolesTest, ARevokedOrForgottenPairingNoLongerSwapsWithoutACode) {
    RemoteServer::pair("127.0.0.1", a.port, "Laptop", "");
    const auto token = pairWith(a, b);
    a.tokens().forget(b.id());
    EXPECT_EQ(codeOf([&] { pairWith(b, a); }), "codeNeeded");

    a.tokens().set(b.id(), token);
    for (const auto& device : b.access->pairedDevices())
        if (device.value("name", std::string()) == a.name) b.access->revokeDevice(device.value("id", std::string()));
    EXPECT_EQ(codeOf([&] { pairWith(b, a); }), "codeNeeded");
}

// The other Control points of a computer that swapped are cut off from it, and pair with the new Server as any device
// does, with a code a paired device shows.
TEST_F(SwitchingRolesTest, AnotherControlPointPairsWithTheNewServerByACode) {
    RemoteServer::pair("127.0.0.1", a.port, "Laptop", "");
    pairWith(a, b);
    const auto onB = controlPoint(b, RemoteServer::pair("127.0.0.1", b.port, "Phone", b.access->pairingCode()).token);

    b.close();
    EXPECT_TRUE(within([&] { return onB->link() != IServer::Link::Connected; }));
    pairWith(b, a);

    EXPECT_EQ(codeOf([&] { RemoteServer::pair("127.0.0.1", a.port, "Phone", ""); }), "codeNeeded");
    const auto paired = RemoteServer::pair("127.0.0.1", a.port, "Phone", a.access->pairingCode());
    EXPECT_NO_THROW(controlPoint(a, paired.token));
}

// Turning into a Control point names the Control points it cuts off, those connected now, one listed but not connected
// left out; the caller itself may be left out.
TEST_F(SwitchingRolesTest, TheControlPointsCutOffAreTheOnesConnectedNow) {
    const auto study = RemoteServer::pair("127.0.0.1", a.port, "Desk", "");
    const auto laptop = RemoteServer::pair("127.0.0.1", a.port, "Laptop", a.access->pairingCode());
    RemoteServer::pair("127.0.0.1", a.port, "Phone", a.access->pairingCode());
    const auto desk = controlPoint(a, study.token);
    const auto lap = controlPoint(a, laptop.token);

    std::vector<std::string> connected;
    ASSERT_TRUE(within([&] {
        connected = connectedNames(a.access->pairedDevices());
        return connected.size() == 2;
    }));
    std::ranges::sort(connected);
    EXPECT_EQ(connected, (std::vector<std::string>{"Desk", "Laptop"}));
    EXPECT_EQ(connectedNames(a.access->pairedDevices(), study.device), std::vector<std::string>{"Laptop"});
}

// A computer in the Control point role runs no Server API: nothing answers there, and those that controlled it lose it.
TEST_F(SwitchingRolesTest, AComputerTurnedIntoAControlPointCannotBeControlled) {
    const auto point = controlPoint(a, RemoteServer::pair("127.0.0.1", a.port, "Laptop", "").token);
    a.close();
    EXPECT_TRUE(within([&] { return point->link() != IServer::Link::Connected; }));
    EXPECT_THROW(RemoteServer::pair("127.0.0.1", a.port, "Phone", "", {}, 1s), std::runtime_error);
    EXPECT_THROW(controlPoint(a, "no token"), std::runtime_error);
}

// The name chosen in Settings is the one served: the Control points connect again and learn it.
TEST_F(SwitchingRolesTest, TheNameChosenIsServedAndTheControlPointsLearnIt) {
    const auto point = controlPoint(a, RemoteServer::pair("127.0.0.1", a.port, "Laptop", "").token);
    EXPECT_EQ(point->serverName(), "Study PC");
    auto settings = a.local->snapshot().settings;
    settings.ownName = "Office";
    Command apply;
    apply.kind = CommandKind::ApplySettings;
    apply.settings = settings;
    a.local->submit(apply);
    EXPECT_TRUE(within([&] { return point->serverName() == "Office"; }, 20s));
    EXPECT_EQ(point->link(), IServer::Link::Connected);
}
