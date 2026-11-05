// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Finds AirPlay 1 receivers through mDNS (docs/Plan.md §5.10). A receiver is known by its device ID, so one that comes
// back at a new address is the same receiver. A declared `et=0` does not show that it takes an unencrypted session, so
// each is asked OPTIONS when found, which starts nothing, and for a session when the user enables it, as that could
// interrupt whoever is playing to it.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Server/Network/AirPlay.h"

namespace camel::server {

struct AirPlayFinderOptions {
    std::string queryHost = "224.0.0.251";
    uint16_t queryPort = 5353;
    std::chrono::milliseconds searchInterval{60000};
    std::chrono::milliseconds retryInterval{1000}; // between the queries of a search, as answers get lost
    int sessionTimeoutMs = 3000;
};

class AirPlayFinder {
public:
    // `onChange` runs on the finder's thread whenever the list changes; keep it short.
    AirPlayFinder(AirPlayFinderOptions options, std::function<void()> onChange);
    ~AirPlayFinder();
    AirPlayFinder(const AirPlayFinder&) = delete;
    AirPlayFinder& operator=(const AirPlayFinder&) = delete;

    std::vector<airplay::Receiver> receivers() const;
    void search();                     // now, rather than at the next interval
    void check(const std::string& id); // asks the receiver for a session, which is closed at once and carries no audio

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace camel::server
