// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// HQPlayer's control protocol (docs/Plan.md §5.11, docs/reports/HQPlayer-control-2026-11-16.md): XML documents over one
// TCP connection to port 4321, and HQPlayers found by a multicast datagram, as HQPlayer's own Client finds them.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Server/Network/Upnp.h"

namespace camel::server::hqplayer {

inline constexpr uint16_t kDefaultPort = 4321;

// "host:port", an IPv6 host in brackets.
std::string addressOf(std::string_view host, uint16_t port);
// False when `address` has no host or its port is not a number from 1 to 65535.
bool splitAddress(std::string_view address, std::string& host, uint16_t& port);
// localhost, 127.x.x.x, ::1 or one of this computer's own addresses.
bool isLocalHost(std::string_view host);
// The host's first IPv4 address, a name resolved; empty when it has none.
std::string ipv4Of(const std::string& host);

// Takes every whole document off the front of `buffer`, leaving a document still arriving. Text that is not XML before a
// document is dropped.
std::vector<std::string> takeDocuments(std::string& buffer);

std::string escape(std::string_view text);

struct Status {
    int state = 0; // 0 stopped, 1 paused, 2 playing
    uint32_t track = 0, trackSerial = 0, transportSerial = 0, tracksTotal = 0;
    double position = 0.0, length = 0.0; // seconds
    double volume = 0.0;                 // dB
    uint32_t activeRate = 0, activeBits = 0;
    std::string activeMode, activeFilter, activeShaper;
    bool operator==(const Status&) const = default;
};
Status parseStatus(const upnp::XmlNode& element);

struct VolumeRange {
    bool enabled = false;
    double minDb = 0.0, maxDb = 0.0;
};

struct Found {
    std::string address; // host:port, "localhost" for this computer
    std::string name, version;
    bool operator==(const Found&) const = default;
};
std::string discoverRequest();
// Nothing for anything but an answer that HQPlayer is there.
std::optional<Found> parseDiscoverReply(std::string_view datagram, const std::string& host, uint16_t port);

struct ClientOptions {
    std::chrono::milliseconds connectTimeout{3000};
    std::chrono::milliseconds firstReply{10000}; // 3-4 s measured, 5.4 s just after HQPlayer started
    std::chrono::milliseconds reply{5000};
};

// One connection, kept open: each command waits for the reply of its own name, pushed Status documents going to
// `onStatus` instead. Every new connection starts with Stop and PlaylistClear, as a restarted HQPlayer plays on from its
// last playlist, and subscribes to Status.
class Client {
public:
    // `onStatus` runs on the client's reader thread; keep it short. `onLost` runs there too when an open connection
    // breaks.
    Client(std::string address, std::function<void(const Status&)> onStatus, std::function<void()> onLost,
           ClientOptions options = {});
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Connects unless connected. Throws audio::Error: DeviceNotFound when HQPlayer cannot be reached or did not answer,
    // AccessDenied when one on another computer stays silent or refuses, as with "Allow control from network" off.
    void connect();
    // The reply to `command`, an element without its XML declaration. A missing reply or a broken connection connects
    // again once and sends it again; throws audio::Error when that fails too.
    upnp::XmlNode send(const std::string& command);
    // Sends and checks the reply's `result`: false for anything but OK.
    bool ok(const std::string& command);
    void disconnect();
    bool connected() const;
    std::optional<Status> status() const;
    uint64_t connections() const; // made so far: a new one means HQPlayer's playlist was cleared

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct FinderOptions {
    std::string group = "239.192.0.199";
    uint16_t port = kDefaultPort;
    std::chrono::milliseconds searchInterval{30000};
    // A found HQPlayer that has not answered for this long is gone.
    std::chrono::milliseconds forgetAfter{75000};
    std::chrono::milliseconds probeTimeout{1000};
    // This computer's own HQPlayer is checked like one added, so that it is listed without being added even with
    // "Allow control from network" off, when it answers no search; 0 checks none.
    uint16_t localPort = kDefaultPort;
};

// Asks the network for HQPlayers now and then; only those with "Allow control from network" on answer.
class Finder {
public:
    Finder(FinderOptions options, std::function<void()> onChange);
    ~Finder();
    Finder(const Finder&) = delete;
    Finder& operator=(const Finder&) = delete;

    // Those that answered, then the watched addresses something listens at, without a name.
    std::vector<Found> found() const;
    void search();
    // HQPlayers added by address, checked at each search with this computer's own: one with network control off answers
    // no search.
    void watch(std::vector<std::string> addresses);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Whether something listens at `address`, within `timeout`: how an HQPlayer added by address shows as online.
bool reachable(const std::string& address, std::chrono::milliseconds timeout);

} // namespace camel::server::hqplayer
