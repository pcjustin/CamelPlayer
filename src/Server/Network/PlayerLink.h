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

#include "Server/Network/PlayerControl.h"
#include "Server/Network/PlayerEvents.h"
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
    // Just given to the player, which may report what it played before for a moment.
    void expectLoaded(ExpectedTracks tracks, std::chrono::milliseconds now);
    void expectNext(std::string track);
    // The player moved on to the next track without saying so; what it reported until now means nothing more.
    void advance();
    std::vector<PlayerEvent> observe(const PlayerObservation& observation, std::chrono::milliseconds now);
    std::vector<PlayerEvent> lost();

private:
    struct Report {
        uint64_t positionMs = 0;
        std::chrono::milliseconds at{};
    };

    void followTrack(const std::string& track, std::chrono::milliseconds now, std::vector<PlayerEvent>& events);
    void followPosition(const Report& report, std::vector<PlayerEvent>& events);
    bool foresaw(const std::optional<Report>& from, const Report& report) const;
    void forgetPosition();

    ExpectedTracks expected_;
    std::string left_;     // the track `advance` moved on from
    std::string justLeft_; // the track a change the player reported left, which it may report a while longer
    std::chrono::milliseconds leftUntil_{};
    std::optional<TransportState> state_;
    std::optional<Report> position_; // where the player is
    std::optional<Report> before_;   // the report before it, which is where the player is when that one came late
    std::optional<Report> jump_;     // off both, a seek once the next report bears it out
    bool ended_ = false;
    std::chrono::milliseconds settledAt_{};
};

class PlayerLink final : public PlayerControl {
public:
    // `onEvent` runs on the link's thread. Throws std::runtime_error when the player cannot be reached for events.
    PlayerLink(upnp::PlayerDescription player, std::function<void(const PlayerEvent&)> onEvent, LinkOptions options = {});
    ~PlayerLink() override;
    PlayerLink(const PlayerLink&) = delete;
    PlayerLink& operator=(const PlayerLink&) = delete;

    void expect(ExpectedTracks tracks);
    // False when the player refused or did not answer.
    LoadOutcome load(const std::string& uri, const std::string& metadata) override;
    bool loadNext(const std::string& uri, const std::string& metadata) override;
    void advance() override;
    bool play() override;
    bool pause() override;
    bool stop() override;
    uint64_t positionMs() const override; // in the current track, as last polled
    bool stopped() const override;        // as last polled
    bool playing() const override;        // as last polled
    bool idle() const override;           // stopped or without media as last polled, whether or not it played since the last load
    bool played() const override;         // reported playing since the last load
    bool lost() const override;           // the player stopped answering; the link follows it no more

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace camel::server
