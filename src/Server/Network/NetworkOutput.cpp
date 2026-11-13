// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Network/NetworkOutput.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "Common/util/Log.h"
#include "Common/util/Text.h"
#include "Server/Support/Http.h"

#pragma comment(lib, "ws2_32.lib")

namespace camel::server {

namespace {

using audio::DeviceFormat;
using audio::Error;
using audio::ErrorCode;
using audio::IRenderSource;
using audio::OutputStatus;
using audio::SampleType;
using audio::StreamTrack;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

constexpr SOCKET kNoSocket = INVALID_SOCKET;
constexpr size_t kChunkFrames = 2048;
constexpr size_t kRetainBytes = size_t{16} << 20; // what a player that reconnects can ask for again
constexpr uint64_t kUnknownLength = uint64_t{1} << 31;
constexpr size_t kDsfBlock = 4096;
constexpr size_t kDsfHeader = 92;
constexpr uint8_t kDsfSilence = 0x96; // the DSD idle pattern 0x69, least significant bit first
constexpr size_t kWavHeader = 44;
constexpr auto kPatience = milliseconds(1500);
constexpr uint64_t kNearStartMs = 2000, kNearEndMs = 3000, kClockSlackMs = 2000;
constexpr int64_t kSlowStartMs = 3000;
constexpr auto kChangeSettles = milliseconds(3000);
constexpr auto kStartLimit = milliseconds(5000);

void putLe(std::string& out, uint64_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) out.push_back(char((value >> (8 * i)) & 0xFF));
}

// A stream past what the 32-bit sizes hold says 0xFFFFFFFF for both, which players take for a size unknown, rather than
// a size it does not have.
std::string wavHeader(const DeviceFormat& format, uint64_t dataBytes) {
    const bool fits = dataBytes <= 0xFFFFFFFFULL - 36;
    const uint64_t data = fits ? dataBytes : 0xFFFFFFFFULL;
    const uint32_t blockAlign = format.bytesPerFrame();
    std::string header = "RIFF";
    putLe(header, fits ? 36ULL + data : 0xFFFFFFFFULL, 4);
    header += "WAVEfmt ";
    putLe(header, 16, 4);
    putLe(header, 1, 2); // PCM
    putLe(header, format.channels, 2);
    putLe(header, format.sampleRate, 4);
    putLe(header, uint64_t{format.sampleRate} * blockAlign, 4);
    putLe(header, blockAlign, 2);
    putLe(header, uint64_t{audio::bytesPerSample(format.type)} * 8, 2);
    header += "data";
    putLe(header, data, 4);
    return header;
}

// DSD interleaved a byte per channel, the oldest sample in each byte's top bit, becomes blocks of one channel with the
// oldest sample in the bottom bit.
std::string dsfHeader(const DeviceFormat& format, uint64_t frames) {
    const uint64_t blocks = (frames + kDsfBlock - 1) / kDsfBlock;
    const uint64_t dataBytes = blocks * kDsfBlock * format.channels;
    std::string header = "DSD ";
    putLe(header, 28, 8);
    putLe(header, kDsfHeader + dataBytes, 8);
    putLe(header, 0, 8);
    header += "fmt ";
    putLe(header, 52, 8);
    putLe(header, 1, 4); // format version
    putLe(header, 0, 4); // DSD raw
    putLe(header, 2, 4); // stereo
    putLe(header, format.channels, 4);
    putLe(header, uint64_t{format.sampleRate} * 8, 4);
    putLe(header, 1, 4); // one bit per sample
    putLe(header, frames * 8, 8);
    putLe(header, kDsfBlock, 4);
    putLe(header, 0, 4);
    header += "data";
    putLe(header, 12 + dataBytes, 8);
    return header;
}

uint8_t reverseBits(uint8_t value) {
    value = uint8_t((value & 0xF0) >> 4 | (value & 0x0F) << 4);
    value = uint8_t((value & 0xCC) >> 2 | (value & 0x33) << 2);
    return uint8_t((value & 0xAA) >> 1 | (value & 0x55) << 1);
}

std::string xmlText(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c == '&')
            out += "&amp;";
        else if (c == '<')
            out += "&lt;";
        else if (c == '>')
            out += "&gt;";
        else if (c == '"')
            out += "&quot;";
        else
            out += c;
    }
    return out;
}

// One track of the stream: bytes the player has asked for or may ask for again.
struct Body {
    uint32_t id = 0;
    StreamTrack track;
    std::string contentType, protocolInfo, extension, uri, didl, coverJpeg;
    uint64_t totalBytes = 0, totalFrames = 0;
    bool lengthKnown = false;
    std::vector<uint8_t> retained;
    uint64_t retainedFrom = 0; // offset of retained[0]
    uint64_t framesWritten = 0;
    bool complete = false;
    bool rendered = false; // the source has rendered all of its track, which a told length can end before
    std::vector<uint8_t> dsfLeft, dsfRight;

    uint64_t produced() const { return retainedFrom + retained.size(); }
};

