// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Network/HQPlayerOutput.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <format>
#include <mutex>
#include <thread>

#include "Common/AudioTypes.h"
#include "Common/util/Log.h"
#include "Server/Network/PlayerControl.h"
#include "Server/Network/PlayerLink.h"

namespace camel::server {

namespace {

using audio::Error;
using audio::ErrorCode;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

constexpr uint64_t kNearEndMs = 3000;
// Long enough for a start of DSD, which HQPlayer spends about 4 s without a Status (2026-11-16).
constexpr auto kOwnShowsWithin = std::chrono::seconds(8);

// The one connection to HQPlayer while it is the output, which each stream's control borrows: a new connection's first
// reply takes seconds, and every stream (a seek, a track chosen) is a new control.
struct Connection {
    Connection(const std::string& address, const hqplayer::ClientOptions& options)
        : client(
              address,
              [this](const hqplayer::Status& s) {
                  std::scoped_lock lock(mutex);
                  if (status) status(s);
              },
              [this] {
                  std::scoped_lock lock(mutex);
                  if (lost) lost();
              },
              options) {}

    std::mutex mutex; // held while a callback runs, so that a control leaving waits for it
    std::function<void(const hqplayer::Status&)> status;
    std::function<void()> lost;
    hqplayer::Client client;
};

// HQPlayer's control, following its Status pushes. Commands go out on the caller's thread or, for those Status calls for,
// on the link's own worker: the client's reader thread, which runs onStatus, cannot wait for a reply.
class HQPlayerLink final : public PlayerControl {
public:
    HQPlayerLink(std::shared_ptr<Connection> connection, std::function<void(const PlayerEvent&)> onEvent,
                 std::function<void(const HQPlayerProcessing&)> processing, const HQPlayerOutputOptions& options)
        : onEvent_(std::move(onEvent)), processing_(std::move(processing)), options_(options),
          connection_(std::move(connection)) {
        connection_->client.connect();
        connections_ = connection_->client.connections();
        {
            std::scoped_lock lock(connection_->mutex);
            connection_->status = [this](const hqplayer::Status& s) { onStatus(s); };
            connection_->lost = [this] { onLost(); };
        }
        if (const auto last = connection_->client.status()) onStatus(*last);
        worker_ = std::thread([this] { work(); });
    }

