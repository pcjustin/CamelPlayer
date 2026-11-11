// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <winsock2.h>

#include <vector>

namespace camel::server {

// The IPv4 addresses of the adapters that are up, without loopback, for multicast on each of them. `firstGuess` is
// the room first given to the adapter list; a test makes it small.
std::vector<in_addr> localAddresses(unsigned long firstGuess = 16 * 1024);

} // namespace camel::server