class NetworkOutput final : public audio::IOutput {
public:
    NetworkOutput(upnp::PlayerDescription player, std::function<TrackDetails(const std::wstring&)> details,
                  std::function<void(const PlayerEvent&)> follow, NetworkOutputOptions options)
        : player_(std::move(player)), details_(std::move(details)), follow_(std::move(follow)), options_(options) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        readCapabilities();
    }

    _No_competing_thread_ ~NetworkOutput() override {
        close();
        link_.reset();
        WSACleanup();
    }
    NetworkOutput(const NetworkOutput&) = delete;
    NetworkOutput& operator=(const NetworkOutput&) = delete;

    bool supports(const DeviceFormat& f) override {
        if (f.channels != 2) return false;
        if (!formatsRead_ || !gaplessRead_) readCapabilities();
        if (f.type == SampleType::Dsd8) return formats_.dsf;
        if (f.type == SampleType::Int16) return formats_.wav || formats_.l16;
        if (f.type == SampleType::Int24) return formats_.wav || formats_.l24;
        return false;
    }

    bool gapless() const override { return gapless_ && options_.gapless; }

    void open(const DeviceFormat& f, IRenderSource* source) override {
        close();
        ensureLink();
        openListener();
        const StreamTrack first = source->streamTrack();
        const TrackDetails details = detailsOf(first.file);
        {
            std::scoped_lock lock(mutex_);
            format_ = f;
            source_ = source;
            ++generation_;
            bodies_.clear();
            carry_.clear();
            framesRendered_ = 0;
            framesFinished_ = 0;
            highestMs_ = 0;
            reportsPosition_ = false;
            started_ = false;
            playing_ = false;
            released_ = false;
            playingSince_ = {};
            pausedAt_ = {};
            pausedFor_ = {};
            askedToPlay_ = {};
            nextId_ = 0;
            queuedNext_ = 0;
            announcedBody_ = 0;
            closing_ = false;
            bodies_.push_back(makeBody(first, details));
        }
        acceptor_ = std::thread([this] { acceptLoop(); });
        announcer_ = std::thread([this] { announceLoop(); });
    }

    void start() override {
        std::string uri, didl;
        {
            std::scoped_lock lock(mutex_);
            if (started_) {
                uri.clear();
            } else {
                uri = bodies_.front().uri;
                didl = bodies_.front().didl;
            }
        }
        if (!uri.empty() && !link_->load(uri, didl)) throw Error(ErrorCode::FormatNotSupported, "the player refused the stream");
        {
            std::scoped_lock lock(mutex_);
            started_ = true;
            playing_ = true;
            askedToPlay_ = Clock::now();
        }
        announceNext();
        if (!link_->play()) throw Error(ErrorCode::DriverError, "the player did not start");
        std::scoped_lock lock(mutex_);
        if (playingSince_ == Clock::time_point{}) playingSince_ = Clock::now();
        if (pausedAt_ != Clock::time_point{}) {
            pausedFor_ += Clock::now() - pausedAt_;
            pausedAt_ = {};
        }
        moved_.notify_all();
    }

    void pause() override {
        bool released = false;
        {
            std::scoped_lock lock(mutex_);
            playing_ = false;
            released = released_;
            if (playingSince_ != Clock::time_point{} && pausedAt_ == Clock::time_point{}) pausedAt_ = Clock::now();
        }
        if (link_ && !released) link_->pause();
    }

    void close() override {
        {
            std::scoped_lock lock(mutex_);
            if (!acceptor_.joinable()) return;
            closing_ = true;
            for (const SOCKET client : clients_) closesocket(client);
            clients_.clear();
            moved_.notify_all();
        }
        closesocket(listener_);
        listener_ = kNoSocket;
        acceptor_.join();
        announcer_.join();
        for (auto& connection : connections_) connection.join();
        connections_.clear();
        std::unique_ptr<PlayerLink> ended;
        {
            std::scoped_lock lock(mutex_);
            source_ = nullptr;
            if (link_ && started_ && !released_) link_->stop();
            started_ = false;
            ended = std::move(link_);
        }
        // Its thread may be reporting to onPlayer, which takes the lock, until it ends.
        ended.reset();
    }

    OutputStatus status() const override {
        OutputStatus status;
        status.mode = "UPnP · " + player_.name;
        status.heldFrames = held();
        if (options_.gapless && !gapless_)
            status.adjustments.emplace_back("this player cannot follow one track with the next without a gap");
        return status;
    }

