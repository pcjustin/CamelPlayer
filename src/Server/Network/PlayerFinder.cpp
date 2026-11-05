// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Network/PlayerFinder.h"

#include <winsock2.h>
#include <ws2tcpip.h>
// ws2tcpip.h comes first
#include <iphlpapi.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>

#include "Common/util/Log.h"
#include "Server/Support/Http.h"

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace camel::server {

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kDescriptionTimeoutMs = 3000;
constexpr auto kPollSockets = std::chrono::milliseconds(100);
constexpr int kRetryAfterSearches = 10; // an address that did not answer waits this long, as asking it takes seconds
constexpr size_t kMaxRefusedAddresses = 256;

std::vector<in_addr> localAddresses() {
    std::vector<in_addr> addresses;
    ULONG size = 16 * 1024;
    std::vector<char> buffer(size);
    auto* list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size) != NO_ERROR) return addresses;
    for (auto* adapter = list; adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next)
            addresses.push_back(reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr)->sin_addr);
    }
    return addresses;
}

std::optional<upnp::PlayerDescription> fetchDescription(const std::string& location) {
    if (!upnp::isLanUrl(location)) return std::nullopt;
    try {
        const auto reply = http::send("GET", location, "", "", kDescriptionTimeoutMs, upnp::kMaxMessageBytes);
        if (reply.status == 200) return upnp::parseDescription(reply.body, location);
    } catch (const std::exception& e) {
        log::info("network player at %s: %s", location.c_str(), e.what());
    }
    return std::nullopt;
}

} // namespace

struct PlayerFinder::Impl {
    struct Known {
        FoundPlayer player;
        std::vector<std::string> locations; // newest first
        Clock::time_point seen;
    };

    FinderOptions options;
    std::function<void()> onChange;
    SOCKET searcher = INVALID_SOCKET, listener = INVALID_SOCKET;
    uint16_t boundNotifyPort = 0;
    mutable std::mutex mutex;
    std::vector<Known> known;
    std::map<std::string, Clock::time_point> refused; // only the finder's thread
    std::atomic<bool> stopping = false, searchWanted = true;
    std::thread thread;

    Impl(FinderOptions finderOptions, std::function<void()> changed)
        : options(std::move(finderOptions)), onChange(std::move(changed)) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        searcher = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        if (bind(searcher, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
            log::warn("network players: cannot search (%d)", WSAGetLastError());

        listener = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        const BOOL reuse = TRUE;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        address.sin_port = htons(options.notifyPort);
        if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
            log::warn("network players: cannot listen for announcements (%d)", WSAGetLastError());
        int length = sizeof(address);
        getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
        boundNotifyPort = ntohs(address.sin_port);
        if (options.notifyPort != 0)
            for (const auto& local : localAddresses()) {
                ip_mreq group{};
                inet_pton(AF_INET, options.searchHost.c_str(), &group.imr_multiaddr);
                group.imr_interface = local;
                setsockopt(listener, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&group), sizeof(group));
            }
        thread = std::thread([this] { run(); });
    }

    ~Impl() {
        stopping = true;
        thread.join();
        closesocket(searcher);
        closesocket(listener);
        WSACleanup();
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void sendSearch() {
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(options.searchPort);
        inet_pton(AF_INET, options.searchHost.c_str(), &target.sin_addr);
        const auto send = [&] {
            for (const auto& request : upnp::searchRequests(options.searchHost, options.searchPort))
                sendto(searcher, request.data(), int(request.size()), 0, reinterpret_cast<const sockaddr*>(&target),
                       sizeof(target));
        };
        send();
        if ((ntohl(target.sin_addr.s_addr) >> 28) != 0xE) return; // a test's unicast address
        for (const auto& local : localAddresses()) {
            setsockopt(searcher, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&local), sizeof(local));
            send();
        }
    }

    void changed() {
        if (onChange) onChange();
    }

    void forget(const std::string& udn) {
        size_t removed = 0;
        {
            std::scoped_lock lock(mutex);
            removed = std::erase_if(known, [&](const Known& k) { return k.player.description.udn == udn; });
        }
        if (removed) changed();
    }

