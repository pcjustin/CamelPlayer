// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Finding CamelPlayers on the LAN (docs/Plan.md §17.8): each one that serves the Server API announces itself through
// mDNS/DNS-SD as `_camelplayer._tcp`, its TXT naming its ID, its name and its API versions; Control points ask for it.
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace camel::server {

struct ServerAnnouncement {
    std::string id;   // the Server's own, which outlives its address and port
    std::string name; // the computer's by default
    uint16_t port = 0;
    int apiMin = 2, apiMax = 2;
};

struct FoundServer {
    ServerAnnouncement announced;
    std::string address; // IPv4, dotted
    bool operator==(const FoundServer&) const = default;
};

namespace discovery {
constexpr std::string_view kService = "_camelplayer._tcp";
// An mDNS response holding the service's PTR, SRV, TXT and A records for `addresses`; a TTL of 0 says goodbye.
// `id` is copied from a legacy unicast query, which wants its ID and question back.
std::string responseMessage(const ServerAnnouncement& announcement, std::string_view host,
                            const std::vector<std::string>& addresses, uint32_t ttl, uint16_t id = 0,
                            std::string_view question = {});
// Whether an mDNS message asks for the service, an instance of it or `host`; the question's bytes come back for a
// legacy unicast answer. Throws nothing: anything it cannot read asks for nothing.
bool asksFor(std::string_view message, std::string_view host, std::string* question = nullptr);
// The Servers the records of `messages` announce, each once by its ID.
std::vector<FoundServer> serversIn(const std::vector<std::string>& messages);
} // namespace discovery

// Announces the Server while it lives: at start and again a second later, then in answer to each question, and a
// goodbye at the end.
class ServerAnnouncer {
public:
    explicit ServerAnnouncer(ServerAnnouncement announcement);
    ~ServerAnnouncer();
    ServerAnnouncer(const ServerAnnouncer&) = delete;
    ServerAnnouncer& operator=(const ServerAnnouncer&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The Servers that answer within `wait`.
std::vector<FoundServer> findServers(std::chrono::milliseconds wait);

} // namespace camel::server
