// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// HQPlayer's own volume (docs/Plan.md §6): offered only while HQPlayer has it enabled, its range read from VolumeRange
// and its level followed through Status, on a connection of its own that leaves the playlist alone. The 0-100 level
// spans HQPlayer's range in dB evenly.
#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include "Server/Network/HQPlayer.h"
#include "Server/Network/PlayerVolume.h"

namespace camel::server {

struct HQPlayerVolumeOptions {
    hqplayer::ClientOptions client{.clearOnConnect = false};
    std::chrono::milliseconds rangeEvery{10000}; // the range read again, as volume may be turned on in HQPlayer later
    std::chrono::milliseconds retryAfter{5000};  // after HQPlayer could not be reached
    std::chrono::milliseconds settling{1500};    // a Status this soon after a write may not show it yet
};

class HQPlayerVolume final : public OwnVolume {
public:
    // Talks to HQPlayer on its own thread, so nothing here waits for it.
    explicit HQPlayerVolume(std::string address, HQPlayerVolumeOptions options = {});
    ~HQPlayerVolume() override;
    HQPlayerVolume(const HQPlayerVolume&) = delete;
    HQPlayerVolume& operator=(const HQPlayerVolume&) = delete;

    std::optional<PlayerLevel> current() const override;
    void setLevel(uint32_t level) override;
    // Muting sets the range's minimum and keeps the level, which unmuting sets again: a second VolumeMute does not
    // restore it.
    void setMuted(bool muted) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace camel::server