    ~HQPlayerLink() override {
        {
            std::scoped_lock lock(connection_->mutex);
            connection_->status = nullptr;
            connection_->lost = nullptr;
        }
        {
            std::scoped_lock lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        worker_.join();
    }
    HQPlayerLink(const HQPlayerLink&) = delete;
    HQPlayerLink& operator=(const HQPlayerLink&) = delete;

    LoadOutcome load(const std::string& uri, const std::string& metadata) override {
        own(0);
        {
            // HQPlayer may say nothing for seconds, as while it starts DSD: what CamelPlayer changes is expected until
            // a Status shows it, however late.
            std::scoped_lock lock(mutex_);
            ownPlaylist_ = ownTrack_ = true;
        }
        // A clear while paused keeps the paused track (docs/Plan.md §5.11), so the playlist is stopped first. A new
        // connection meanwhile is no loss: it starts with the same Stop and PlaylistClear.
        // A command that went unanswered has reported the connection lost.
        const auto failed = [&] { return lost_ ? LoadOutcome::NotAnswered : LoadOutcome::Refused; };
        if (!command("<Stop/>", true) || !command("<PlaylistClear/>", true)) return failed();
        own(std::nullopt); // the add follows a wait for those replies, which may have been long
        if (!command(add(uri, metadata))) return failed();
        std::scoped_lock lock(mutex_);
        nextQueued_ = false;
        playedSinceLoad_ = false;
        playAgainSent_ = false;
        loadedSincePlay_ = true;
        return LoadOutcome::Loaded;
    }

    bool loadNext(const std::string& uri, const std::string& metadata) override {
        own(std::nullopt);
        {
            std::scoped_lock lock(mutex_);
            ownPlaylist_ = true;
        }
        if (!command(add(uri, metadata))) return false;
        std::scoped_lock lock(mutex_);
        nextQueued_ = true;
        return true;
    }

    void advance() override {
        std::scoped_lock lock(mutex_);
        nextQueued_ = false;
    }

    bool play() override {
        own(2);
        {
            std::scoped_lock lock(mutex_);
            askedToPlay_ = Clock::now();
            // A track just loaded starts at 0; the last Status may still show where the one before it was.
            positionAtPlay_ = status_ && !std::exchange(loadedSincePlay_, false) ? status_->position : 0.0;
        }
        return command(R"(<Play last="0"/>)");
    }

    bool pause() override {
        own(1);
        return command("<Pause/>");
    }

    bool stop() override {
        own(0);
        return command("<Stop/>");
    }

    uint64_t positionMs() const override {
        std::scoped_lock lock(mutex_);
        return status_ ? uint64_t(std::max(0.0, status_->position) * 1000.0) : 0;
    }
    bool stopped() const override {
        std::scoped_lock lock(mutex_);
        return status_ && status_->state == 0;
    }
    bool playing() const override {
        std::scoped_lock lock(mutex_);
        return status_ && status_->state == 2;
    }
    bool idle() const override {
        std::scoped_lock lock(mutex_);
        return !status_ || status_->state == 0;
    }
    bool played() const override {
        std::scoped_lock lock(mutex_);
        return playedSinceLoad_;
    }
    bool lost() const override { return lost_; }

private:
    static std::string add(const std::string& uri, const std::string& metadata) {
        return std::format(R"(<PlaylistAdd uri="{}" queued="0" clear="0"><metadata {}/></PlaylistAdd>)", hqplayer::escape(uri),
                           metadata);
    }

    // A command whose reply is not OK, or none, fails; a lost connection is reported once.
    bool command(const std::string& xml, bool clearing = false) {
        try {
            const bool ok = connection_->client.ok(xml);
            // A new connection cleared HQPlayer's playlist: what played is gone, unless it was being cleared anyway.
            if (connection_->client.connections() != connections_) {
                if (!clearing) reportLost();
                // What the new connection's Stop and PlaylistClear did is CamelPlayer's; its Status starts anew.
                std::scoped_lock lock(mutex_);
                status_.reset();
                connections_ = connection_->client.connections();
            }
            return ok;
        } catch (const std::exception& e) {
            log::warn("HQPlayer: %s", e.what());
            reportLost();
            return false;
        }
    }

    // What CamelPlayer is about to change, so that the Status showing it is not taken for another source's change.
    void own(std::optional<int> state) {
        std::scoped_lock lock(mutex_);
        if (state) {
            if (*state != wantedState_) earlierWanted_ = wantedState_;
            wantedState_ = *state;
            stopReported_ = false;
        }
        ownedAt_ = Clock::now();
        ownUntil_ = ownedAt_ + options_.ownChange;
    }

    void onStatus(const hqplayer::Status& s) {
        std::vector<PlayerEvent> events;
        bool playAgain = false;
        {
            std::scoped_lock lock(mutex_);
            // A Status from a connection this control has not taken up yet shows only what that connection did.
            const std::optional<hqplayer::Status> before =
                connection_->client.connections() == connections_ ? status_ : std::nullopt;
            status_ = s;
            const auto now = Clock::now();
            const bool ours = now < ownUntil_;
            if (s.state == 2 && s.position > positionAtPlay_) playedSinceLoad_ = true;
            if (!before || before->state != s.state || before->trackSerial != s.trackSerial ||
                before->transportSerial != s.transportSerial)
                log::info("HQPlayer: state %d, track %u, playlist %u, position %.1f of %.1f", s.state, s.trackSerial,
                          s.transportSerial, s.position, s.length);
            const char* why = "";
            if (before && !taken_) {
                const bool playlistChanged = s.transportSerial != before->transportSerial;
                const bool trackChanged = s.trackSerial != before->trackSerial;
                const bool ownPlaylist = playlistChanged && std::exchange(ownPlaylist_, false);
                const bool ownTrack = trackChanged && std::exchange(ownTrack_, false);
                if (playlistChanged && !ownPlaylist && !ours) {
                    taken_ = true; // another client edited the playlist
                    why = "the playlist changed";
                } else if (trackChanged && !ownTrack) {
                    const double lengthMs = before->length * 1000.0;
                    const bool atEnd = lengthMs <= 0.0 || before->position * 1000.0 + double(kNearEndMs) >= lengthMs;
                    if (atEnd && nextQueued_) {
                        nextQueued_ = false;
                        events.push_back({PlayerEventKind::NextTrack, 0});
                    } else if (!atEnd && !ours) {
                        taken_ = true; // next or previous pressed in HQPlayer's window
                        why = "the track changed";
                    }
                    // At its end with nothing after it, the playlist ran out: HQPlayer moves on and stops, which the
                    // state below reports.
                }
                if (s.state == wantedState_) earlierWanted_.reset();
                if (!taken_ && s.state != wantedState_ && !ours) {
                    // Compared with what CamelPlayer last asked for, not with the last Status, so that a change made
                    // just after one of its own commands is seen once that command's time is over. What it asked for
                    // before that is its own too until the last shows: HQPlayer starting DSD shows a Play seconds late,
                    // after a Pause sent meanwhile.
                    const bool earlier = earlierWanted_ == s.state && now - ownedAt_ < kOwnShowsWithin;
                    if (s.state != 0 && !earlier) {
                        taken_ = true;
                        why = "the state is not the one asked for";
                    } else if (s.state == 0 && !stopReported_) {
                        stopReported_ = true;
                        events.push_back({PlayerEventKind::Stopped, 0}); // its end, or a stop: the stream tells which
                    }
                }
                if (taken_) {
                    events.push_back({PlayerEventKind::TakenOver, 0});
                    log::info("HQPlayer: taken as another source's, %s (state %d to %d, wanted %d; track %u to %u; playlist %u "
                              "to %u; position %.1f of %.1f)",
                              why, before->state, s.state, wantedState_, before->trackSerial, s.trackSerial,
                              before->transportSerial, s.transportSerial, before->position, before->length);
                }
            }
            // Playing well after CamelPlayer's last change, HQPlayer has shown all of it: what is still expected never came.
            if (s.state == 2 && s.position > 0.0 && now - ownedAt_ > kOwnShowsWithin) ownPlaylist_ = ownTrack_ = false;
            // Play answered OK yet nothing moves: sent once more, as HQPlayer did not always take the first.
            if (wantedState_ == 2 && !playAgainSent_ && !playedSinceLoad_ && askedToPlay_ != Clock::time_point{} &&
                now - askedToPlay_ >= options_.playAgainAfter) {
                playAgainSent_ = true;
                playAgain = true;
                ownUntil_ = now + options_.ownChange;
            }
        }
        if (processing_) processing_({s.activeRate, s.activeBits, s.activeMode, s.activeFilter, s.activeShaper});
        if (playAgain) {
            std::scoped_lock lock(mutex_);
            queued_.emplace_back(R"(<Play last="0"/>)");
            wake_.notify_all();
        }
        for (const auto& event : events)
            if (onEvent_) onEvent_(event);
    }

    void onLost() { reportLost(); }

    void reportLost() {
        if (lost_.exchange(true)) return;
        if (onEvent_) onEvent_({PlayerEventKind::Lost, 0});
    }

    void work() {
        std::unique_lock lock(mutex_);
        while (true) {
            wake_.wait(lock, [&] { return stopping_ || !queued_.empty(); });
            if (stopping_) return;
            const std::string next = std::move(queued_.front());
            queued_.pop_front();
            lock.unlock();
            log::info("HQPlayer: the position stood still after Play; sending it once more");
            command(next);
            lock.lock();
        }
    }

    std::function<void(const PlayerEvent&)> onEvent_;
    std::function<void(const HQPlayerProcessing&)> processing_;
    HQPlayerOutputOptions options_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<hqplayer::Status> status_;
    Clock::time_point ownUntil_, ownedAt_, askedToPlay_;
    double positionAtPlay_ = 0.0;
    int wantedState_ = 0;
    std::optional<int> earlierWanted_; // asked for before wantedState_, until a Status shows that
    bool nextQueued_ = false, playedSinceLoad_ = false, playAgainSent_ = false, taken_ = false, stopping_ = false;
    bool stopReported_ = false;
    bool loadedSincePlay_ = false;
    bool ownPlaylist_ = false, ownTrack_ = false; // changed by CamelPlayer, not shown by a Status yet
    std::deque<std::string> queued_;
    std::atomic<bool> lost_ = false;
    std::shared_ptr<Connection> connection_;
    std::atomic<uint64_t> connections_ = 0; // the client's, when this control began or last connected again
    std::thread worker_;
};

class HQPlayerStream final : public StreamPlayer {
public:
    HQPlayerStream(std::string address, HQPlayerOutputOptions options) : address_(std::move(address)), options_(options) {
        if (!hqplayer::splitAddress(address_, host_, port_)) host_ = address_;
    }

