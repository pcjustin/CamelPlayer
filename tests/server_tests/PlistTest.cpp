// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The vectors come from Python's plistlib, which writes and reads Apple's format.

#include <gtest/gtest.h>

#include <string>

#include "Server/Network/Plist.h"

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

// What the owner's TV answered to a stream SETUP, with a name in UTF-16, a key, a long string and a real added.
constexpr std::string_view kAnswer =
    "62706c6973743030d70102030405060708090a0b0c0d195b6465736372697074696f6e576c6174656e6379546e616d655373"
    "686b5a736b69705265636f72645773747265616d735b75695072656c6f616465645f10226120737472696e67206c6f6e6765"
    "72207468616e206669667465656e206279746573233ff8000000000000685ba25ef3768496fb89960020d83cdfb54f102000"
    "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f08a10ed50f1011121314151617185f100f6175"
    "64696f42756666657253697a655b636f6e74726f6c506f72745864617461506f72745873747265616d494454747970651200"
    "80000011bce0119b9113fc7a7b42871b9dc4106709000800170023002b00300034003f0047005300780081009200b500b600"
    "b800c300d500e100ea00f300f800fd01000103010c010e0000000000000201000000000000001a0000000000000000000000"
    "000000010f";

std::string range(int count) {
    std::string out;
    for (int i = 0; i < count; ++i) out.push_back(char(i));
    return out;
}

} // namespace

TEST(PlistTest, ReadsWhatPlistlibWrites) {
    const auto parsed = parseBinaryPlist(bytes(kAnswer));
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->keys,
              (std::vector<std::string>{"description", "latency", "name", "shk", "skipRecord", "streams", "uiPreloaded"}));
    EXPECT_EQ(parsed->find("description")->text, "a string longer than fifteen bytes");
    EXPECT_EQ(parsed->find("latency")->real, 1.5);
    EXPECT_EQ(parsed->find("name")->text, "\xE5\xAE\xA2\xE5\xBB\xB3\xE7\x9A\x84\xE9\x9B\xBB\xE8\xA6\x96 \xF0\x9F\x8E\xB5");
    EXPECT_EQ(parsed->find("shk")->kind, Plist::Kind::Data);
    EXPECT_EQ(parsed->find("shk")->text, range(32));
    EXPECT_FALSE(parsed->find("skipRecord")->boolean);
    EXPECT_TRUE(parsed->find("uiPreloaded")->boolean);
    const auto& streams = parsed->find("streams")->items;
    ASSERT_EQ(streams.size(), 1U);
    EXPECT_EQ(streams[0].find("audioBufferSize")->integer, 8388608);
    EXPECT_EQ(streams[0].find("type")->integer, 103);
    EXPECT_EQ(streams[0].find("dataPort")->integer, 39825);
    EXPECT_EQ(streams[0].find("controlPort")->integer, 48352);
    EXPECT_EQ(streams[0].find("streamID")->integer, -253754903328219708);
    EXPECT_EQ(parsed->find("absent"), nullptr);
}

TEST(PlistTest, WhatItWritesReadsBackTheSame) {
    Plist stream = Plist::dict();
    stream.set("type", Plist::of(96)).set("audioFormat", Plist::of(0x40000)).set("shk", Plist::data(range(32)));
    stream.set("streamConnectionID", Plist::of(int64_t{0x7123456789ABCDEF})).set("isMedia", Plist::of(true));
    stream.set("negative", Plist::of(-1)).set("wide", Plist::of(int64_t{0xFFFFFFFF} + 1)).set("latency", Plist::of(0.25));
    std::vector<Plist> many;
    many.reserve(300);
    for (int i = 0; i < 300; ++i) many.push_back(Plist::of(i));
    Plist root = Plist::dict();
    root.set("streams", Plist::array({stream})).set("name", Plist::of("\xE5\xAE\xA2\xE5\xBB\xB3 \xF0\x9F\x8E\xB5"));
    root.set("many", Plist::array(std::move(many))).set("long", Plist::of(std::string(70000, 'x')));
    const auto parsed = parseBinaryPlist(toBinaryPlist(root));
    ASSERT_TRUE(parsed);
    EXPECT_EQ(*parsed, root);
}

// Checked by Python's plistlib.loads, which reads it as {'streams': [{'type': 96, 'ct': 2, 'shk': b'\x00\x01\x02',
// 'isMedia': True}], 'name': 'CamelPlayer'}.
TEST(PlistTest, WritesWhatPlistlibReads) {
    Plist stream = Plist::dict();
    stream.set("type", Plist::of(96)).set("ct", Plist::of(2)).set("shk", Plist::data(range(3))).set("isMedia", Plist::of(true));
    Plist root = Plist::dict();
    root.set("streams", Plist::array({stream})).set("name", Plist::of("CamelPlayer"));
    EXPECT_EQ(hexOf(toBinaryPlist(root)),
              "62706c6973743030d20102030d5773747265616d73546e616d65a104d405060708090a0b0c54747970655263745373686b5769734d"
              "656469611060100243000102095b43616d656c506c61796572080d151a1c252a2d31393b3d41420000000000000101000000000000"
              "000e0000000000000000000000000000004e");
}

TEST(PlistTest, WhatIsDamagedIsNotRead) {
    const std::string good = bytes(kAnswer);
    EXPECT_FALSE(parseBinaryPlist(""));
    EXPECT_FALSE(parseBinaryPlist(good.substr(0, good.size() - 1)));
    EXPECT_FALSE(parseBinaryPlist("bplist00" + std::string(32, '\0')));
    std::string magic = good;
    magic[0] = 'x';
    EXPECT_FALSE(parseBinaryPlist(magic));
    // An array holding itself: object 0, at offset 8, refers to object 0.
    std::string loop = "bplist00";
    loop += "\xA1";
    loop.push_back('\0');
    loop.push_back('\x08');
    loop +=
        std::string(6, '\0') + "\x01\x01" + std::string(7, '\0') + "\x01" + std::string(8, '\0') + std::string(7, '\0') + "\x0A";
    EXPECT_FALSE(parseBinaryPlist(loop));
}
