// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <winsock2.h>

#include <chrono>
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

} // namespace camel::server
