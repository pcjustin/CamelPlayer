// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "Server/Playback/LocalServer.h"

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
        std::filesystem::path uploads; // where uploaded playlists are kept
    };

    // Starts listening; throws std::runtime_error when none of the ports is free.
    ApiServer(LocalServer& server, Options options);
    ~ApiServer(); // closes every connection and waits for them
    ApiServer(const ApiServer&) = delete;
    ApiServer& operator=(const ApiServer&) = delete;

    uint16_t port() const;
    size_t connections() const; // WebSocket connections open now

private:
    struct Impl;
    std::shared_ptr<Impl> impl_; // shared with an event on its way to the connections
};

} // namespace camel::server
