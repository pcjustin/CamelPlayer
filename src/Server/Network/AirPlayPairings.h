// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// What CamelPlayer keeps to play AirPlay receivers that ask for a password or a code (docs/Plan.md §5.10): this
// computer's pairing identity, and for each receiver the password it took or the keys pair-setup left. One file, protected
// with DPAPI for the Windows user; without a file it is kept in memory.
#pragma once

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "Server/Network/AirPlay.h"
#include "Server/Network/Pairing.h"
#include "Server/Network/Rtsp.h"

namespace camel::server {

struct AirPlayCredentials {
    std::string password;                        // played after transient pairing with it
    std::optional<airplay::PairedReceiver> keys; // played after pair-verify
    bool operator==(const AirPlayCredentials&) const = default;
};

class AirPlayPairings {
public:
    explicit AirPlayPairings(std::filesystem::path file = {});

    airplay::PairingIdentity identity();
    std::optional<AirPlayCredentials> find(const std::string& receiver) const;
    void save(const std::string& receiver, AirPlayCredentials credentials);
    void forget(const std::string& receiver);

private:
    void load();
    void store() const;

    mutable std::mutex mutex_;
    std::filesystem::path file_;
    airplay::PairingIdentity identity_;
    std::map<std::string, AirPlayCredentials> receivers_;
};

// Pairs `client` as the receiver needs before a session: with its keys or its password when kept, transiently when it
// takes that, else not at all. Keys it no longer takes count as a wrong code.
airplay::PairOutcome pairFor(airplay::RtspClient& client, const airplay::Receiver& receiver, AirPlayPairings* pairings);

} // namespace camel::server
