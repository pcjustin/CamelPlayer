// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include <chrono>

#include "Server/Network/SocketWait.h"

using namespace camel::server;
using namespace std::chrono_literals;

// A wait that fails, as on a socket that could not be made, still takes its time, so the finder and AirPlay loops
// around it do not spin a core.
TEST(SocketWait, AFailedWaitTakesItsTime) {
    WSADATA wsa;
    ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &wsa), 0);
    fd_set sockets;
    FD_ZERO(&sockets);
    FD_SET(INVALID_SOCKET, &sockets);
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(waitReadable(sockets, 100ms));
    EXPECT_GE(std::chrono::steady_clock::now() - start, 90ms);
    WSACleanup();
}
