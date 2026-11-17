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
    std::optional<double> db; // a player whose volume is in dB (HQPlayer): the level `level` stands for
    bool operator==(const PlayerLevel&) const = default;
};

// A player's own volume, followed and set without waiting for the player.
class OwnVolume {
public:
    virtual ~OwnVolume() = default;
    virtual std::optional<PlayerLevel> current() const = 0; // nothing while it cannot be read or used
    virtual void setLevel(uint32_t level) = 0;
    virtual void setMuted(bool muted) = 0;
};

class PlayerVolume final : public OwnVolume {
public:
    // Talks to the player on its own thread, so nothing here waits for it.
    explicit PlayerVolume(upnp::PlayerDescription player, LinkOptions options = {std::chrono::seconds(5), 2, 1800, 1500});
    ~PlayerVolume() override;
    PlayerVolume(const PlayerVolume&) = delete;
    PlayerVolume& operator=(const PlayerVolume&) = delete;

    std::optional<PlayerLevel> current() const override;
    void setLevel(uint32_t level) override;
    void setMuted(bool muted) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace camel::server
