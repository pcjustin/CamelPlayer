// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The output that streams to a UPnP AV or OpenHome player (docs/Plan.md §5.10). The Engine renders as for any device;
// the player fetches one URL per track from this output's HTTP server, as WAV, L16 or L24, or raw DSD as DSF. Rendering
// stays a few seconds ahead of what the player has played, so a change is heard about a second after it is asked for.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "OutputBridge/IOutput.h"
#include "Server/Network/PlayerLink.h"
#include "Server/Network/Upnp.h"

namespace camel::server {

struct TrackDetails {
    std::string title, artist, album;
    std::string coverJpeg; // up to 512 px; empty without a cover
};

struct NetworkOutputOptions {
    LinkOptions link{std::chrono::milliseconds(500), 3, 1800, 3000};
    std::chrono::milliseconds ahead{5000};            // rendered past what the player has played
    std::chrono::milliseconds aheadBeforePlay{30000}; // for a player that fills its buffer before it starts
    // Rendered ahead once a next track is queued, so that the player has time to open that track before the change.
    std::chrono::milliseconds nextTrackLead{20000};
};

// `details` and `follow` run on the output's own threads. `follow` gets what the player did by itself.
std::unique_ptr<audio::IOutput> createNetworkOutput(upnp::PlayerDescription player,
                                                    std::function<TrackDetails(const std::wstring& file)> details,
                                                    std::function<void(const PlayerEvent&)> follow,
                                                    NetworkOutputOptions options = {});

} // namespace camel::server
