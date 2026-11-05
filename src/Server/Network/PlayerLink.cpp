// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Network/PlayerLink.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "Common/util/Log.h"
#include "Server/Support/Http.h"

namespace camel::server {

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;
using Values = std::map<std::string, std::string>;

constexpr uint64_t kSeekMarginMs = 2000; // polls and events lag, so a smaller jump is not a seek
constexpr auto kTick = milliseconds(25);
constexpr auto kSettling = milliseconds(3000); // a player can still report the old track this long after a new one

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
    upnp::PlayerDescription player;
    std::function<void(const PlayerEvent&)> onEvent;
    LinkOptions options;
    PlayerEvents events;
    std::atomic<bool> stopping = false;
    std::mutex mutex; // the follower and the source
    PlayerFollower follower;
    std::optional<std::string> ownSource, seenSource;
    std::string currentId; // OpenHome: the playlist entry playing
    std::atomic<uint64_t> lastPositionMs = 0;
    std::atomic<bool> lastStopped = false, lastPlaying = false, sawPlaying = false;
    Clock::time_point started = Clock::now();
    std::thread working;

    milliseconds now() const { return std::chrono::duration_cast<milliseconds>(Clock::now() - started); }

    Impl(upnp::PlayerDescription description, std::function<void(const PlayerEvent&)> handler, LinkOptions linkOptions)
        : player(std::move(description)), onEvent(std::move(handler)), options(linkOptions),
          events(player, subscribedKinds(), options) {
        working = std::thread([this] { work(); });
    }

    ~Impl() {
        stopping = true;
        working.join();
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    std::vector<std::string> subscribedKinds() const {
        if (player.protocol == upnp::PlayerProtocol::OpenHome) return {"Product", "Playlist", "Time"};
        return {"AVTransport"};
    }

    void report(const std::vector<PlayerEvent>& happened) {
        for (const auto& event : happened) onEvent(event);
    }

    void observe(const PlayerObservation& observation) {
        if (observation.state) {
            lastPlaying = *observation.state == TransportState::Playing;
            if (lastPlaying) sawPlaying = true;
            lastStopped = sawPlaying && *observation.state == TransportState::Stopped; // not the moment before it starts
        }
        // Until it plays, a player reports the position of what it played before.
        if (observation.positionMs && sawPlaying && !lastStopped) lastPositionMs = *observation.positionMs;
        std::vector<PlayerEvent> happened;
        {
            std::scoped_lock lock(mutex);
            happened = follower.observe(observation, now());
        }
        // An event that names the next track says nothing of the position, which still is the last track's.
        if (!observation.positionMs &&
            std::ranges::any_of(happened, [](const PlayerEvent& event) { return event.kind == PlayerEventKind::NextTrack; }))
            lastPositionMs = 0;
        report(happened);
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

    void handle(const PlayerEvents::Notification& notification) {
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
            for (const auto& notification : events.take()) handle(notification);
            try {
                events.renewDue();
                if (Clock::now() >= nextPoll) {
                    nextPoll = Clock::now() + options.pollInterval;
                    poll();
                    failures = 0;
                }
            } catch (const std::exception& e) {
                log::info("network player %s: %s", player.name.c_str(), e.what());
                if (++failures >= options.failuresUntilLost) {
                    std::vector<PlayerEvent> happened;
                    {
                        std::scoped_lock lock(mutex);
                        happened = follower.lost();
                    }
                    report(happened);
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
