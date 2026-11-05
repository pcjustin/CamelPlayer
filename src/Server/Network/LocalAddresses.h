// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <winsock2.h>

#include <vector>

namespace camel::server {

// The IPv4 addresses of the adapters that are up, without loopback, for multicast on each of them.
std::vector<in_addr> localAddresses();

} // namespace camel::server
