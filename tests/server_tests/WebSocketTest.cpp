// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include <gtest/gtest.h>

#include <string>

#include "Server/Remote/WebSocket.h"

using namespace camel::server::websocket;

namespace {

std::string bytes(std::initializer_list<int> values) {
    std::string out;
    for (const int v : values) out += char(v);
    return out;
}

} // namespace

TEST(WebSocket, AnswersTheHandshakeAsRfc6455Does) {
    EXPECT_EQ(acceptKey("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    const std::string man = "Man", ma = "Ma", m = "M";
    EXPECT_EQ(base64({reinterpret_cast<const uint8_t*>(man.data()), man.size()}), "TWFu");
    EXPECT_EQ(base64({reinterpret_cast<const uint8_t*>(ma.data()), ma.size()}), "TWE=");
    EXPECT_EQ(base64({reinterpret_cast<const uint8_t*>(m.data()), m.size()}), "TQ==");
}

TEST(WebSocket, ReadsRequestHeadsAndQueries) {
    const auto head = parseHead("GET /api?x=1 HTTP/1.1\r\nHost: pc:9440\r\nUPGRADE:  websocket \r\nSec-WebSocket-Key: abc");
    EXPECT_EQ(head.first, "GET");
    EXPECT_EQ(head.second, "/api?x=1");
    EXPECT_EQ(head.third, "HTTP/1.1");
    EXPECT_EQ(head.header("upgrade"), "websocket");
    EXPECT_EQ(head.header("host"), "pc:9440");
    EXPECT_EQ(head.header("missing"), "");
    EXPECT_EQ(parseHead("HTTP/1.1 404 Not Found").third, "Not Found");
    EXPECT_THROW(parseHead(""), std::invalid_argument);
    EXPECT_THROW(parseHead("GARBAGE"), std::invalid_argument);
    EXPECT_THROW(parseHead("GET / HTTP/1.1\r\nno colon"), std::invalid_argument);

    const auto query = queryOf("/artist?name=Sigur%20R%C3%B3s&size=512&empty");
    EXPECT_EQ(query.at("name"), "Sigur Rós");
    EXPECT_EQ(query.at("size"), "512");
    EXPECT_EQ(query.at("empty"), "");
    EXPECT_TRUE(queryOf("/no-query").empty());
    EXPECT_THROW(queryOf("/x?name=%G0"), std::invalid_argument);
    EXPECT_THROW(queryOf("/x?name=%4"), std::invalid_argument);
    EXPECT_EQ(percentDecode(percentEncode("a b/é?&=")), "a b/é?&=");
}

TEST(WebSocket, DecodesTheFramesOfRfc6455) {
    // A masked "Hello" from a client and the same unmasked from a server.
    const auto masked = bytes({0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58});
    auto frame = decodeFrame(masked, true, 1024);
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->first.payload, "Hello");
    EXPECT_EQ(frame->first.opcode, Opcode::Text);
    EXPECT_TRUE(frame->first.fin);
    EXPECT_EQ(frame->second, masked.size());
    const auto plain = bytes({0x81, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f});
    EXPECT_EQ(decodeFrame(plain, false, 1024)->first.payload, "Hello");
    // A fragment of a text message.
    EXPECT_FALSE(decodeFrame(bytes({0x01, 0x03, 0x48, 0x65, 0x6c}), false, 1024)->first.fin);
    // Too few bytes so far.
    EXPECT_FALSE(decodeFrame(masked.substr(0, 6), true, 1024));
    EXPECT_FALSE(decodeFrame(bytes({0x82, 0x7e, 0x01}), false, 1 << 20));
}

TEST(WebSocket, EncodesEveryLengthAndMasksForAClient) {
    for (const size_t size : {size_t(0), size_t(125), size_t(126), size_t(65535), size_t(65536), size_t(200000)}) {
        const std::string payload(size, 'x');
        const auto server = encodeFrame(Opcode::Binary, payload);
        const auto read = decodeFrame(server, false, 1 << 20);
        ASSERT_TRUE(read) << size;
        EXPECT_EQ(read->first.payload, payload);
        EXPECT_EQ(read->second, server.size());
        const auto client = encodeFrame(Opcode::Text, payload, std::array<uint8_t, 4>{1, 2, 3, 4});
        EXPECT_EQ(decodeFrame(client, true, 1 << 20)->first.payload, payload);
    }
}

TEST(WebSocket, RefusesFramesThatBreakTheProtocol) {
    EXPECT_THROW(decodeFrame(bytes({0xC1, 0x00}), false, 1024), std::invalid_argument); // a reserved bit
    EXPECT_THROW(decodeFrame(bytes({0x83, 0x00}), false, 1024), std::invalid_argument); // opcode 3
    EXPECT_THROW(decodeFrame(bytes({0x81, 0x00}), true, 1024), std::invalid_argument);  // unmasked from a client
    EXPECT_THROW(decodeFrame(bytes({0x81, 0x80, 0, 0, 0, 0}), false, 1024), std::invalid_argument);
    EXPECT_THROW(decodeFrame(bytes({0x09, 0x00}), false, 1024), std::invalid_argument);             // a fragmented ping
    EXPECT_THROW(decodeFrame(bytes({0x89, 0x7e, 0x00, 0x80}), false, 1024), std::invalid_argument); // a long ping
    EXPECT_THROW(decodeFrame(bytes({0x82, 0x7f, 0x80, 0, 0, 0, 0, 0, 0, 0}), false, 1024), std::invalid_argument);
    EXPECT_THROW(decodeFrame(bytes({0x82, 0x7e, 0x04, 0x01}), false, 1024), std::invalid_argument); // over the limit
}
