// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Controlling one network player and following it (docs/Plan.md §5.10): its events, kept subscribed, and a poll that
// notices a player gone silent. Its own pause, play, seek, next and previous are reported, so CamelPlayer follows them;
// another source taking it over and the player disappearing end the link.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Server/Network/Upnp.h"

namespace camel::server {

enum class PlayerEventKind : uint8_t { Playing, Paused, Stopped, Seeked, NextTrack, PreviousTrack, TakenOver, Lost };

struct PlayerEvent {
    PlayerEventKind kind = PlayerEventKind::Stopped;
    uint64_t positionMs = 0; // Seeked
    bool operator==(const PlayerEvent&) const = default;
};

// What CamelPlayer gave the player: track URIs on UPnP AV, playlist IDs on OpenHome. Empty before anything is playing.
struct ExpectedTracks {
    std::string current, next, previous;
};

enum class TransportState : uint8_t { Playing, Paused, Stopped };

struct PlayerObservation {
    std::optional<TransportState> state;
    std::optional<std::string> track;
    std::optional<uint64_t> positionMs;
    bool otherSource = false;
};

// Turns what a player reports into what happened. After TakenOver or Lost it stays silent until tracks are expected again.
class PlayerFollower {
public:
    void expect(ExpectedTracks tracks);
    void expectPosition(uint64_t positionMs, std::chrono::milliseconds now);
    std::vector<PlayerEvent> observe(const PlayerObservation& observation, std::chrono::milliseconds now);
    std::vector<PlayerEvent> lost();

private:
    void followTrack(const std::string& track, std::vector<PlayerEvent>& events);

    ExpectedTracks expected_;
    std::optional<TransportState> state_;
    std::optional<uint64_t> position_;
    std::chrono::milliseconds positionAt_{};
    bool ended_ = false;
};

struct LinkOptions {
    std::chrono::milliseconds pollInterval{2000};
    int failuresUntilLost = 2;
    int subscribeSeconds = 1800;
    int requestTimeoutMs = 3000;
};

class PlayerLink {
public:
    // `onEvent` runs on the link's thread. Throws std::runtime_error when the player cannot be reached for events.
    PlayerLink(upnp::PlayerDescription player, std::function<void(const PlayerEvent&)> onEvent, LinkOptions options = {});
    ~PlayerLink();
    PlayerLink(const PlayerLink&) = delete;
    PlayerLink& operator=(const PlayerLink&) = delete;

    void expect(ExpectedTracks tracks);
    // False when the player refused or did not answer.
    bool play();
    bool pause();
    bool stop();
    bool seek(uint64_t positionMs);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace camel::server
