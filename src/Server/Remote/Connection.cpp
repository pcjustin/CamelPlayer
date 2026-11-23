// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Remote/Connection.h"

#include <array>
#include <random>
#include <stdexcept>

namespace camel::server::remote {
namespace {

// Up to one chunk more into `buffer`; false when the connection closed or `deadline` passed first.
bool receiveMore(SOCKET s, std::string& buffer, std::chrono::steady_clock::time_point deadline) {
    const auto left = deadline - std::chrono::steady_clock::now();
    if (left <= std::chrono::steady_clock::duration::zero()) return false;
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(s, &readable);
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(left).count();
    const timeval wait{long(us / 1'000'000), long(us % 1'000'000)};
    if (select(0, &readable, nullptr, nullptr, &wait) <= 0) return false;
    char chunk[16384];
    const int got = recv(s, chunk, int(sizeof(chunk)), 0);
    if (got <= 0) return false;
    buffer.append(chunk, size_t(got));
    return true;
}

std::array<uint8_t, 4> randomMask() {
    thread_local std::mt19937 generator{std::random_device{}()};
    const uint32_t bits = generator();
    return {uint8_t(bits), uint8_t(bits >> 8), uint8_t(bits >> 16), uint8_t(bits >> 24)};
}

} // namespace

bool sendAll(SOCKET s, std::string_view data) {
    while (!data.empty()) {
        const auto part = data.substr(0, 1 << 20);
        const int sent = ::send(s, part.data(), int(part.size()), 0);
        if (sent <= 0) return false;
        data.remove_prefix(size_t(sent));
    }
    return true;
}

std::optional<std::string> readHead(SOCKET s, std::string& rest, size_t limit, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (const auto end = rest.find("\r\n\r\n"); end != std::string::npos) {
            std::string head = rest.substr(0, end);
            rest.erase(0, end + 4);
            return head;
        }
        if (rest.size() > limit || !receiveMore(s, rest, deadline)) return std::nullopt;
    }
}

std::optional<std::string> readBody(SOCKET s, std::string& rest, size_t length, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (rest.size() < length)
        if (!receiveMore(s, rest, deadline)) return std::nullopt;
    std::string body = rest.substr(0, length);
    rest.erase(0, length);
    return body;
}

Connection::Connection(SOCKET s, bool client, std::string pending) : socket_(s), client_(client), buffer_(std::move(pending)) {}

Connection::~Connection() {
    closesocket(socket_);
}

void Connection::sendFrame(websocket::Opcode opcode, std::string_view payload) {
    std::lock_guard lock(sending_);
    if (closed_) throw std::runtime_error("the connection is closed");
    const auto frame = client_ ? websocket::encodeFrame(opcode, payload, randomMask()) : websocket::encodeFrame(opcode, payload);
    if (!sendAll(socket_, frame)) {
        closed_ = true;
        throw std::runtime_error("the connection is gone");
    }
}

void Connection::send(std::string_view text) {
    sendFrame(websocket::Opcode::Text, text);
}

std::optional<std::string> Connection::read(size_t limit) {
    std::optional<std::string> message;
    for (;;) {
        const auto frame = websocket::decodeFrame(buffer_, !client_, limit);
        if (!frame) {
            char chunk[65536];
            const int got = recv(socket_, chunk, int(sizeof(chunk)), 0);
            if (got <= 0) return std::nullopt;
            buffer_.append(chunk, size_t(got));
            continue;
        }
        buffer_.erase(0, frame->second);
        const auto& [fin, opcode, payload] = frame->first;
        switch (opcode) {
        case websocket::Opcode::Ping:
            try {
                sendFrame(websocket::Opcode::Pong, payload);
            } catch (const std::runtime_error&) {
                return std::nullopt;
            }
            continue;
        case websocket::Opcode::Pong:
            continue;
        case websocket::Opcode::Close:
            try {
                sendFrame(websocket::Opcode::Close, payload.substr(0, 2));
            } catch (const std::runtime_error&) {
            }
            return std::nullopt;
        case websocket::Opcode::Continuation:
            if (!message) throw std::invalid_argument("WebSocket: a continuation without a message");
            *message += payload;
            break;
        default:
            if (message) throw std::invalid_argument("WebSocket: a message inside another");
            message = payload;
            break;
        }
        if (message->size() > limit) throw std::invalid_argument("WebSocket: a message longer than the limit");
        if (fin) return message;
    }
}

void Connection::shut(uint16_t code) {
    try {
        const char reason[2] = {char(code >> 8), char(code & 0xFF)};
        sendFrame(websocket::Opcode::Close, std::string_view(reason, 2));
    } catch (const std::runtime_error&) {
    }
    {
        std::lock_guard lock(sending_);
        closed_ = true;
    }
    shutdown(socket_, SD_BOTH);
}

} // namespace camel::server::remote
