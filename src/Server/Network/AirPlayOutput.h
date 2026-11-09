// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The output that sends to AirPlay 1 receivers (docs/Plan.md §5.10): 44.1 kHz/16-bit stereo as uncompressed ALAC over
// RTP, unencrypted. This computer's clock is the master: packets leave at 44.1 kHz and every receiver plays them a fixed
// latency later, so receivers sent the same stream play in step. A pause, a seek and a new track end in a FLUSH and go on
// in the same RTSP sessions, which are closed once nothing has been sent for a few seconds. Each speaker of a stereo pair
// has its own session and gets the same packets. A group's receivers come and go while it plays: one added is sent the
// stream from the next packet and is heard in step once those packets are due, about two seconds later.
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Common/settings/Settings.h"
#include "OutputBridge/IOutput.h"
#include "Server/Network/AirPlay.h"
#include "Server/Network/NetworkOutput.h"

namespace camel::server {

// A receiver of the group as the Server wants it now.
struct AirPlayMember {
    std::string id;
    std::optional<airplay::Receiver> receiver; // nothing while it is not found: one playing stays, one missing waits
    AirPlayChannels channels = AirPlayChannels::Stereo;
    std::optional<double> volumeDb; // what to tell the receiver; nothing while its volume is fixed
    bool operator==(const AirPlayMember&) const = default;
};

struct AirPlayOutputOptions {
    std::function<TrackDetails(const std::wstring& file)> details;
    std::function<std::optional<double>()> volumeDb;     // the one receiver's, when the output is made for one
    std::function<std::vector<AirPlayMember>()> members; // a group's, asked again as it plays
    // A receiver that left the group while the others play on: taken by another source, gone or refusing.
    std::function<void(const std::string& id, const audio::Error& why)> memberLeft;
    int requestTimeoutMs = 3000;
    std::chrono::milliseconds heartbeat{5000};
    std::chrono::milliseconds closeSessionAfter{5000};
};

// The options' functions run on the output's own threads.
std::unique_ptr<audio::IOutput> createAirPlayOutput(airplay::Receiver receiver, AirPlayOutputOptions options);
std::unique_ptr<audio::IOutput> createAirPlayGroupOutput(AirPlayOutputOptions options);

} // namespace camel::server
