// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// A network player's own volume (docs/Plan.md §6): read when the player is chosen, followed through RenderingControl's
// LastChange events and a slow read for a player that sends none, and written only when the user asks.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "Server/Network/PlayerEvents.h"
#include "Server/Network/Upnp.h"

namespace camel::server {

struct PlayerLevel {
    uint32_t level = 0; // on the player's 0-100 scale
    bool muted = false;
    bool operator==(const PlayerLevel&) const = default;
};

class PlayerVolume {
public:
    // Talks to the player on its own thread, so nothing here waits for it.
    explicit PlayerVolume(upnp::PlayerDescription player, LinkOptions options = {std::chrono::seconds(5), 2, 1800, 1500});
    ~PlayerVolume();
    PlayerVolume(const PlayerVolume&) = delete;
    PlayerVolume& operator=(const PlayerVolume&) = delete;

    std::optional<PlayerLevel> current() const; // nothing while the player cannot be read
    void setLevel(uint32_t level);
    void setMuted(bool muted);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace camel::server
