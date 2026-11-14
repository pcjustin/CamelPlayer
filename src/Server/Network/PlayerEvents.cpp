// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Network/PlayerEvents.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <charconv>
#include <mutex>
#include <optional>
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

// The body of a chunked message once its last chunk has come; nothing before, or when it is not chunked as it says.
std::optional<std::string> dechunked(std::string_view data) {
    std::string body;
    size_t at = 0;
    for (;;) {
        const auto lineEnd = data.find("\r\n", at);
        if (lineEnd == std::string_view::npos) return std::nullopt;
        uint64_t size = 0;
        const auto [end, error] = std::from_chars(data.data() + at, data.data() + lineEnd, size, 16);
        if (error != std::errc() || end == data.data() + at) return std::nullopt;
        at = lineEnd + 2;
        if (size == 0) return body;
        if (data.size() - at < size + 2) return std::nullopt;
        body.append(data.substr(at, size_t(size)));
        at += size_t(size) + 2;
    }
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
        try {
            openListener(player);
        } catch (...) {
            WSACleanup();
            throw;
        }
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
            unsubscribe();
            stopListening();
            throw;
        }
    }

    ~Impl() {
        unsubscribe();
        stopListening();
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void unsubscribe() const {
        for (const auto& subscription : subscriptions) {
            if (subscription.sid.empty()) continue;
            try {
                http::send("UNSUBSCRIBE", subscription.eventUrl, "SID: " + subscription.sid, "", kUnsubscribeTimeoutMs, 4096);
            } catch (const std::exception&) {
                log::info("network player: left a subscription to expire");
            }
        }
    }

    void stopListening() {
        closesocket(listener);
        listening.join();
        WSACleanup();
    }

    void openListener(const upnp::PlayerDescription& player) {
        upnp::Listener bound;
        try {
            bound = upnp::listenFor(player);
        } catch (const upnp::NoRoute&) {
            throw;
        } catch (const std::runtime_error&) {
            throw std::runtime_error("cannot listen for the player's events");
        }
        listener = SOCKET(bound.socket);
        callback = bound.base + "/event";
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
            bool chunked = false;
            std::optional<std::string> body;
            while (request.size() < upnp::kMaxMessageBytes) {
                const int read = recv(client, buffer, sizeof(buffer), 0);
                if (read <= 0) break;
                request.append(buffer, size_t(read));
                if (headEnd == std::string::npos) {
                    headEnd = request.find("\r\n\r\n");
                    if (headEnd == std::string::npos) continue;
                    const auto head = request.substr(0, headEnd);
                    chunked = _stricmp(headerValue(head, "TRANSFER-ENCODING").c_str(), "chunked") == 0;
                    total = headEnd + 4 + size_t(number(headerValue(head, "CONTENT-LENGTH")));
                }
                if (chunked) {
                    body = dechunked(std::string_view(request).substr(headEnd + 4));
                    if (body) break;
                } else if (request.size() >= total) {
                    break;
                }
            }
            if (headEnd != std::string::npos && !chunked) body = request.substr(headEnd + 4);
            bool accepted = false;
            if (body && request.starts_with("NOTIFY ")) {
                const auto head = request.substr(0, headEnd);
                std::scoped_lock lock(mutex);
                received.push_back({headerValue(head, "SID"), std::move(*body)});
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
