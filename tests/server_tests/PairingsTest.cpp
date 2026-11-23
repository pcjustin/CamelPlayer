// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include <gtest/gtest.h>

#include <windows.h>

#include <filesystem>
#include <fstream>

#include "Server/Remote/Pairings.h"
#include "Server/Remote/ServerTokens.h"
#include "TestUtil.h"

using namespace camel::server;

namespace {
constexpr int64_t kNow = 1'800'000'000'000;
}

// TC-R02: an ownerless Server is claimed without a code; every later device gives a code a paired device was shown,
// once, within five minutes.
TEST(Pairings, TheFirstClaimsTheServerAndLaterOnesNeedACode) {
    Pairings pairings({});
    EXPECT_FALSE(pairings.owned());
    const auto first = pairings.pair("Study PC", "", kNow);
    ASSERT_FALSE(first.token.empty()) << first.error;
    EXPECT_TRUE(pairings.owned());
    EXPECT_EQ(pairings.deviceOf(first.token)->name, "Study PC");

    EXPECT_EQ(pairings.pair("Laptop", "", kNow).error, "codeNeeded");
    EXPECT_EQ(pairings.pair("Laptop", "123456", kNow).error, "wrongCode");
    const auto code = pairings.newCode(kNow);
    EXPECT_EQ(code.size(), 6u);
    const auto second = pairings.pair("Laptop", code, kNow + 1000);
    ASSERT_FALSE(second.token.empty()) << second.error;
    EXPECT_NE(second.token, first.token);
    // A code pairs once.
    EXPECT_EQ(pairings.pair("Tablet", code, kNow + 2000).error, "wrongCode");
    // A code is good for five minutes.
    const auto late = pairings.newCode(kNow);
    EXPECT_EQ(pairings.pair("Tablet", late, kNow + Pairings::kCodeLifetimeMs + 1).error, "wrongCode");
    EXPECT_EQ(pairings.devices().size(), 2u);
}

// Five wrong codes stop pairing for five minutes, the right code included; the fifth says so, so the paired devices
// are told.
TEST(Pairings, FiveWrongCodesStopPairingForFiveMinutes) {
    Pairings pairings({});
    ASSERT_FALSE(pairings.pair("Study PC", "", kNow).token.empty());
    const auto code = pairings.newCode(kNow);
    for (int i = 1; i < Pairings::kWrongCodes; ++i) EXPECT_FALSE(pairings.pair("Laptop", "000000", kNow).stoppedNow);
    const auto fifth = pairings.pair("Laptop", "000000", kNow);
    EXPECT_TRUE(fifth.stoppedNow);
    EXPECT_EQ(fifth.stoppedUntil, kNow + Pairings::kStoppedForMs);
    const auto refused = pairings.pair("Laptop", code, kNow + 1000);
    EXPECT_EQ(refused.error, "stopped");
    EXPECT_EQ(refused.stoppedUntil, kNow + Pairings::kStoppedForMs);
    const auto fresh = pairings.newCode(kNow + Pairings::kStoppedForMs);
    EXPECT_FALSE(pairings.pair("Laptop", fresh, kNow + Pairings::kStoppedForMs).token.empty());
}

// No two paired devices share a name: the device is asked for another, "<name> (2)" offered.
TEST(Pairings, ANameAPairedDeviceHasIsAskedToChange) {
    Pairings pairings({});
    ASSERT_FALSE(pairings.pair("Study PC", "", kNow).token.empty());
    const auto taken = pairings.pair("study pc", "", kNow);
    EXPECT_EQ(taken.error, "nameTaken");
    EXPECT_EQ(taken.suggestion, "study pc (2)");
    ASSERT_FALSE(pairings.pair("Study PC (2)", pairings.newCode(kNow), kNow).token.empty());
    EXPECT_EQ(pairings.pair("Study PC", pairings.newCode(kNow), kNow).suggestion, "Study PC (3)");
    // A name taken does not use the code up.
    const auto code = pairings.newCode(kNow);
    EXPECT_EQ(pairings.pair("Study PC", code, kNow).error, "nameTaken");
    EXPECT_FALSE(pairings.pair("Laptop", code, kNow).token.empty());
}

TEST(Pairings, RevokedTokensStopAndResetOwnerOpensTheServerToAClaim) {
    const auto file = testutil::tempDir() / ("pairings-" + std::to_string(GetCurrentProcessId()) + ".json");
    std::filesystem::remove(file);
    std::string first, second;
    {
        Pairings pairings(file);
        first = pairings.pair("Study PC", "", kNow).token;
        second = pairings.pair("Laptop", pairings.newCode(kNow), kNow).token;
    }
    // Kept across a restart, tokens only as their hashes.
    {
        std::ifstream in(file);
        const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        EXPECT_EQ(text.find(first), std::string::npos);
    }
    Pairings pairings(file);
    ASSERT_TRUE(pairings.deviceOf(first));
    EXPECT_TRUE(pairings.revoke(pairings.deviceOf(second)->id));
    EXPECT_FALSE(pairings.deviceOf(second));
    EXPECT_FALSE(pairings.deviceOf("not a token"));
    EXPECT_FALSE(pairings.deviceOf(""));
    pairings.resetOwner();
    EXPECT_FALSE(pairings.owned());
    EXPECT_FALSE(pairings.deviceOf(first));
    EXPECT_FALSE(pairings.pair("Tablet", "", kNow).token.empty());

    // A file it cannot read leaves the Server owned: pairing needs a code, never a claim.
    std::ofstream(file, std::ios::trunc) << "{ damaged";
    Pairings damaged(file);
    EXPECT_TRUE(damaged.owned());
    EXPECT_EQ(damaged.pair("Laptop", "", kNow).error, "codeNeeded");
    EXPECT_FALSE(damaged.pair("Laptop", damaged.newCode(kNow), kNow).token.empty());
    std::filesystem::remove(file);
}

// A Control point keeps each Server's token encrypted with DPAPI for the Windows user, by the Server's ID.
TEST(ServerTokens, AreKeptEncryptedByServer) {
    const auto file = testutil::tempDir() / ("server-tokens-" + std::to_string(GetCurrentProcessId()) + ".dat");
    std::filesystem::remove(file);
    {
        ServerTokens tokens(file);
        EXPECT_FALSE(tokens.token("server-1"));
        tokens.set("server-1", "a-secret-token-0123456789");
        tokens.set("server-2", "another");
    }
    {
        std::ifstream in(file, std::ios::binary);
        const std::string sealed{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        EXPECT_EQ(sealed.find("a-secret-token"), std::string::npos);
    }
    ServerTokens tokens(file);
    EXPECT_EQ(tokens.token("server-1"), "a-secret-token-0123456789");
    tokens.forget("server-1");
    EXPECT_FALSE(ServerTokens(file).token("server-1"));
    EXPECT_EQ(ServerTokens(file).token("server-2"), "another");
    std::ofstream(file, std::ios::binary | std::ios::trunc) << "not sealed";
    EXPECT_FALSE(ServerTokens(file).token("server-2"));
    std::filesystem::remove(file);
}
