// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <winsock2.h>

#include <chrono>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "Server/Remote/WebSocket.h"

namespace camel::server::remote {

// Writes all of `data`; false once the connection is gone.
bool sendAll(SOCKET s, std::string_view data);
// An HTTP head read from `s` up to its blank line, the bytes read past it left in `rest`; nothing when the connection
// closes first, takes longer than `timeout` or the head is longer than `limit`.
std::optional<std::string> readHead(SOCKET s, std::string& rest, size_t limit, std::chrono::milliseconds timeout);
// The `length` bytes after a head, `rest` first; nothing when the connection closes first.
std::optional<std::string> readBody(SOCKET s, std::string& rest, size_t length, std::chrono::milliseconds timeout);

// A WebSocket over a connected socket, which it closes. Messages are sent whole from any thread and read whole on one.
class Connection {
public:
    Connection(SOCKET s, bool client, std::string pending = {});
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // Throws std::runtime_error once the connection is gone.
    void send(std::string_view text);
    // The next whole text message; nothing once the peer closed it or it was shut. Pings are answered on the way.
    // Throws std::invalid_argument for a frame that breaks the protocol or a message longer than `limit`.
    std::optional<std::string> read(size_t limit);
    // Ends it from any thread: a read under way returns nothing.
    void shut(uint16_t code = 1000);

private:
    void sendFrame(websocket::Opcode opcode, std::string_view payload);
    SOCKET socket_;
    bool client_;
    std::string buffer_;
    std::mutex sending_;
    bool closed_ = false;
    std::atomic<bool> shut_{false}; // a read under way sees it within a quarter of a second, whatever the peer does
};

} // namespace camel::server::remote
