// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <winsock2.h>

#include <chrono>
#include <cstdint>
#include <thread>

namespace camel::server {

// Waits up to `limit` for one of `sockets` to be readable; true when one is. A wait that fails, as on a socket that
// could not be made, still takes `limit`, so a loop around it does not spin.
inline bool waitReadable(fd_set& sockets, std::chrono::microseconds limit) {
    const timeval wait{long(limit.count() / 1000000), long(limit.count() % 1000000)};
    const int ready = select(0, &sockets, nullptr, nullptr, &wait);
    if (ready == SOCKET_ERROR) std::this_thread::sleep_for(limit);
    return ready > 0;
}

// Winsock started while the object lives: a member before those that hold sockets, so it ends after them.
class WinsockScope {
public:
    WinsockScope() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    ~WinsockScope() { WSACleanup(); }
    WinsockScope(const WinsockScope&) = delete;
    WinsockScope& operator=(const WinsockScope&) = delete;
};

enum class Reach : uint8_t { Connected, Refused, TimedOut, Failed };

// Connects `s` to `address` within `timeout`, leaving the socket non-blocking; Refused when the peer turned it away.
inline Reach connectWithin(SOCKET s, const sockaddr* address, int length, std::chrono::milliseconds timeout) {
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    if (connect(s, address, length) == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        if (error == WSAECONNREFUSED) return Reach::Refused;
        if (error != WSAEWOULDBLOCK) return Reach::Failed;
    }
    fd_set writable, failed;
    FD_ZERO(&writable);
    FD_ZERO(&failed);
    FD_SET(s, &writable);
    FD_SET(s, &failed);
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(timeout).count();
    const timeval wait{long(us / 1000000), long(us % 1000000)};
    const int ready = select(0, nullptr, &writable, &failed, &wait);
    if (ready > 0 && FD_ISSET(s, &writable) && !FD_ISSET(s, &failed)) return Reach::Connected;
    if (ready > 0) return Reach::Refused;
    return ready == 0 ? Reach::TimedOut : Reach::Failed;
}

} // namespace camel::server
