// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// What a stream served over HTTP plays on (docs/Plan.md §5.10, §5.11): a UPnP AV or OpenHome player, or HQPlayer. The
// stream itself, its bodies, range requests and gapless changes, is NetworkOutput's.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "Server/Network/Upnp.h"

namespace camel::server {

struct PlayerEvent;

// Controlling the player while it plays the stream; its own changes and its loss come as PlayerEvents.
class PlayerControl {
public:
    virtual ~PlayerControl() = default;
    // False when the player refused or did not answer.
    virtual bool load(const std::string& uri, const std::string& metadata) = 0;
    virtual bool loadNext(const std::string& uri, const std::string& metadata) = 0;
    virtual void advance() = 0; // the stream moved on to the next track without the player saying so
    virtual bool play() = 0;
    virtual bool pause() = 0;
    virtual bool stop() = 0;
    virtual uint64_t positionMs() const = 0; // in the current track, as last reported
    virtual bool stopped() const = 0;
    virtual bool playing() const = 0;
    virtual bool idle() const = 0;   // stopped or without media, whether or not it played since the last load
    virtual bool played() const = 0; // reported playing since the last load
    virtual bool lost() const = 0;   // stopped answering; it is followed no more
};

// The player itself: what it takes, where it can reach this computer, and its control.
class StreamPlayer {
public:
    virtual ~StreamPlayer() = default;
    virtual std::string name() const = 0;
    virtual std::string mode() const = 0; // OutputStatus::mode, e.g. "UPnP · Living room"
    // OutputStatus::detail: what the player reports of its own processing, if anything.
    virtual std::string detail() const { return {}; }
    // Reports its position from the start of a track, so that a position standing at 0 is not yet played.
    virtual bool reportsPosition() const { return false; }
    // The formats it takes and whether it follows one track with the next; `known` false while it cannot tell yet.
    virtual upnp::PlayerFormats formats(bool& known) = 0;
    virtual bool gapless(bool& known) = 0;
    // A socket listening where the player reaches this computer. Throws audio::Error.
    virtual upnp::Listener listen() = 0;
    // The metadata `load` takes for a track.
    virtual std::string metadata(const upnp::DidlTrack& track) const = 0;
    // Throws audio::Error when it cannot be reached.
    virtual std::unique_ptr<PlayerControl> connect(std::function<void(const PlayerEvent&)> onEvent) = 0;
    // What a stop that is neither ours nor the track's end means, and what a player that never started means.
    virtual std::string inUseByAnother() const { return "the player is in use by another source"; }
    virtual std::string didNotStart() const { return "the player did not start playing"; }
    virtual std::string notConnected() const { return "the network player is not connected"; }
};

} // namespace camel::server
