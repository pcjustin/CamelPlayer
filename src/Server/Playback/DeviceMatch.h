// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "Server/Api/ServerApi.h"

namespace camel::server {

// The entry in `candidates` (devices of backend `to`) that is the same hardware as `device` on backend `from`, judged
// by name: WASAPI names the device in parentheses after the endpoint, KS before " / ", ASIO in the driver name. An
// identical device name wins; otherwise the most shared distinctive words. Nothing when none fits or several fit
// equally, so a switch never guesses between devices.
std::optional<std::string> matchDevice(Backend from, const DeviceInfo& device, Backend to,
                                       const std::vector<DeviceInfo>& candidates);

} // namespace camel::server
