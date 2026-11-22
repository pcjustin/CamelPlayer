// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Finds UPnP AV and OpenHome players through SSDP (docs/Plan.md §5.10). A player is known by its UDN, so one with several
// addresses is listed once and one that comes back at a new address is the same player.
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Server/Network/Upnp.h"

namespace camel::server {

struct FinderOptions {
    std::string searchHost = "239.255.255.250";
    uint16_t searchPort = 1900;
    uint16_t notifyPort = 1900; // 0 picks a free one, without joining the multicast group
    std::chrono::milliseconds searchInterval{60000};
    std::vector<uint16_t> moreSearchPorts; // tests: other ports at searchHost searched too, each a player of its own
    // The MAC address of the player at a location; empty for none. By default the ARP table's for its address.
    std::function<std::string(const std::string& location)> macOf;
};

// The MAC address of the host at `location` from the ARP table, asking for it when it is not there; empty for none, as on
// the loopback interface.
std::string arpMacOf(const std::string& location);

struct FoundPlayer {
    upnp::PlayerDescription description;
    std::string location;
    std::string mac; // its MAC address; empty when unknown
    bool operator==(const FoundPlayer&) const = default;
};

class PlayerFinder {
public:
    // `onChange` runs on the finder's thread whenever the list changes; keep it short.
    PlayerFinder(FinderOptions options, std::function<void()> onChange);
    ~PlayerFinder();
    PlayerFinder(const PlayerFinder&) = delete;
    PlayerFinder& operator=(const PlayerFinder&) = delete;

    std::vector<FoundPlayer> players() const;
    void search();               // now, rather than at the next interval
    uint16_t notifyPort() const; // where NOTIFY messages arrive

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace camel::server
