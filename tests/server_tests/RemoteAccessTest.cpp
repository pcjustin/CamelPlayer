// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <thread>

#include "Server/Remote/RemoteAccess.h"
#include "Server/Remote/RemoteServer.h"
#include "TestUtil.h"

using namespace camel::server;

namespace {

class RemoteAccessTest : public ::testing::Test {
protected:
    std::filesystem::path dir;
    std::unique_ptr<LocalServer> local;
    std::unique_ptr<RemoteAccess> access;

    void SetUp() override {
        dir = testutil::tempDir() / "remote-access" /
              (std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()) + "-" +
               std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        std::filesystem::create_directories(dir / "data");
    }

    void TearDown() override {
        if (access) access->stop();
        access.reset();
        if (local) local->shutdown();
        local.reset();
    }

    void start(bool allowed) {
        camel::Settings settings;
        settings.allowControl = allowed;
        local = std::make_unique<LocalServer>(settings, dir / "data" / "settings.json");
        RemoteAccess::Options options;
        options.data = dir / "data";
        options.name = "Test PC";
        options.port = 0;
        options.loopbackOnly = true;
        options.announce = false;
        access = std::make_unique<RemoteAccess>(*local, options);
    }

    static void allow(IServer& server, bool allowed) {
        auto settings = server.snapshot().settings;
        settings.allowControl = allowed;
        Command command;
        command.kind = CommandKind::ApplySettings;
        command.settings = settings;
        server.submit(command);
    }

    static bool within(const std::function<bool()>& done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return done();
    }

    std::unique_ptr<RemoteServer> controlPoint() {
        const auto paired = RemoteServer::pair("127.0.0.1", *access->port(), "Study PC", "");
        RemoteServer::Options options;
        options.host = "127.0.0.1";
        options.port = *access->port();
        options.cache = dir / "cache";
        options.token = paired.token;
        return std::make_unique<RemoteServer>(options);
    }
};

} // namespace

// Off by default: nothing is served until the Server's settings allow it, and turning it off from a Control point
// closes it, that Control point cut off too (docs/Plan.md §17.8).
TEST_F(RemoteAccessTest, TheApiIsServedOnlyWhileControlIsAllowed) {
    start(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_FALSE(access->port());
    EXPECT_THROW(access->pairingCode(), std::runtime_error);

    allow(*local, true);
    ASSERT_TRUE(within([&] { return access->port().has_value(); }));
    const auto remote = controlPoint();
    EXPECT_TRUE(remote->connected());
    EXPECT_EQ(remote->hello()["server"]["id"], access->serverId());
    EXPECT_FALSE(remote->device().empty());
    const auto devices = access->pairedDevices();
    ASSERT_EQ(devices.size(), 1U);
    EXPECT_EQ(devices[0]["name"], "Study PC");
    EXPECT_EQ(devices[0]["id"], remote->device());

    allow(*remote, false);
    EXPECT_TRUE(within([&] { return !remote->connected(); }));
    EXPECT_TRUE(within([&] { return !access->port().has_value(); }));
    EXPECT_FALSE(local->snapshot().settings.allowControl);
}

TEST_F(RemoteAccessTest, AServerStartedWithControlAllowedServesAtOnceAndStopCloses) {
    start(true);
    ASSERT_TRUE(within([&] { return access->port().has_value(); }));
    EXPECT_EQ(access->pairingCode().size(), 6U);
    access->stop();
    EXPECT_FALSE(access->port());
}

// Pairing survives turning control off and on again, as the paired devices are kept in the data folder.
TEST_F(RemoteAccessTest, PairedDevicesAreKeptWhileControlIsOff) {
    start(true);
    ASSERT_TRUE(within([&] { return access->port().has_value(); }));
    controlPoint().reset();
    allow(*local, false);
    ASSERT_TRUE(within([&] { return !access->port().has_value(); }));
    allow(*local, true);
    ASSERT_TRUE(within([&] { return access->port().has_value(); }));
    const auto devices = access->pairedDevices();
    ASSERT_EQ(devices.size(), 1U);
    EXPECT_EQ(devices[0]["name"], "Study PC");
    EXPECT_THROW(RemoteServer::pair("127.0.0.1", *access->port(), "Kitchen PC", ""), PairingRefused);
}

TEST(ServerIdTest, AServerKeepsItsIdInItsDataFolder) {
    const auto dir =
        testutil::tempDir() / ("server-id-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    const auto first = serverIdIn(dir / "a");
    EXPECT_EQ(first.size(), 32U);
    EXPECT_EQ(first.find_first_not_of("0123456789abcdef"), std::string::npos);
    EXPECT_EQ(serverIdIn(dir / "a"), first);
    EXPECT_NE(serverIdIn(dir / "b"), first);
    EXPECT_FALSE(computerName().empty());
}