    void remember(const upnp::SsdpMessage& message) {
        {
            std::scoped_lock lock(mutex);
            const auto found =
                std::ranges::find_if(known, [&](const Known& k) { return k.player.description.udn == message.udn; });
            if (found != known.end()) {
                found->seen = Clock::now();
                if (std::ranges::find(found->locations, message.location) != found->locations.end()) return;
            }
        }
        if (const auto failed = refused.find(message.location);
            failed != refused.end() && Clock::now() - failed->second < kRetryAfterSearches * options.searchInterval)
            return;
        // Outside the lock, as a player that does not answer takes seconds.
        auto description = fetchDescription(message.location);
        if (!description || description->udn != message.udn) {
            if (refused.size() >= kMaxRefusedAddresses) refused.clear();
            refused[message.location] = Clock::now();
            return;
        }
        refused.erase(message.location);
        {
            std::scoped_lock lock(mutex);
            auto found = std::ranges::find_if(known, [&](const Known& k) { return k.player.description.udn == message.udn; });
            if (found == known.end()) found = known.insert(known.end(), Known{});
            found->player = {std::move(*description), message.location};
            found->locations.insert(found->locations.begin(), message.location);
            found->seen = Clock::now();
        }
        changed();
    }

    void receive(SOCKET from) {
        char buffer[8192];
        const int length = recv(from, buffer, sizeof(buffer), 0);
        if (length <= 0) return;
        const auto message = upnp::parseSsdp(std::string_view(buffer, size_t(length)));
        if (!message) return;
        if (message->kind == upnp::SsdpKind::ByeBye)
            forget(message->udn);
        else
            remember(*message);
    }

    // A player that stopped answering searches is asked directly, at any address it has had, before it is forgotten.
    void checkSilentPlayers() {
        std::vector<Known> silent;
        const auto limit = Clock::now() - 2 * options.searchInterval;
        {
            std::scoped_lock lock(mutex);
            std::ranges::copy_if(known, std::back_inserter(silent), [&](const Known& k) { return k.seen < limit; });
        }
        for (const auto& player : silent) {
            std::optional<upnp::PlayerDescription> description;
            std::string location;
            for (const auto& candidate : player.locations) {
                description = fetchDescription(candidate);
                if (description) {
                    location = candidate;
                    break;
                }
            }
            const auto& udn = player.player.description.udn;
            if (!description || description->udn != udn) {
                forget(udn);
                continue;
            }
            bool moved = false;
            {
                std::scoped_lock lock(mutex);
                const auto found = std::ranges::find_if(known, [&](const Known& k) { return k.player.description.udn == udn; });
                if (found != known.end()) {
                    found->seen = Clock::now();
                    moved = found->player.location != location;
                    found->player = {std::move(*description), location};
                }
            }
            if (moved) changed();
        }
    }

    void run() {
        auto nextSearch = Clock::now();
        while (!stopping) {
            if (searchWanted.exchange(false) || Clock::now() >= nextSearch) {
                sendSearch();
                checkSilentPlayers();
                nextSearch = Clock::now() + options.searchInterval;
            }
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(searcher, &readable);
            FD_SET(listener, &readable);
            const timeval wait{0, long(std::chrono::microseconds(kPollSockets).count())};
            if (select(0, &readable, nullptr, nullptr, &wait) <= 0) continue;
            if (FD_ISSET(searcher, &readable)) receive(searcher);
            if (FD_ISSET(listener, &readable)) receive(listener);
        }
    }
};

PlayerFinder::PlayerFinder(FinderOptions options, std::function<void()> onChange)
    : impl_(std::make_unique<Impl>(std::move(options), std::move(onChange))) {}

PlayerFinder::~PlayerFinder() = default;

std::vector<FoundPlayer> PlayerFinder::players() const {
    std::scoped_lock lock(impl_->mutex);
    std::vector<FoundPlayer> players;
    players.reserve(impl_->known.size());
    for (const auto& known : impl_->known) players.push_back(known.player);
    return players;
}

void PlayerFinder::search() {
    impl_->searchWanted = true;
}

uint16_t PlayerFinder::notifyPort() const {
    return impl_->boundNotifyPort;
}

} // namespace camel::server