    std::string detail() const override {
        std::scoped_lock lock(*processingMutex_);
        return describeProcessing(*processing_);
    }

    std::string name() const override { return address_; }
    std::string mode() const override { return "HQPlayer · " + address_; }
    bool reportsPosition() const override { return true; }
    upnp::PlayerFormats formats(bool& known) override {
        known = true;
        return {.wav = true, .dsf = true};
    }
    bool gapless(bool& known) override {
        known = true;
        return true;
    }

    upnp::Listener listen() override {
        const std::string local =
            hqplayer::isLocalHost(host_) ? std::string("127.0.0.1") : upnp::localAddressFor(hqplayer::ipv4Of(host_));
        if (local.empty()) throw Error(ErrorCode::DeviceNotFound, notConnected() + ": no route to it");
        try {
            return upnp::listenOn(local);
        } catch (const std::runtime_error&) {
            throw Error(ErrorCode::DeviceNotFound, "cannot serve the stream");
        }
    }

    std::vector<std::string> hosts() const override {
        if (hqplayer::isLocalHost(host_)) return {"127.0.0.1"};
        auto address = hqplayer::ipv4Of(host_);
        return address.empty() ? std::vector<std::string>{} : std::vector<std::string>{std::move(address)};
    }

    std::string metadata(const upnp::DidlTrack& track) const override {
        std::string attributes = std::format(R"(song="{}" artist="{}" album="{}")", hqplayer::escape(track.title),
                                             hqplayer::escape(track.artist), hqplayer::escape(track.album));
        if (!track.coverUrl.empty()) attributes += std::format(R"( cover="{}")", hqplayer::escape(track.coverUrl));
        return attributes;
    }

