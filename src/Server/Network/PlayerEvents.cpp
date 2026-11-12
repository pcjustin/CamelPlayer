// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Network/PlayerEvents.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <charconv>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "Common/util/Log.h"
#include "Server/Support/Http.h"

#pragma comment(lib, "ws2_32.lib")

namespace camel::server {

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kNotifyTimeoutMs = 2000;
// Nothing waits for the answer, which a Linkplay module takes 5 s to give; a subscription left behind just expires.
constexpr int kUnsubscribeTimeoutMs = 300;
constexpr SOCKET kNoSocket = INVALID_SOCKET;

uint64_t number(std::string_view text) {
    uint64_t value = 0;
    std::from_chars(text.data(), text.data() + text.size(), value);
    return value;
}

std::string headerValue(const std::string& head, const char* name) {
    const std::string wanted = std::string(name) + ":";
    size_t start = 0;
    while (start < head.size()) {
        auto end = head.find("\r\n", start);
        if (end == std::string::npos) end = head.size();
        if (end - start > wanted.size() && _strnicmp(head.c_str() + start, wanted.c_str(), wanted.size()) == 0) {
            auto value = head.substr(start + wanted.size(), end - start - wanted.size());
            value.erase(0, value.find_first_not_of(" \t"));
            return value;
        }
        start = end + 2;
    }
    return {};
}

} // namespace

struct PlayerEvents::Impl {
    struct Subscription {
        std::string kind, eventUrl, sid;
        Clock::time_point renewAt;
    };
    struct Received {
        std::string sid, body;
    };

    LinkOptions options;
    SOCKET listener = INVALID_SOCKET;
    std::string callback;
    std::vector<Subscription> subscriptions;
    std::mutex mutex; // the queue
    std::deque<Received> received;
    std::thread listening;

