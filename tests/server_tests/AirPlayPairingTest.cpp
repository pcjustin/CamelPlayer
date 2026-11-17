// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The vectors not from RFC 8439 come from Python's `cryptography` package and a separate SRP-6a written in Python.

#include <gtest/gtest.h>

#include <string>

#include "FakeAirPlayReceiver.h"
#include "Server/Network/Pairing.h"
#include "Server/Network/Rtsp.h"

using namespace camel::server::airplay;

namespace {

std::string bytes(std::string_view hex) {
    std::string out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(char(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
    return out;
}

std::string hexOf(std::string_view data) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (const char c : data) {
        out += kDigits[uint8_t(c) >> 4];
        out += kDigits[uint8_t(c) & 0xF];
    }
    return out;
}

std::string range(int first, int count) {
    std::string out;
    for (int i = 0; i < count; ++i) out.push_back(char(first + i));
    return out;
}

const std::string kSunscreen = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, "
                               "sunscreen would be it.";

} // namespace

TEST(PairingCryptoTest, Poly1305MatchesRfc8439) {
    const auto key = bytes("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b");
    EXPECT_EQ(hexOf(poly1305(key, "Cryptographic Forum Research Group")), "a8061dc1305136c6c22b8baf0c0127a9");
}

TEST(PairingCryptoTest, ChaCha20Poly1305MatchesRfc8439) {
    const auto key = range(0x80, 32);
    const auto nonce = bytes("070000004041424344454647");
    const auto aad = bytes("50515253c0c1c2c3c4c5c6c7");
    const auto sealed = seal(key, nonce, aad, kSunscreen);
    EXPECT_EQ(
        hexOf(sealed),
        "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6"
        "a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc3ff4def08e4b7a9de576d26586cec64b6116"
        "1ae10b594f09e26a7e902ecbd0600691");
    EXPECT_EQ(open(key, nonce, aad, sealed), kSunscreen);
    auto damaged = sealed;
    damaged[5] = char(damaged[5] ^ 1);
    EXPECT_FALSE(open(key, nonce, aad, damaged));
}

TEST(PairingCryptoTest, HkdfSha512MatchesAnotherImplementation) {
    EXPECT_EQ(hexOf(hkdfSha512(std::string(64, 'k'), "Control-Salt", "Control-Write-Encryption-Key", 32)),
              "2215752060bcc833e5d7002d56212d6f6117794339c3581c4a93ab0e2aa452b0");
}

TEST(PairingCryptoTest, ModularPowersAreRight) {
    EXPECT_EQ(modPow(bytes("03"), bytes("010001"), bytes("0f4243")), bytes("076d5e")); // 3^65537 mod 1000003 = 486750
    EXPECT_EQ(hexOf(modPow(bytes("1234567890abcdef"), bytes("0fedcba987654321"), bytes("7fffffffffffffffffffffffffffffff"))),
              "2a767415dbb277360980ef5f7e8f9abe");
}

TEST(PairingCryptoTest, SrpMatchesAnotherImplementation) {
    const auto salt = range(0, 16);
    const auto b =
        bytes("ecc67eced91a036378e1f9a4969a33051adad472b998e89a3590bae58c543084d7dab8eabcc8dc1c0a2762b9976f141f10e669564636bea3"
              "6fb4dafc4578677c4a81ce350524dd54048efd7a6405ddca2d3515b92e26e7f7ba285e44d4762091d1117ac807282889be8173f05681c9"
              "e52e9748310788c5b5cb95fd2008711aad9a267a6c29a32f81da40f618222042da6b234fbc5ee8cb5030c3028e68c864e8b31ba3e9f407"
              "4069590f94036f5c2e5467a9406ed3c8e68c8dd1083362f37827f2b54db7a9bf2fa96fe29186d94ba8c8a3d08c3e1e766067036fbb15a0"
              "c9d0b1c5f4ac7ce16f47c2e9309b0708cfa3092baeab4e1515efa29e15d8336d1c085f1e4dbab972397b6a194ea8cf85ac04489af3ebf1"
              "430414ed13770c968f7fd5ed3aacb315c3df9ba2ae7c5bf2d1f479785f20001e2588621e1c0e561429a6fa9556ac15d8077883b264ce6a"
              "57ed6fc2f7fd98559b4f1279c62f059343228377a28e4928cc4a7b3eb2d9d33482a92cadd1e5373fec62ea577ad857621529816903");
    const auto server = srpServer("Pair-Setup", "3939", salt, range(33, 32));
    EXPECT_EQ(server.publicKey, b);
    const auto proof = srpClient("Pair-Setup", "3939", salt, b, range(1, 32));
    EXPECT_EQ(hexOf(sha512(proof.publicKey)).substr(0, 32), "5a7e0097d9d811439d2eb9e82f58a97c");
    EXPECT_EQ(hexOf(proof.sessionKey), "e630bac6f2a1f47bb1c03420dd99a546491404a5a0a90146fdab3775a7e36778f9d20a75dc6d6728712db6"
                                       "455c5dd894d3eda560e49075cca34d3ea82590ea98");
    EXPECT_EQ(hexOf(proof.proof), "9b68730f513a09bfc189ae640956488fdd06bca049406b360c2e213c69329076bdd0b27191f6cfb560baedfb4"
                                  "ffc5b830df791be689034afdf839eb80bbc247d");
    EXPECT_EQ(hexOf(proof.expected), "cf5f73c8bd14d1f63580815bbc6836b818a4dbaa967cadd43fd0fcc7d3cb2f68643428bbc080e08039be1f"
                                     "2f87590a9465fc5d7ac7ee71e36ea4a18d9ac29bb4");
    const auto checked = srpServerCheck(server, "Pair-Setup", salt, proof.publicKey, proof.proof, range(33, 32));
    ASSERT_TRUE(checked);
    EXPECT_EQ(checked->first, proof.sessionKey);
    EXPECT_EQ(checked->second, proof.expected);
}

TEST(PairingCryptoTest, AWrongCodeFailsTheProof) {
    const auto salt = range(0, 16);
    const auto server = srpServer("Pair-Setup", "1234", salt, range(33, 32));
    const auto proof = srpClient("Pair-Setup", "3939", salt, server.publicKey, range(1, 32));
    EXPECT_FALSE(srpServerCheck(server, "Pair-Setup", salt, proof.publicKey, proof.proof, range(33, 32)));
}

TEST(PairingCryptoTest, AReceiverKeyOfZeroIsNoKey) {
    EXPECT_TRUE(srpClient("Pair-Setup", "3939", range(0, 16), std::string(384, '\0'), range(1, 32)).proof.empty());
    EXPECT_TRUE(srpClient("Pair-Setup", "3939", range(0, 16), srpPrime(), range(1, 32)).proof.empty());
}

TEST(PairingCryptoTest, LongValuesSpanSeveralItems) {
    const std::string key(300, 'x');
    const auto message = tlv({{6, "\x01"}, {3, key}, {0, ""}});
    EXPECT_EQ(message.size(), 3 + (2 + 255) + (2 + 45) + 2);
    const auto items = parseTlv(message);
    EXPECT_EQ(items.at(3), key);
    EXPECT_EQ(items.at(6), "\x01");
    EXPECT_EQ(items.at(0), "");
}

TEST(PairingCryptoTest, TheChannelCarriesLongMessagesInBlocksBothWays) {
    auto sender = SecureChannel::forSender(std::string(64, 'k'));
    auto receiver = SecureChannel::forReceiver(std::string(64, 'k'));
    const std::string first(2500, 'a');
    std::string wire = sender.seal(first);
    wire += sender.seal("second");
    EXPECT_EQ(wire.size(), 2500 + 6 + 4 * (2 + 16));
    std::string part = wire.substr(0, 1500);
    EXPECT_EQ(receiver.open(part), first.substr(0, 1024));
    part += wire.substr(1500);
    EXPECT_EQ(receiver.open(part), first.substr(1024) + "second");
    EXPECT_TRUE(part.empty());
    std::string back = receiver.seal("reply");
    EXPECT_EQ(sender.open(back), "reply");
    std::string replayed = sender.seal("again");
    std::string copy = replayed;
    EXPECT_EQ(receiver.open(replayed), "again");
    EXPECT_FALSE(receiver.open(copy)); // the same block again has the wrong count
}

TEST(PairingRtspTest, AReceiverThatDemandsPairingIsPairedAndTheConnectionEncrypted) {
    testutil::FakeRtsp rtsp;
    rtsp.demandPairing();
    {
        RtspClient plain("127.0.0.1", rtsp.port(), 1000);
        EXPECT_EQ(plain.request("OPTIONS", "*").status, 403);
    }
    RtspClient client("127.0.0.1", rtsp.port(), 1000);
    ASSERT_TRUE(client.pairTransiently());
    EXPECT_EQ(client.request("OPTIONS", "*").status, 200);
    EXPECT_EQ(client.request("ANNOUNCE", client.url(), "application/sdp", std::string(3000, 'v')).status, 200);
    EXPECT_EQ(rtsp.encryptedRequests(), 2);
    EXPECT_EQ(rtsp.requestsOf("ANNOUNCE").front().substr(rtsp.requestsOf("ANNOUNCE").front().size() - 3000),
              std::string(3000, 'v'));
}

TEST(PairingRtspTest, AReceiverWithAnotherCodeDoesNotPair) {
    testutil::FakeRtsp rtsp;
    rtsp.demandPairing("1234");
    RtspClient client("127.0.0.1", rtsp.port(), 1000);
    EXPECT_FALSE(client.pairTransiently());
    EXPECT_EQ(client.request("OPTIONS", "*").status, 403);
    EXPECT_EQ(rtsp.encryptedRequests(), 0);
}