private:
    // What the player takes, read again later for what it could not tell yet, as when it was waking from standby.
    void readCapabilities() {
        if (!formatsRead_) {
            try {
                if (const auto* manager = player_.service("ConnectionManager")) {
                    const auto request = upnp::soapRequest(manager->type, "GetProtocolInfo", {});
                    const auto reply = http::send("POST", manager->controlUrl,
                                                  "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPACTION: \"" + manager->type +
                                                      "#GetProtocolInfo\"",
                                                  request, options_.link.requestTimeoutMs, upnp::kMaxMessageBytes);
                    const auto parsed = upnp::parseSoapReply(reply.status, reply.body);
                    if (const auto sink = parsed.values.find("Sink"); sink != parsed.values.end())
                        formats_ = upnp::parseProtocolInfo(sink->second);
                    formatsRead_ = parsed.ok;
                } else {
                    formatsRead_ = true;
                }
            } catch (const std::exception& e) {
                log::warn("network player %s: %s", player_.name.c_str(), e.what());
            }
            if (player_.protocol == upnp::PlayerProtocol::OpenHome && !formats_.wav && !formats_.l16)
                formats_.wav = true; // OpenHome players take WAV by URL
        }
        if (gaplessRead_) return;
        const auto* transport = player_.service("AVTransport");
        if (player_.protocol == upnp::PlayerProtocol::OpenHome || !transport || transport->scpdUrl.empty()) {
            gapless_ = player_.protocol == upnp::PlayerProtocol::OpenHome;
            gaplessRead_ = true;
            return;
        }
        try {
            const auto reply =
                http::send("GET", transport->scpdUrl, "", "", options_.link.requestTimeoutMs, upnp::kMaxMessageBytes);
            if (reply.status != 200) return;
            const auto actions = upnp::parseActions(reply.body);
            gapless_ = std::ranges::find(actions, "SetNextAVTransportURI") != actions.end();
            gaplessRead_ = true;
        } catch (const std::exception& e) {
            log::warn("network player %s: %s", player_.name.c_str(), e.what());
        }
    }

    void ensureLink() {
        if (link_ && !link_->lost()) return;
        link_.reset();
        try {
            link_ = std::make_unique<PlayerLink>(player_, [this](const PlayerEvent& event) { onPlayer(event); }, options_.link);
        } catch (const std::exception& e) {
            throw Error(ErrorCode::DeviceNotFound, "the network player is not connected: " + std::string(e.what()));
        }
    }

    // A player another source took, or one gone, is sent nothing more, not even a stop, until it is played to again.
    void release(const Error& error) {
        {
            std::scoped_lock lock(mutex_);
            if (released_) return;
            released_ = true;
        }
        raiseError(error);
    }

    void onPlayer(const PlayerEvent& event) {
        if (event.kind == PlayerEventKind::Lost) {
            release(Error(ErrorCode::DeviceLost, "the network player is not connected"));
            return;
        }
        if (event.kind == PlayerEventKind::TakenOver) {
            release(Error(ErrorCode::DeviceInUse, "the player is in use by another source"));
            return;
        }
        {
            // A player still starting reports PLAYING after a stop, which is not a request to play.
            std::scoped_lock lock(mutex_);
            if (!started_) return;
        }
        if (event.kind == PlayerEventKind::Stopped) {
            // Another source playing by other means than AVTransport, or the player's own remote, stopped it.
            if (stoppedByItself()) release(Error(ErrorCode::DeviceInUse, "the player is in use by another source"));
            return;
        }
        if (event.kind == PlayerEventKind::NextTrack) {
            std::scoped_lock lock(mutex_);
            if (bodies_.size() > 1 && bodies_.front().complete) {
                // The stream's own change of track: the player moved on to what it was given next.
                framesFinished_ += bodies_.front().framesWritten;
                bodies_.pop_front();
                highestMs_ = 0;
                changedAt_ = Clock::now();
                moved_.notify_all();
                return;
            }
        }
        if (event.kind == PlayerEventKind::Seeked) {
            std::scoped_lock lock(mutex_);
            if (event.positionMs < kNearStartMs && (bodies_.size() > 1 || Clock::now() - changedAt_ < kChangeSettles)) return;
            if (!bodies_.empty()) {
                if (follow_) follow_({PlayerEventKind::Seeked, positionInFile(bodies_.front(), event.positionMs)});
            }
            return;
        }
        if (follow_) follow_(event);
    }

    // Stopped after it played, while the track it plays is still being sent: neither its end nor a change of track, which
    // come once the track is sent whole.
    bool stoppedByItself() const {
        std::scoped_lock lock(mutex_);
        return playing_ && !closing_ && !released_ && link_->stopped() && !bodies_.empty() && !bodies_.front().complete &&
               Clock::now() - askedToPlay_ >= kChangeSettles;
    }

    // Some seconds after Play, a player stopped while its track is still being sent: one that never got going, as the
    // Essential Bravo in its standby, which reports PLAYING for a moment and then stays stopped, or one whose stop came
    // too soon after Play for `stoppedByItself` to trust it.
    void checkStarted() {
        bool started = false;
        {
            std::scoped_lock lock(mutex_);
            if (closing_ || released_ || !playing_ || askedToPlay_ == Clock::time_point{} ||
                Clock::now() - askedToPlay_ < kStartLimit || bodies_.empty() || bodies_.front().complete)
                return;
            if (!link_->stopped() && (link_->played() || !link_->idle())) return;
            started = link_->positionMs() != 0 || highestMs_ != 0;
        }
        if (started)
            release(Error(ErrorCode::DeviceInUse, "the player is in use by another source"));
        else
            release(Error(ErrorCode::DriverError, "the player did not start playing"));
    }

    _Requires_lock_held_(mutex_) uint64_t playedFrames() const {
        const uint64_t positionMs = link_ ? link_->positionMs() : 0;
        const auto now = Clock::now();
        if (positionMs != lastReported_) {
            lastReported_ = positionMs;
            lastAdvance_ = now;
        }
        // Players report whole seconds, late by up to a poll, so a clock runs that only a report far from it corrects.
        uint64_t smoothMs = positionMs;
        if (positionMs && link_ && link_->playing()) {
            const uint64_t flowing = clockMs_ + uint64_t(std::chrono::duration_cast<milliseconds>(now - clockAt_).count());
            if (flowing >= positionMs && flowing <= positionMs + kClockSlackMs) {
                smoothMs = flowing;
            } else {
                clockMs_ = positionMs;
                clockAt_ = now;
            }
        }
        uint64_t reported = smoothMs * format_.sampleRate / 1000;
        if (positionMs) reportsPosition_ = true;
        // A player that never reports a position is taken to play in real time from a start it may take seconds to reach (the
        // DMP-A6 does); 0 is also a track's start.
        if (!reportsPosition_ && playingSince_ != Clock::time_point{}) {
            // Time paused is not played.
            const auto until = pausedAt_ != Clock::time_point{} ? pausedAt_ : Clock::now();
            const auto elapsed = std::chrono::duration_cast<milliseconds>(until - playingSince_ - pausedFor_).count();
            if (elapsed > kSlowStartMs) reported = uint64_t(elapsed - kSlowStartMs) * format_.sampleRate / 1000;
        }
        return reported;
    }

    // Back at the start with the next track ready is the change of track: a player can report the old URL long after it
    // (the DMP-A6 does), so the position says so first.
    _Requires_lock_held_(mutex_) bool backAtStart() const {
        const uint64_t positionMs = link_ ? link_->positionMs() : 0;
        if (positionMs > highestMs_) highestMs_ = positionMs;
        if (bodies_.size() < 2 || !bodies_.front().complete || positionMs >= kNearStartMs || positionMs >= highestMs_)
            return false;
        const uint64_t durationMs = bodies_.front().framesWritten * 1000 / std::max(format_.sampleRate, 1U);
        return highestMs_ + kNearEndMs >= durationMs;
    }

    _Requires_lock_held_(mutex_) uint64_t allPlayed() const {
        return framesFinished_ + (backAtStart() ? bodies_.front().framesWritten : 0) + playedFrames();
    }

    uint64_t held() const {
        std::scoped_lock lock(mutex_);
        if (!source_ || !started_) return 0;
        if (link_ && link_->stopped() && bodies_.size() <= 1 && !bodies_.empty() && bodies_.front().complete) return 0;
        const uint64_t played = allPlayed();
        return framesRendered_ > played ? framesRendered_ - played : 0;
    }

    void followChangeOfTrack() {
        std::scoped_lock lock(mutex_);
        if (closing_ || !backAtStart()) return;
        framesFinished_ += bodies_.front().framesWritten;
        bodies_.pop_front();
        highestMs_ = 0;
        changedAt_ = Clock::now();
        link_->advance();
        moved_.notify_all();
    }

    // The cover's read and scaling take up to seconds from a NAS, so this is called without the lock.
    TrackDetails detailsOf(const std::wstring& file) const { return details_ ? details_(file) : TrackDetails{}; }

    _Requires_lock_held_(mutex_) Body makeBody(const StreamTrack& track, const TrackDetails& details) {
        Body body;
        body.id = nextId_++;
        body.track = track;
        body.lengthKnown = track.frames != 0;
        body.totalFrames = track.frames ? track.frames + track.leadFrames : 0;
        const uint32_t bytesPerFrame = format_.bytesPerFrame();
        std::string header;
        if (format_.type == SampleType::Dsd8) {
            const uint64_t frames = body.lengthKnown ? body.totalFrames : kUnknownLength;
            header = dsfHeader(format_, frames);
            body.totalBytes = kDsfHeader + (frames + kDsfBlock - 1) / kDsfBlock * kDsfBlock * format_.channels;
            body.contentType = "audio/x-dsf";
            body.extension = "dsf";
        } else if (formats_.wav) {
            const uint64_t data = (body.lengthKnown ? body.totalFrames : kUnknownLength) * bytesPerFrame;
            header = wavHeader(format_, data);
            body.totalBytes = kWavHeader + data;
            body.contentType = "audio/wav";
            body.extension = "wav";
        } else {
            body.totalBytes = (body.lengthKnown ? body.totalFrames : kUnknownLength) * bytesPerFrame;
            body.contentType = std::string(format_.type == SampleType::Int16 ? "audio/L16" : "audio/L24") +
                               ";rate=" + std::to_string(format_.sampleRate) + ";channels=" + std::to_string(format_.channels);
            body.extension = "pcm";
        }
        body.protocolInfo = "http-get:*:" + body.contentType + ":*";
        body.retained.assign(header.begin(), header.end());
        body.uri = base_ + "/s/" + std::to_string(generation_) + "/" + std::to_string(body.id) + "." + body.extension;
        body.coverJpeg = details.coverJpeg;
        const std::string cover = details.coverJpeg.empty()
                                      ? std::string()
                                      : base_ + "/c/" + std::to_string(generation_) + "/" + std::to_string(body.id) + ".jpg";
        const uint64_t durationMs = body.totalFrames * 1000 / std::max(format_.sampleRate, 1U);
        body.didl = upnp::didlLite(
            {details.title, details.artist, details.album, cover, body.protocolInfo, body.uri, durationMs, body.totalBytes});
        return body;
    }

    void openListener() {
        const auto* first = player_.services.empty() ? nullptr : &player_.services.front();
        const std::string host = first ? upnp::hostOf(first->controlUrl) : std::string();
        const std::string local = upnp::localAddressFor(host);
        if (local.empty()) throw Error(ErrorCode::DeviceNotFound, "the network player is not connected: no route to it");
        const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        inet_pton(AF_INET, local.c_str(), &address.sin_addr);
        if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(listener, 8) != 0) {
            closesocket(listener);
            throw Error(ErrorCode::DeviceNotFound, "cannot serve the stream");
        }
        int length = sizeof(address);
        getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
        listener_ = listener;
        base_ = "http://" + local + ":" + std::to_string(ntohs(address.sin_port));
    }

    void acceptLoop() {
        for (;;) {
            const SOCKET client = accept(listener_, nullptr, nullptr);
            if (client == kNoSocket) return;
            std::scoped_lock lock(mutex_);
            if (closing_) {
                closesocket(client);
                return;
            }
            clients_.push_back(client);
            connections_.emplace_back([this, client] { serve(client); });
        }
    }

    // Tells the player what follows once that track has data: a player opens the URL at once, and one that finds nothing
    // to read gives up on it and reopens it at the change, with a gap (the DMP-A6 does).
    void announceNext() {
        std::unique_lock lock(mutex_);
        if (closing_ || released_ || !gapless() || !started_ || !source_) return;
        const auto next = source_->nextStreamTrack();
        // A next track the Engine took back, or put another in place of, goes with its body. The Engine can do so only
        // before rendering any of it, and the player hears of a body only once it has data, so it never heard of this one.
        if (bodies_.size() > 1 && !bodies_.back().framesWritten && !bodies_.back().complete &&
            (!next || next->sequence != bodies_.back().track.sequence)) {
            bodies_.pop_back();
            queuedNext_ = 0;
        }
        if (next && next->sequence != queuedNext_) {
            const StreamTrack& track = *next;
            lock.unlock();
            const TrackDetails details = detailsOf(track.file);
            lock.lock();
            if (closing_ || released_ || !source_) return;
            // Taken back or replaced meanwhile: the next pass sees what is queued now.
            const auto still = source_->nextStreamTrack();
            if (!still || still->sequence != track.sequence) return;
            queuedNext_ = track.sequence;
            bodies_.push_back(makeBody(track, details));
        }
        if (bodies_.size() < 2 || !bodies_.front().complete || bodies_[1].id == announcedBody_ || !mayRender()) return;
        announcedBody_ = bodies_[1].id;
        const std::string uri = bodies_[1].uri, didl = bodies_[1].didl;
        lock.unlock();
        if (!link_->loadNext(uri, didl)) log::warn("network player %s refused the next track", player_.name.c_str());
    }

    // The player finished a track before it heard of the next, as with a very short one: it starts that one like any other.
    void startLateNext() {
        std::string uri, didl;
        {
            std::scoped_lock lock(mutex_);
            if (closing_ || released_ || !started_ || !playing_ || !link_->stopped() || bodies_.size() < 2 ||
                !bodies_.front().complete)
                return;
            framesFinished_ += bodies_.front().framesWritten;
            bodies_.pop_front();
            highestMs_ = 0;
            changedAt_ = Clock::now();
            uri = bodies_.front().uri;
            didl = bodies_.front().didl;
        }
        if (link_->load(uri, didl)) {
            {
                std::scoped_lock lock(mutex_);
                askedToPlay_ = Clock::now();
            }
            if (link_->play()) return;
        }
        log::warn("network player %s refused the next track", player_.name.c_str());
    }

    // Renders on once every track is sent, for the Engine to reach its end or its next track.
    void announceLoop() {
        for (;;) {
            bool sent = false;
            {
                std::unique_lock lock(mutex_);
                const auto due = Clock::now() + milliseconds(50);
                while (!closing_)
                    if (moved_.wait_until(lock, due) == std::cv_status::timeout) break;
                if (closing_) return;
                sent = !bodies_.empty() && std::ranges::all_of(bodies_, &Body::complete);
            }
            followChangeOfTrack();
            announceNext();
            startLateNext();
            checkStarted();
            if (sent) produce();
        }
    }

    _Requires_lock_held_(mutex_) uint64_t positionInFile(const Body& body, uint64_t positionMs) const {
        const uint32_t rate = std::max(format_.sampleRate, 1U);
        const uint64_t leadMs = body.track.leadFrames * 1000 / rate;
        return (positionMs > leadMs ? positionMs - leadMs : 0) + body.track.startFrame * 1000 / rate;
    }

    void forget(SOCKET client) {
        std::scoped_lock lock(mutex_);
        if (std::erase(clients_, client)) closesocket(client);
    }

    static bool sendAll(SOCKET client, const char* data, size_t size) {
        for (size_t sent = 0; sent < size;) {
            const int n = send(client, data + sent, int(std::min<size_t>(size - sent, 1U << 20)), 0);
            if (n <= 0) return false;
            sent += size_t(n);
        }
        return true;
    }

    void serve(SOCKET client) {
        const BOOL noDelay = TRUE;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
        std::string request;
        char buffer[4096];
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < sizeof(buffer) * 4) {
            const int read = recv(client, buffer, sizeof(buffer), 0);
            if (read <= 0) break;
            request.append(buffer, size_t(read));
        }
        answer(client, request);
        forget(client);
    }

    void answer(SOCKET client, const std::string& request) {
        const auto space = request.find(' ');
        if (space == std::string::npos) return;
        log::info("network player asks: %s", request.substr(0, request.find("\r\n")).c_str());
        const std::string method = request.substr(0, space);
        const std::string path = request.substr(space + 1, request.find(' ', space + 1) - space - 1);
        const bool head = method == "HEAD";
        if (method != "GET" && !head) return respond(client, "405 Method Not Allowed");
        unsigned generation = 0, id = 0;
        char kind = 0;
        if (sscanf_s(path.c_str(), "/%c/%u/%u.", &kind, 1, &generation, &id) != 3) return respond(client, "404 Not Found");
        std::unique_lock lock(mutex_);
        if (generation != generation_ || closing_) {
            lock.unlock();
            return respond(client, "404 Not Found");
        }
        Body* body = find(id);
        if (!body) {
            lock.unlock();
            return respond(client, "404 Not Found");
        }
        if (kind == 'c') {
            const std::string cover = body->coverJpeg;
            lock.unlock();
            const std::string reply =
                "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: " + std::to_string(cover.size()) +
                "\r\nConnection: close\r\n\r\n";
            sendAll(client, reply.data(), reply.size());
            if (!head) sendAll(client, cover.data(), cover.size());
            return;
        }
        uint64_t start = 0;
        std::optional<uint64_t> end;
        const bool ranged = rangeOf(request, start, end);
        const uint64_t total = body->totalBytes;
        if (start >= total && total) {
            lock.unlock();
            return respond(client, "416 Range Not Satisfiable");
        }
        // Further than the stream has got, or before what it keeps: the player seeked, which restarts the stream there.
        if (start < body->retainedFrom || start > body->produced() + kRetainBytes) {
            const uint64_t frames = (start > kWavHeader ? start - kWavHeader : 0) / std::max(format_.bytesPerFrame(), 1U);
            const uint64_t positionMs = positionInFile(*body, frames * 1000 / std::max(format_.sampleRate, 1U));
            lock.unlock();
            if (follow_) follow_({PlayerEventKind::Seeked, positionMs});
            return respond(client, "503 Service Unavailable");
        }
        const uint64_t last = end ? std::min(*end, total - 1) : total - 1;
        std::string reply = ranged ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n";
        reply += "Content-Type: " + body->contentType + "\r\nContent-Length: " + std::to_string(last - start + 1) +
                 "\r\nAccept-Ranges: bytes\r\ntransferMode.dlna.org: Streaming\r\nConnection: close\r\n";
        if (ranged)
            reply += "Content-Range: bytes " + std::to_string(start) + "-" + std::to_string(last) + "/" + std::to_string(total) +
                     "\r\n";
        reply += "\r\n";
        lock.unlock();
        if (!sendAll(client, reply.data(), reply.size()) || head) return;
        stream(client, id, start, last + 1);
    }

    void respond(SOCKET client, const std::string& status) {
        const std::string reply = "HTTP/1.1 " + status + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        sendAll(client, reply.data(), reply.size());
    }

    static bool rangeOf(const std::string& request, uint64_t& start, std::optional<uint64_t>& end) {
        std::string lowered = request;
        std::ranges::transform(lowered, lowered.begin(), [](unsigned char c) { return char(tolower(c)); });
        const auto at = lowered.find("\r\nrange: bytes=");
        if (at == std::string::npos) return false;
        unsigned long long first = 0, last = 0;
        const int read = sscanf_s(lowered.c_str() + at + 15, "%llu-%llu", &first, &last);
        if (read < 1) return false;
        start = first;
        if (read == 2) end = last;
        return true;
    }

    _Requires_lock_held_(mutex_) Body* find(uint32_t id) {
        const auto found = std::ranges::find(bodies_, id, &Body::id);
        return found == bodies_.end() ? nullptr : &*found;
    }

    void stream(SOCKET client, uint32_t id, uint64_t from, uint64_t to) {
        std::vector<uint8_t> chunk;
        uint64_t offset = from;
        while (offset < to) {
            std::unique_lock lock(mutex_);
            if (closing_) return;
            Body* body = find(id);
            if (!body) return;
            if (offset < body->produced()) {
                if (offset < body->retainedFrom) return;
                const auto begin = size_t(offset - body->retainedFrom);
                const auto size = std::min<size_t>({body->retained.size() - begin, size_t{256} * 1024, size_t(to - offset)});
                chunk.assign(body->retained.begin() + std::ptrdiff_t(begin),
                             body->retained.begin() + std::ptrdiff_t(begin + size));
                lock.unlock();
                if (!sendAll(client, reinterpret_cast<const char*>(chunk.data()), chunk.size())) return;
                offset += size;
                continue;
            }
            if (body->complete) return; // an unknown length ended here
            lock.unlock();
            if (!produce()) std::this_thread::sleep_for(milliseconds(10));
        }
    }

    _Requires_lock_held_(mutex_) bool mayRender() {
        if (!source_ || !started_ || !playing_) return false;
        const auto now = Clock::now();
        const uint64_t played = allPlayed();
        // A player whose position stands still may want more in its buffer before it goes on (a Linkplay module does).
        const bool waiting = playingSince_ != Clock::time_point{} && now - std::max(playingSince_, lastAdvance_) > kPatience;
        const auto ahead = bodies_.size() > 1 ? options_.nextTrackLead : waiting ? options_.aheadBeforePlay : options_.ahead;
        return framesRendered_ < played + uint64_t(ahead.count()) * format_.sampleRate / 1000;
    }

    // The source outlives the renderers: `close` joins them before it clears the source.
    _Requires_lock_held_(mutex_) IRenderSource* renderSource() const { return source_; }

    // Renders a chunk into the track the stream has reached. False when there is nothing to render now.
    bool produce() {
        std::scoped_lock rendering(renderMutex_);
        size_t frames = 0;
        size_t boundary = IRenderSource::kNoBoundary;
        bool ended = false;
        const uint32_t bytesPerFrame = [&] {
            std::scoped_lock lock(mutex_);
            return format_.bytesPerFrame();
        }();
        rendered_.resize(kChunkFrames * bytesPerFrame);
        {
            std::unique_lock lock(mutex_);
            if (closing_ || !source_) return false;
            if (Body* body = producing(); body && !carry_.empty()) {
                write(*body, carry_.data(), carry_.size() / bytesPerFrame);
                carry_.clear();
            }
            // Frames past the last track belong to one queued too late to have a URL yet.
            if ((!carry_.empty() && !producing()) || !mayRender()) return false;
            auto* source = renderSource();
            lock.unlock();
            frames = source->render(rendered_.data(), kChunkFrames);
            boundary = source->boundaryInLastRender();
            ended = source->renderEnded();
        }
        std::scoped_lock lock(mutex_);
        Body* body = producing();
        framesRendered_ += frames;
        if (!body) {
            carry_.assign(rendered_.begin(), rendered_.begin() + std::ptrdiff_t(frames * bytesPerFrame));
            return frames != 0;
        }
        if (boundary != IRenderSource::kNoBoundary && boundary <= frames) {
            write(*body, rendered_.data(), boundary);
            finish(*body);
            body->rendered = true;
            carry_.assign(rendered_.begin() + std::ptrdiff_t(boundary * bytesPerFrame),
                          rendered_.begin() + std::ptrdiff_t(frames * bytesPerFrame));
        } else if (frames) {
            write(*body, rendered_.data(), frames);
        }
        if (ended && !frames) {
            finish(*body);
            body->rendered = true;
        }
        return frames != 0;
    }

    // The body of the track the source renders: one complete at its told length takes the rest of its track, dropped.
    _Requires_lock_held_(mutex_) Body* producing() {
        const auto found = std::ranges::find_if(bodies_, [](const Body& b) { return !b.rendered; });
        return found == bodies_.end() ? nullptr : &*found;
    }

    _Requires_lock_held_(mutex_) void write(Body& body, const uint8_t* data, size_t frames) {
        if (body.lengthKnown && body.framesWritten + frames > body.totalFrames) {
            const auto kept = size_t(body.totalFrames - body.framesWritten);
            framesFinished_ += frames - kept; // never to be played
            frames = kept;
        }
        const size_t bytes = frames * format_.bytesPerFrame();
        if (format_.type == SampleType::Dsd8) {
            for (size_t i = 0; i < frames; ++i) {
                body.dsfLeft.push_back(reverseBits(data[i * 2]));
                body.dsfRight.push_back(reverseBits(data[i * 2 + 1]));
                if (body.dsfLeft.size() == kDsfBlock) flushDsf(body);
            }
        } else if (body.extension == "pcm") {
            const size_t width = audio::bytesPerSample(format_.type);
            const size_t at = body.retained.size();
            body.retained.resize(at + bytes);
            for (size_t i = 0; i < bytes; i += width)
                std::reverse_copy(data + i, data + i + width, body.retained.begin() + std::ptrdiff_t(at + i));
        } else {
            body.retained.insert(body.retained.end(), data, data + bytes);
        }
        body.framesWritten += frames;
        if (body.lengthKnown && body.framesWritten == body.totalFrames) finish(body);
        trim(body);
        moved_.notify_all();
    }

    static void flushDsf(Body& body) {
        if (body.dsfLeft.empty()) return;
        body.dsfLeft.resize(kDsfBlock);
        body.dsfRight.resize(kDsfBlock);
        body.retained.insert(body.retained.end(), body.dsfLeft.begin(), body.dsfLeft.end());
        body.retained.insert(body.retained.end(), body.dsfRight.begin(), body.dsfRight.end());
        body.dsfLeft.clear();
        body.dsfRight.clear();
    }

    // What is missing of a length that was only estimated is silence, as the player was told the length. Zeros are not
    // silence in DSD.
    void finish(Body& body) {
        if (body.complete) return;
        if (body.lengthKnown && format_.type == SampleType::Dsd8 && body.framesWritten < body.totalFrames) {
            log::info("network player: %s ended %llu frames short of its length", toUtf8(body.track.file).c_str(),
                      static_cast<unsigned long long>(body.totalFrames - body.framesWritten));
            for (uint64_t frame = body.framesWritten; frame < body.totalFrames; ++frame) {
                body.dsfLeft.push_back(kDsfSilence);
                body.dsfRight.push_back(kDsfSilence);
                if (body.dsfLeft.size() == kDsfBlock) flushDsf(body);
            }
        }
        flushDsf(body);
        if (body.lengthKnown && body.produced() < body.totalBytes) {
            log::info("network player: %s ended %llu bytes short of its length", toUtf8(body.track.file).c_str(),
                      static_cast<unsigned long long>(body.totalBytes - body.produced()));
            body.retained.resize(body.retained.size() + size_t(body.totalBytes - body.produced()));
        }
        if (!body.lengthKnown) body.totalBytes = body.produced();
        body.complete = true;
        moved_.notify_all();
    }

    static void trim(Body& body) {
        if (body.retained.size() <= 2 * kRetainBytes) return;
        const size_t drop = body.retained.size() - kRetainBytes;
        body.retained.erase(body.retained.begin(), body.retained.begin() + std::ptrdiff_t(drop));
        body.retainedFrom += drop;
    }

    upnp::PlayerDescription player_;
    std::function<TrackDetails(const std::wstring&)> details_;
    std::function<void(const PlayerEvent&)> follow_;
    NetworkOutputOptions options_;
    upnp::PlayerFormats formats_;
    bool gapless_ = false;
    bool formatsRead_ = false, gaplessRead_ = false;
    std::unique_ptr<PlayerLink> link_;

    SOCKET listener_ = kNoSocket;
    std::string base_;
    std::thread acceptor_, announcer_;
    std::vector<std::thread> connections_;
    mutable std::mutex mutex_;
    std::condition_variable moved_;
    _Guarded_by_(mutex_) std::vector<SOCKET> clients_;
    _Guarded_by_(mutex_) DeviceFormat format_ {};
    _Guarded_by_(mutex_) IRenderSource* source_ = nullptr;
    _Guarded_by_(mutex_) std::deque<Body> bodies_;
    _Guarded_by_(mutex_) std::vector<uint8_t> carry_; // frames past a boundary, for the track that has no body yet
    _Guarded_by_(mutex_) uint32_t generation_ = 0;
    _Guarded_by_(mutex_) uint32_t nextId_ = 0;
    _Guarded_by_(mutex_) uint64_t queuedNext_ = 0;
    _Guarded_by_(mutex_) uint32_t announcedBody_ = 0;
    _Guarded_by_(mutex_) uint64_t framesRendered_ = 0;
    _Guarded_by_(mutex_) uint64_t framesFinished_ = 0;
    _Guarded_by_(mutex_) bool started_ = false;
    _Guarded_by_(mutex_) bool playing_ = false;
    _Guarded_by_(mutex_) bool closing_ = false;
    _Guarded_by_(mutex_) bool released_ = false;
    _Guarded_by_(mutex_) Clock::time_point playingSince_;
    _Guarded_by_(mutex_) Clock::time_point pausedAt_; // while paused, since when
    _Guarded_by_(mutex_) Clock::duration pausedFor_ {};
    _Guarded_by_(mutex_) Clock::time_point askedToPlay_;
    _Guarded_by_(mutex_) mutable uint64_t highestMs_ = 0; // of the track playing
    _Guarded_by_(mutex_) mutable bool reportsPosition_ = false;
    _Guarded_by_(mutex_) Clock::time_point changedAt_;
    _Guarded_by_(mutex_) mutable uint64_t lastReported_ = 0;
    _Guarded_by_(mutex_) mutable uint64_t clockMs_ = 0;
    _Guarded_by_(mutex_) mutable Clock::time_point clockAt_;
    _Guarded_by_(mutex_) mutable Clock::time_point lastAdvance_;

    std::mutex renderMutex_; // one renderer at a time; taken before mutex_
    _Guarded_by_(renderMutex_) std::vector<uint8_t> rendered_;
};

} // namespace

std::unique_ptr<audio::IOutput> createNetworkOutput(upnp::PlayerDescription player,
                                                    std::function<TrackDetails(const std::wstring&)> details,
                                                    std::function<void(const PlayerEvent&)> follow,
                                                    NetworkOutputOptions options) {
    return std::make_unique<NetworkOutput>(std::move(player), std::move(details), std::move(follow), options);
}

} // namespace camel::server