    Impl(const upnp::PlayerDescription& player, const std::vector<std::string>& kinds, LinkOptions linkOptions)
        : options(linkOptions) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        openListener(player);
        listening = std::thread([this] { listen(); });
        try {
            for (const auto& kind : kinds) {
                const auto* service = player.service(kind);
                if (!service || service->eventUrl.empty()) continue;
                subscriptions.push_back({kind, service->eventUrl, {}, {}});
                subscribe(subscriptions.back());
            }
            if (subscriptions.empty()) throw std::runtime_error("the player offers no events");
        } catch (...) {
            stopListening();
            throw;
        }
    }

    ~Impl() {
        for (const auto& subscription : subscriptions) {
            try {
                http::send("UNSUBSCRIBE", subscription.eventUrl, "SID: " + subscription.sid, "", kUnsubscribeTimeoutMs, 4096);
            } catch (const std::exception&) {
                log::info("network player: left a subscription to expire");
            }
        }
        stopListening();
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void stopListening() {
        closesocket(listener);
        listening.join();
        WSACleanup();
    }

    void openListener(const upnp::PlayerDescription& player) {
        const auto* first = player.services.empty() ? nullptr : &player.services.front();
        const std::string host = first ? upnp::hostOf(first->controlUrl) : std::string();
        const std::string local = upnp::localAddressFor(host);
        if (local.empty()) throw std::runtime_error("no route to the player");
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        inet_pton(AF_INET, local.c_str(), &address.sin_addr);
        if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(listener, 8) != 0) {
            closesocket(listener);
            throw std::runtime_error("cannot listen for the player's events");
        }
        int length = sizeof(address);
        getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
        callback = "http://" + local + ":" + std::to_string(ntohs(address.sin_port)) + "/event";
    }

    void subscribe(Subscription& subscription) const {
        const auto reply = http::send("SUBSCRIBE", subscription.eventUrl,
                                      "CALLBACK: <" + callback + ">\r\nNT: upnp:event\r\nTIMEOUT: Second-" +
                                          std::to_string(options.subscribeSeconds),
                                      "", options.requestTimeoutMs, upnp::kMaxMessageBytes);
        const auto sid = headerValue(reply.headers, "SID");
        if (reply.status != 200 || sid.empty()) throw std::runtime_error("the player refused a subscription");
        subscription.sid = sid;
        subscription.renewAt = renewalTime(reply.headers);
    }

    // Halfway through what the player granted, which may be less than what was asked. A grant without end, as
    // Second-infinite, is taken for what was asked.
    Clock::time_point renewalTime(const std::string& head) const {
        const auto timeout = headerValue(head, "TIMEOUT");
        int seconds = options.subscribeSeconds;
        if (timeout.starts_with("Second-")) {
            const auto digits = std::string_view(timeout).substr(7);
            uint64_t granted = 0;
            const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), granted);
            if (error == std::errc() && end != digits.data())
                seconds = int(std::clamp<uint64_t>(granted, 2, uint64_t(options.subscribeSeconds)));
        }
        return Clock::now() + std::chrono::seconds(seconds) / 2;
    }

    void renew(Subscription& subscription) const {
        const auto reply =
            http::send("SUBSCRIBE", subscription.eventUrl,
                       "SID: " + subscription.sid + "\r\nTIMEOUT: Second-" + std::to_string(options.subscribeSeconds), "",
                       options.requestTimeoutMs, upnp::kMaxMessageBytes);
        if (reply.status == 200) {
            subscription.renewAt = renewalTime(reply.headers);
            return;
        }
        // The player forgot the subscription, as after a restart.
        subscribe(subscription);
    }

    void listen() {
        for (;;) {
            const SOCKET client = accept(listener, nullptr, nullptr);
            if (client == kNoSocket) return;
            const DWORD timeout = kNotifyTimeoutMs;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            std::string request;
            char buffer[4096];
            size_t headEnd = std::string::npos;
            size_t total = 0;
            while (request.size() < upnp::kMaxMessageBytes) {
                const int read = recv(client, buffer, sizeof(buffer), 0);
                if (read <= 0) break;
                request.append(buffer, size_t(read));
                if (headEnd == std::string::npos) {
                    headEnd = request.find("\r\n\r\n");
                    if (headEnd != std::string::npos)
                        total = headEnd + 4 + size_t(number(headerValue(request.substr(0, headEnd), "CONTENT-LENGTH")));
                }
                if (headEnd != std::string::npos && request.size() >= total) break;
            }
            bool accepted = false;
            if (headEnd != std::string::npos && request.starts_with("NOTIFY ")) {
                const auto head = request.substr(0, headEnd);
                std::scoped_lock lock(mutex);
                received.push_back({headerValue(head, "SID"), request.substr(headEnd + 4)});
                accepted = true;
            }
            const std::string reply = accepted
                                          ? "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
                                          : "HTTP/1.1 412 Precondition Failed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send(client, reply.data(), int(reply.size()), 0);
            shutdown(client, SD_SEND);
            closesocket(client);
        }
    }
};

PlayerEvents::PlayerEvents(const upnp::PlayerDescription& player, const std::vector<std::string>& kinds, LinkOptions options)
    : impl_(std::make_unique<Impl>(player, kinds, options)) {}

PlayerEvents::~PlayerEvents() = default;

std::deque<PlayerEvents::Notification> PlayerEvents::take() {
    std::deque<Impl::Received> received;
    {
        std::scoped_lock lock(impl_->mutex);
        received.swap(impl_->received);
    }
    std::deque<Notification> notifications;
    for (auto& item : received) {
        const auto subscribed = std::ranges::find(impl_->subscriptions, item.sid, &Impl::Subscription::sid);
        if (subscribed != impl_->subscriptions.end()) notifications.push_back({subscribed->kind, std::move(item.body)});
    }
    return notifications;
}

void PlayerEvents::renewDue() {
    for (auto& subscription : impl_->subscriptions)
        if (Clock::now() >= subscription.renewAt) impl_->renew(subscription);
}

} // namespace camel::server
