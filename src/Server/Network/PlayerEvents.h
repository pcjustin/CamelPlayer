// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The event subscriptions to a network player's services (docs/Plan.md §5.10), renewed halfway through what the player
// granted and made again after a restart of the player.
#pragma once

#include <chrono>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "Server/Network/Upnp.h"

namespace camel::server {

struct LinkOptions {
    std::chrono::milliseconds pollInterval{2000};
    int failuresUntilLost = 2;
    int subscribeSeconds = 1800;
    int requestTimeoutMs = 3000;
};

class PlayerEvents {
public:
    struct Notification {
        std::string kind; // the service it came from: "AVTransport", "RenderingControl", ...
        std::string body;
    };

    // Throws std::runtime_error when the player offers none of these events or refuses one.
    PlayerEvents(const upnp::PlayerDescription& player, const std::vector<std::string>& kinds, LinkOptions options);
    ~PlayerEvents();
    PlayerEvents(const PlayerEvents&) = delete;
    PlayerEvents& operator=(const PlayerEvents&) = delete;

    std::deque<Notification> take();
    void renewDue(); // throws std::runtime_error when the player does not answer or dropped a subscription

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace camel::server
