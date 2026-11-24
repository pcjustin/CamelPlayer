// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "Server/Playback/LocalServer.h"
#include "Server/Remote/ApiSession.h"

namespace camel::server {

// The Server API on the LAN (docs/Plan.md §17.8, docs/Server-API.md): WebSocket JSON at /api for the calls, the pushed
// snapshots and events, and plain HTTP for covers, artist pictures, uploads, exported playlists and the diagnostic
// bundle, on one port for every connection.
class ApiServer {
public:
    static constexpr uint16_t kPort = 9440;
    struct Options {
        uint16_t port = kPort; // 0 takes any free one
        int portsToTry = 10;   // the next ones when another program holds it
        bool loopbackOnly = false;
        std::string serverId, serverName;
        std::filesystem::path uploads;                      // where uploaded playlists are kept
        std::filesystem::path pairings;                     // the paired devices; in memory when empty
        uint32_t apiMin = kApiOldest, apiMax = kApiVersion; // the API versions served
        std::string appVersion;                             // this CamelPlayer's, told to Control points
        // Calls of the Server's computer rather than of its IServer, such as updating it; nothing for one it does not
        // know. They run as the caller (Caller.h).
        std::function<std::optional<ApiSession::Answer>(const std::string& call, const nlohmann::json& args)> calls;
        // Whether `proof` shows that the computer whose Server ID is `server` gave this one a token: two computers
        // that paired pair the other way without a code (docs/Plan.md §17.8).
        std::function<bool(const std::string& server, const std::string& proof)> trusts;
    };

    // Starts listening; throws std::runtime_error when none of the ports is free.
    ApiServer(LocalServer& server, Options options);
    ~ApiServer(); // closes every connection and waits for them
    ApiServer(const ApiServer&) = delete;
    ApiServer& operator=(const ApiServer&) = delete;

    uint16_t port() const;
    size_t connections() const; // WebSocket connections open now

    // Pairing as the Server's own window, always paired, does it (docs/Plan.md 17.8).
    std::string pairingCode();
    nlohmann::json pairedDevices() const;   // each with its ID, name, when it paired and whether it is connected now
    bool revoke(const std::string& device); // its connections close at once
    void resetOwner();                      // every device forgotten and cut off

private:
    struct Impl;
    std::shared_ptr<Impl> impl_; // shared with an event on its way to the connections
};

} // namespace camel::server