    std::unique_ptr<PlayerControl> connect(std::function<void(const PlayerEvent&)> onEvent) override {
        if (!connection_) connection_ = std::make_shared<Connection>(address_, options_.client);
        return std::make_unique<HQPlayerLink>(
            connection_, std::move(onEvent),
            [mutex = processingMutex_, kept = processing_](const HQPlayerProcessing& p) {
                std::scoped_lock lock(*mutex);
                *kept = p;
            },
            options_);
    }

    std::string inUseByAnother() const override { return "HQPlayer is in use by another source"; }
    std::string didNotStart() const override {
        return "HQPlayer could not open its output: turning the DAC on may not be enough, HQPlayer may need restarting";
    }
    std::string notConnected() const override {
        return "HQPlayer is not connected (an unlicensed HQPlayer quits by itself when its 30-minute trial ends)";
    }

private:
    std::string address_, host_;
    uint16_t port_ = hqplayer::kDefaultPort;
    HQPlayerOutputOptions options_;
    std::shared_ptr<Connection> connection_; // from the first stream, for as long as HQPlayer is the output
    // Shared with the link, which may outlive a call of `detail` on its own thread.
    std::shared_ptr<std::mutex> processingMutex_ = std::make_shared<std::mutex>();
    std::shared_ptr<HQPlayerProcessing> processing_ = std::make_shared<HQPlayerProcessing>();
};

} // namespace

std::string describeProcessing(const HQPlayerProcessing& p) {
    if (!p.rate) return {};
    // One bit is DSD, which HQPlayer reports by its bit rate in Hz.
    if (p.bits == 1) {
        std::string text =
            p.rate % 44100 == 0 ? std::format("DSD{}", p.rate / 44100) : std::format("DSD {:.4g} MHz", p.rate / 1e6);
        for (const auto* part : {&p.filter, &p.shaper})
            if (!part->empty()) text += " · " + *part;
        return text;
    }
    std::string text = p.rate % 1000 == 0 ? std::format("{} kHz", p.rate / 1000) : std::format("{:.1f} kHz", p.rate / 1000.0);
    if (p.bits) text += std::format("/{}-bit", p.bits);
    for (const auto* part : {&p.filter, &p.shaper})
        if (!part->empty()) text += " · " + *part;
    return text;
}

std::unique_ptr<audio::IOutput> createHQPlayerOutput(std::string address,
                                                     std::function<TrackDetails(const std::wstring&)> details,
                                                     std::function<void(const PlayerEvent&)> follow,
                                                     HQPlayerOutputOptions options) {
    auto stream = options.stream;
    stream.startLimit = options.playAgainAfter * 2 + milliseconds(2000);
    return createStreamOutput(std::make_unique<HQPlayerStream>(std::move(address), options), std::move(details),
                              std::move(follow), stream);
}

} // namespace camel::server
