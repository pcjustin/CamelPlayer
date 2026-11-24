// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "Server/Remote/ApiServer.h"
#include "Server/Remote/DevicePairing.h"

namespace camel::server {

// Letting other computers control this one (docs/Plan.md §17.8): while the Server's settings allow it, the Server API
// serves the LAN and mDNS announces it; once they no longer do, it closes, cutting every Control point off.
class RemoteAccess final : public DevicePairing {
public:
    struct Options {
        std::filesystem::path data; // the Server's data folder: its ID, the paired devices and the uploads
        std::string name;           // what other computers call it; the computer's by default
        uint16_t port = ApiServer::kPort;
        bool loopbackOnly = false;
        bool announce = true;
    };
    RemoteAccess(LocalServer& server, Options options);
    ~RemoteAccess() override; // as stop()
    RemoteAccess(const RemoteAccess&) = delete;
    RemoteAccess& operator=(const RemoteAccess&) = delete;

    // Closes the Server API for good, before the Server shuts down.
    void stop();
    // The port served while other computers may control this one; nothing while they may not, or none was free.
    std::optional<uint16_t> port() const;
    const std::string& serverId() const;

    // Throw std::runtime_error while the Server API is not served.
    std::string pairingCode() override;
    nlohmann::json pairedDevices() override;
    bool revokeDevice(const std::string& device) override;
    void resetOwner() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The Server's ID, kept in `data` from the first time it is asked for; another one when the file cannot be kept.
std::string serverIdIn(const std::filesystem::path& data);
// This computer's name, as other computers see it by default.
std::string computerName();

} // namespace camel::server
