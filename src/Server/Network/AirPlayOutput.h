// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The output that sends to an AirPlay 1 receiver (docs/Plan.md §5.10): 44.1 kHz/16-bit stereo as uncompressed ALAC over
// RTP, unencrypted. This computer's clock is the master: packets leave at 44.1 kHz and the receiver plays them a fixed
// latency later. A pause, a seek and a new track end in a FLUSH and go on in the same RTSP session, which is closed once
// nothing has been sent for a few seconds.
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "OutputBridge/IOutput.h"
#include "Server/Network/AirPlay.h"
#include "Server/Network/NetworkOutput.h"

namespace camel::server {

struct AirPlayOutputOptions {
    std::function<TrackDetails(const std::wstring& file)> details;
    std::function<std::optional<double>()> volumeDb; // what to tell the receiver; nothing while its volume is fixed
    int requestTimeoutMs = 3000;
    std::chrono::milliseconds heartbeat{5000};
    std::chrono::milliseconds closeSessionAfter{5000};
};

// The options' functions run on the output's own thread.
std::unique_ptr<audio::IOutput> createAirPlayOutput(airplay::Receiver receiver, AirPlayOutputOptions options);

} // namespace camel::server
