// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// HQPlayer as the output (docs/Plan.md §5.11): the stream of NetworkOutput, which HQPlayer fetches, played through
// HQPlayer's control protocol.
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "OutputBridge/IOutput.h"
#include "Server/Network/HQPlayer.h"
#include "Server/Network/NetworkOutput.h"

namespace camel::server {

struct HQPlayerOutputOptions {
    hqplayer::ClientOptions client;
    // A change of state, track or playlist that does not follow one of CamelPlayer's own commands within this long is
    // another source's.
    std::chrono::milliseconds ownChange{1500};
    // Play leaving the position still this long is sent once more: HQPlayer did not take the first, or its output
    // cannot open (about 1 s for PCM and 4 s for DSD measured to start).
    std::chrono::milliseconds playAgainAfter{5000};
    NetworkOutputOptions stream;
};

// What HQPlayer's Status shows of its own processing, for the signal path.
struct HQPlayerProcessing {
    uint32_t rate = 0, bits = 0;
    std::string mode, filter, shaper;
    bool operator==(const HQPlayerProcessing&) const = default;
};

// What HQPlayer's Status says of its output, for the signal path: "384 kHz/32-bit · poly-sinc-gauss-long · TPDF", or
// empty before it reports any.
std::string describeProcessing(const HQPlayerProcessing& p);

std::unique_ptr<audio::IOutput> createHQPlayerOutput(std::string address,
                                                     std::function<TrackDetails(const std::wstring& file)> details,
                                                     std::function<void(const PlayerEvent&)> follow,
                                                     HQPlayerOutputOptions options = {});

} // namespace camel::server
