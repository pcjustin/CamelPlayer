// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Network/PlayerLink.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "Common/util/Log.h"
#include "Server/Support/Http.h"

#pragma comment(lib, "ws2_32.lib")

namespace camel::server {

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;
using Values = std::map<std::string, std::string>;

constexpr uint64_t kSeekMarginMs = 2000; // polls and events lag, so a smaller jump is not a seek
constexpr int kNotifyTimeoutMs = 2000;
constexpr auto kTick = milliseconds(25);
constexpr auto kSettling = milliseconds(3000); // a player can still report the old track this long after a new one
constexpr SOCKET kNoSocket = INVALID_SOCKET;

std::optional<TransportState> stateFrom(const std::string& text) {
    if (text == "PLAYING" || text == "Playing") return TransportState::Playing;
    if (text == "PAUSED_PLAYBACK" || text == "Paused") return TransportState::Paused;
    if (text == "STOPPED" || text == "NO_MEDIA_PRESENT" || text == "Stopped") return TransportState::Stopped;
    return std::nullopt; // TRANSITIONING, Buffering: nothing to follow yet
}

uint64_t number(std::string_view text) {
    uint64_t value = 0;
    std::from_chars(text.data(), text.data() + text.size(), value);
    return value;
}

std::string find(const Values& values, const char* name) {
    const auto found = values.find(name);
    return found == values.end() ? std::string() : found->second;
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

void PlayerFollower::expect(ExpectedTracks tracks) {
    expected_ = std::move(tracks);
    left_.clear();
    ended_ = false;
    position_.reset();
}

void PlayerFollower::expectLoaded(ExpectedTracks tracks, milliseconds now) {
    expect(std::move(tracks));
    settledAt_ = now + kSettling;
}

void PlayerFollower::expectNext(std::string track) {
    expected_.next = std::move(track);
}

void PlayerFollower::advance() {
    left_ = expected_.current;
    expected_ = {expected_.next, {}, {}};
    position_.reset();
}

void PlayerFollower::expectPosition(uint64_t positionMs, milliseconds now) {
    position_ = positionMs;
    positionAt_ = now;
}

void PlayerFollower::followTrack(const std::string& track, std::vector<PlayerEvent>& events) {
    if (expected_.current.empty() || track == expected_.current || track == left_) return;
    if (track.empty()) return; // a stopped player has none, which its state reports
    if (track == expected_.next) {
        expected_ = {expected_.next, {}, expected_.current};
        events.push_back({PlayerEventKind::NextTrack});
    } else if (track == expected_.previous) {
        expected_ = {expected_.previous, expected_.current, {}};
        events.push_back({PlayerEventKind::PreviousTrack});
    } else {
        ended_ = true;
        events.push_back({PlayerEventKind::TakenOver});
        return;
    }
    position_.reset();
}

std::vector<PlayerEvent> PlayerFollower::observe(const PlayerObservation& observation, milliseconds now) {
    std::vector<PlayerEvent> events;
    if (ended_) return events;
    if (observation.otherSource) {
        ended_ = true;
        events.push_back({PlayerEventKind::TakenOver});
        return events;
    }
    if (observation.track && now >= settledAt_) {
        followTrack(*observation.track, events);
        if (ended_) return events;
    }
    if (observation.state && observation.state != state_) {
        const bool first = !state_;
        state_ = observation.state;
        if (!first || *state_ != TransportState::Stopped)
            events.push_back({*state_ == TransportState::Playing  ? PlayerEventKind::Playing
                              : *state_ == TransportState::Paused ? PlayerEventKind::Paused
                                                                  : PlayerEventKind::Stopped});
    }
    if (observation.positionMs) {
        const bool stopped = state_ == TransportState::Stopped;
        if (stopped) position_.reset(); // a stopped Linkplay reports the track's length as its position
        if (position_ && now >= settledAt_) {
            const auto elapsed = state_ == TransportState::Playing ? uint64_t((now - positionAt_).count()) : uint64_t{0};
            const uint64_t predicted = *position_ + elapsed;
            const uint64_t reported = *observation.positionMs;
            if ((reported > predicted ? reported - predicted : predicted - reported) > kSeekMarginMs)
                events.push_back({PlayerEventKind::Seeked, reported});
        }
        if (!stopped) {
            position_ = observation.positionMs;
            positionAt_ = now;
        }
    }
    return events;
}

std::vector<PlayerEvent> PlayerFollower::lost() {
    if (ended_) return {};
    ended_ = true;
    return {{PlayerEventKind::Lost}};
}

struct PlayerLink::Impl {
    struct Subscription {
        std::string kind, eventUrl, sid;
        Clock::time_point renewAt;
    };
    struct Notification {
        std::string sid, body;
    };

    upnp::PlayerDescription player;
    std::function<void(const PlayerEvent&)> onEvent;
    LinkOptions options;
    SOCKET listener = INVALID_SOCKET;
    std::string callback;
    std::vector<Subscription> subscriptions;
    std::atomic<bool> stopping = false;
    std::mutex mutex; // the follower, the queue and the source
    PlayerFollower follower;
    std::deque<Notification> notifications;
    std::optional<std::string> ownSource, seenSource;
    std::string currentId; // OpenHome: the playlist entry playing
    std::atomic<uint64_t> lastPositionMs = 0;
    std::atomic<bool> lastStopped = false, lastPlaying = false, sawPlaying = false;
    Clock::time_point started = Clock::now();
    std::thread listening, working;

    milliseconds now() const { return std::chrono::duration_cast<milliseconds>(Clock::now() - started); }

    Impl(upnp::PlayerDescription description, std::function<void(const PlayerEvent&)> handler, LinkOptions linkOptions)
        : player(std::move(description)), onEvent(std::move(handler)), options(linkOptions) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        openListener();
        listening = std::thread([this] { listen(); });
        try {
            for (const char* kind : subscribedKinds()) {
                const auto* service = player.service(kind);
                if (!service || service->eventUrl.empty()) continue;
                subscribe(*service, kind);
            }
            if (subscriptions.empty()) throw std::runtime_error("the player offers no events");
        } catch (...) {
            stopListening();
            throw;
        }
        working = std::thread([this] { work(); });
    }

    ~Impl() {
        stopping = true;
        working.join();
        for (const auto& subscription : subscriptions) {
            try {
                http::send("UNSUBSCRIBE", subscription.eventUrl, "SID: " + subscription.sid, "", options.requestTimeoutMs, 4096);
            } catch (const std::exception&) {
                log::info("network player: could not end a subscription");
            }
        }
        stopListening();
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    std::vector<const char*> subscribedKinds() const {
        if (player.protocol == upnp::PlayerProtocol::OpenHome) return {"Product", "Playlist", "Time"};
        return {"AVTransport"};
    }

    void stopListening() {
        stopping = true;
        closesocket(listener);
        listening.join();
        WSACleanup();
    }

    void openListener() {
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

    void subscribe(const upnp::PlayerService& service, const char* kind) {
        const auto reply = http::send("SUBSCRIBE", service.eventUrl,
                                      "CALLBACK: <" + callback + ">\r\nNT: upnp:event\r\nTIMEOUT: Second-" +
                                          std::to_string(options.subscribeSeconds),
                                      "", options.requestTimeoutMs, upnp::kMaxMessageBytes);
        const auto sid = headerValue(reply.headers, "SID");
        if (reply.status != 200 || sid.empty()) throw std::runtime_error("the player refused a subscription");
        subscriptions.push_back({kind, service.eventUrl, sid, renewalTime(reply.headers)});
    }

    // Halfway through what the player granted, which may be less than what was asked.
    Clock::time_point renewalTime(const std::string& head) const {
        const auto timeout = headerValue(head, "TIMEOUT");
        int seconds = options.subscribeSeconds;
        if (timeout.starts_with("Second-"))
            seconds = std::clamp(int(number(std::string_view(timeout).substr(7))), 2, options.subscribeSeconds);
        return Clock::now() + std::chrono::seconds(seconds) / 2;
    }

    void renew(Subscription& subscription) {
        const auto reply =
            http::send("SUBSCRIBE", subscription.eventUrl,
                       "SID: " + subscription.sid + "\r\nTIMEOUT: Second-" + std::to_string(options.subscribeSeconds), "",
                       options.requestTimeoutMs, upnp::kMaxMessageBytes);
        if (reply.status == 200) {
            subscription.renewAt = renewalTime(reply.headers);
            return;
        }
        // The player forgot the subscription, as after a restart.
        const auto* service = player.service(subscription.kind);
        const auto fresh = service ? http::send("SUBSCRIBE", service->eventUrl,
                                                "CALLBACK: <" + callback + ">\r\nNT: upnp:event\r\nTIMEOUT: Second-" +
                                                    std::to_string(options.subscribeSeconds),
                                                "", options.requestTimeoutMs, upnp::kMaxMessageBytes)
                                   : http::Reply{};
        const auto sid = headerValue(fresh.headers, "SID");
        if (fresh.status != 200 || sid.empty()) throw std::runtime_error("the player dropped a subscription");
        subscription.sid = sid;
        subscription.renewAt = renewalTime(fresh.headers);
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
                notifications.push_back({headerValue(head, "SID"), request.substr(headEnd + 4)});
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

    void report(const std::vector<PlayerEvent>& events) {
        for (const auto& event : events) onEvent(event);
    }

    void observe(const PlayerObservation& observation) {
        if (observation.state) {
            lastPlaying = *observation.state == TransportState::Playing;
            if (lastPlaying) sawPlaying = true;
            lastStopped = sawPlaying && *observation.state == TransportState::Stopped; // not the moment before it starts
        }
        // Until it plays, a player reports the position of what it played before.
        if (observation.positionMs && sawPlaying && !lastStopped) lastPositionMs = *observation.positionMs;
        std::vector<PlayerEvent> events;
        {
            std::scoped_lock lock(mutex);
            events = follower.observe(observation, now());
        }
        // An event that names the next track says nothing of the position, which still is the last track's.
        if (!observation.positionMs &&
            std::ranges::any_of(events, [](const PlayerEvent& event) { return event.kind == PlayerEventKind::NextTrack; }))
            lastPositionMs = 0;
        report(events);
    }

    PlayerObservation fromAvTransport(const Values& values) {
        PlayerObservation seen;
        if (const auto state = find(values, "TransportState"); !state.empty()) seen.state = stateFrom(state);
        for (const char* name : {"CurrentTrackURI", "AVTransportURI"})
            if (values.contains(name)) {
                seen.track = find(values, name);
                break;
            }
        if (const auto time = find(values, "RelativeTimePosition"); !time.empty()) seen.positionMs = upnp::parseDuration(time);
        return seen;
    }

    // OpenHome tells a takeover by the player's source changing away from the one the playlist was on.
    PlayerObservation fromOpenHome(const Values& values) {
        PlayerObservation seen;
        if (const auto state = find(values, "TransportState"); !state.empty()) seen.state = stateFrom(state);
        if (values.contains("Id")) seen.track = find(values, "Id");
        if (const auto seconds = find(values, "Seconds"); !seconds.empty()) seen.positionMs = number(seconds) * 1000;
        if (const auto source = find(values, "SourceIndex"); !source.empty()) {
            std::scoped_lock lock(mutex);
            seenSource = source;
            if (!ownSource) ownSource = source;
            seen.otherSource = source != *ownSource;
        }
        return seen;
    }

    void handle(const Notification& notification) {
        const auto subscribed = std::ranges::find(subscriptions, notification.sid, &Subscription::sid);
        if (subscribed == subscriptions.end()) return;
        const auto values = upnp::parsePropertySet(notification.body);
        if (player.protocol == upnp::PlayerProtocol::OpenHome) {
            observe(fromOpenHome(values));
            return;
        }
        if (const auto change = find(values, "LastChange"); !change.empty())
            observe(fromAvTransport(upnp::parseLastChange(change)));
    }

    upnp::SoapReply call(const char* kind, const char* action,
                         const std::vector<std::pair<std::string, std::string>>& arguments) {
        const auto* service = player.service(kind);
        if (!service) return {};
        const auto body = upnp::soapRequest(service->type, action, arguments);
        const auto reply =
            http::send("POST", service->controlUrl,
                       "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPACTION: \"" + service->type + "#" + action + "\"", body,
                       options.requestTimeoutMs, upnp::kMaxMessageBytes);
        return upnp::parseSoapReply(reply.status, reply.body);
    }

    bool attempt(const char* kind, const char* action, const std::vector<std::pair<std::string, std::string>>& arguments) {
        try {
            return call(kind, action, arguments).ok;
        } catch (const std::exception& e) {
            log::warn("network player %s: %s", action, e.what());
            return false;
        }
    }

    // Throws when the player does not answer.
    void poll() {
        PlayerObservation seen;
        if (player.protocol == upnp::PlayerProtocol::OpenHome) {
            const auto state = call("Playlist", "TransportState", {});
            if (!state.ok) throw std::runtime_error("the player refused a state request");
            seen.state = stateFrom(find(state.values, "Value"));
            const auto id = call("Playlist", "Id", {});
            if (id.ok) seen.track = find(id.values, "Value");
            const auto time = call("Time", "Time", {});
            if (time.ok) seen.positionMs = number(find(time.values, "Seconds")) * 1000;
            const auto source = call("Product", "SourceIndex", {});
            if (source.ok) {
                const auto index = find(source.values, "Value");
                std::scoped_lock lock(mutex);
                if (!ownSource) ownSource = index;
                seen.otherSource = index != *ownSource;
            }
        } else {
            const std::vector<std::pair<std::string, std::string>> instance{{"InstanceID", "0"}};
            const auto state = call("AVTransport", "GetTransportInfo", instance);
            if (!state.ok) throw std::runtime_error("the player refused a state request");
            seen.state = stateFrom(find(state.values, "CurrentTransportState"));
            const auto position = call("AVTransport", "GetPositionInfo", instance);
            if (position.ok) {
                seen.track = find(position.values, "TrackURI");
                seen.positionMs = upnp::parseDuration(find(position.values, "RelTime"));
            }
        }
        observe(seen);
    }

    void work() {
        int failures = 0;
        auto nextPoll = Clock::now();
        while (!stopping) {
            std::deque<Notification> pending;
            {
                std::scoped_lock lock(mutex);
                pending.swap(notifications);
            }
            for (const auto& notification : pending) handle(notification);
            try {
                for (auto& subscription : subscriptions)
                    if (Clock::now() >= subscription.renewAt) renew(subscription);
                if (Clock::now() >= nextPoll) {
                    nextPoll = Clock::now() + options.pollInterval;
                    poll();
                    failures = 0;
                }
            } catch (const std::exception& e) {
                log::info("network player %s: %s", player.name.c_str(), e.what());
                if (++failures >= options.failuresUntilLost) {
                    std::vector<PlayerEvent> events;
                    {
                        std::scoped_lock lock(mutex);
                        events = follower.lost();
                    }
                    report(events);
                    return;
                }
            }
            std::this_thread::sleep_for(kTick);
        }
    }
};

PlayerLink::PlayerLink(upnp::PlayerDescription player, std::function<void(const PlayerEvent&)> onEvent, LinkOptions options)
    : impl_(std::make_unique<Impl>(std::move(player), std::move(onEvent), options)) {}

PlayerLink::~PlayerLink() = default;

void PlayerLink::expect(ExpectedTracks tracks) {
    std::scoped_lock lock(impl_->mutex);
    impl_->follower.expect(std::move(tracks));
    impl_->ownSource = impl_->seenSource;
}

bool PlayerLink::load(const std::string& uri, const std::string& metadata) {
    auto& link = *impl_;
    link.lastPositionMs = 0;
    link.lastStopped = false; // what the player reports next is about the new track
    link.lastPlaying = false;
    link.sawPlaying = false;
    if (link.player.protocol == upnp::PlayerProtocol::OpenHome) {
        try {
            if (!link.call("Playlist", "DeleteAll", {}).ok) return false;
            const auto inserted = link.call("Playlist", "Insert", {{"AfterId", "0"}, {"Uri", uri}, {"Metadata", metadata}});
            const auto id = inserted.values.find("NewId");
            if (!inserted.ok || id == inserted.values.end() || !link.call("Playlist", "SeekId", {{"Value", id->second}}).ok)
                return false;
            link.currentId = id->second;
            std::scoped_lock lock(link.mutex);
            link.follower.expectLoaded({id->second, {}, {}}, link.now());
            link.ownSource = link.seenSource;
            return true;
        } catch (const std::exception& e) {
            log::warn("network player Insert: %s", e.what());
            return false;
        }
    }
    if (!link.attempt("AVTransport", "SetAVTransportURI",
                      {{"InstanceID", "0"}, {"CurrentURI", uri}, {"CurrentURIMetaData", metadata}}))
        return false;
    std::scoped_lock lock(link.mutex);
    link.follower.expectLoaded({uri, {}, {}}, link.now());
    return true;
}

bool PlayerLink::loadNext(const std::string& uri, const std::string& metadata) {
    auto& link = *impl_;
    std::string track = uri;
    if (link.player.protocol == upnp::PlayerProtocol::OpenHome) {
        try {
            const auto inserted =
                link.call("Playlist", "Insert", {{"AfterId", link.currentId}, {"Uri", uri}, {"Metadata", metadata}});
            const auto id = inserted.values.find("NewId");
            if (!inserted.ok || id == inserted.values.end()) return false;
            track = id->second;
            link.currentId = track;
        } catch (const std::exception& e) {
            log::warn("network player Insert: %s", e.what());
            return false;
        }
    } else if (!link.attempt("AVTransport", "SetNextAVTransportURI",
                             {{"InstanceID", "0"}, {"NextURI", uri}, {"NextURIMetaData", metadata}})) {
        return false;
    }
    std::scoped_lock lock(link.mutex);
    link.follower.expectNext(track);
    return true;
}

bool PlayerLink::play() {
    if (impl_->player.protocol == upnp::PlayerProtocol::OpenHome) return impl_->attempt("Playlist", "Play", {});
    return impl_->attempt("AVTransport", "Play", {{"InstanceID", "0"}, {"Speed", "1"}});
}

bool PlayerLink::pause() {
    if (impl_->player.protocol == upnp::PlayerProtocol::OpenHome) return impl_->attempt("Playlist", "Pause", {});
    return impl_->attempt("AVTransport", "Pause", {{"InstanceID", "0"}});
}

bool PlayerLink::stop() {
    if (impl_->player.protocol == upnp::PlayerProtocol::OpenHome) return impl_->attempt("Playlist", "Stop", {});
    return impl_->attempt("AVTransport", "Stop", {{"InstanceID", "0"}});
}

bool PlayerLink::seek(uint64_t positionMs) {
    {
        std::scoped_lock lock(impl_->mutex);
        impl_->follower.expectPosition(positionMs, impl_->now());
    }
    if (impl_->player.protocol == upnp::PlayerProtocol::OpenHome)
        return impl_->attempt("Playlist", "SeekSecondAbsolute", {{"Value", std::to_string(positionMs / 1000)}});
    return impl_->attempt("AVTransport", "Seek",
                          {{"InstanceID", "0"}, {"Unit", "REL_TIME"}, {"Target", upnp::formatDuration(positionMs)}});
}

void PlayerLink::advance() {
    std::scoped_lock lock(impl_->mutex);
    impl_->follower.advance();
}

uint64_t PlayerLink::positionMs() const {
    return impl_->lastPositionMs;
}

bool PlayerLink::stopped() const {
    return impl_->lastStopped;
}

bool PlayerLink::playing() const {
    return impl_->lastPlaying;
}

} // namespace camel::server
