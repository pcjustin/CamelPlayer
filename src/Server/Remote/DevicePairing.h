// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace camel::server {

// What any paired device may do about pairing (docs/Plan.md §17.8): the Server's own window through its RemoteAccess,
// a Control point through its RemoteServer. Each throws std::runtime_error when the Server cannot be reached.
class DevicePairing {
public:
    virtual ~DevicePairing() = default;
    virtual std::string pairingCode() = 0;
    virtual nlohmann::json pairedDevices() = 0; // each {"id", "name", "paired", "connected"}
    virtual bool revokeDevice(const std::string& device) = 0;
    virtual void resetOwner() = 0;
};

// The names of the paired devices connected now, but `except`: those an exit or a change of role cuts off.
inline std::vector<std::string> connectedNames(const nlohmann::json& devices, const std::string& except = {}) {
    std::vector<std::string> names;
    for (const auto& device : devices)
        if (device.value("connected", false) && (except.empty() || device.value("id", std::string()) != except))
            names.push_back(device.value("name", std::string()));
    return names;
}

} // namespace camel::server
