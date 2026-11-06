// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Network/AirPlayOutput.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <format>
#include <limits>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "Common/util/Log.h"
#include "Server/Network/Rtsp.h"

#pragma comment(lib, "ws2_32.lib")

namespace camel::server {

namespace {

using namespace airplay;
using audio::DeviceFormat;
using audio::Error;
using audio::ErrorCode;
using audio::IRenderSource;
using audio::OutputStatus;
using audio::SampleType;
using audio::StreamTrack;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;
using std::chrono::seconds;

constexpr size_t kBytesPerFrame = 4;
constexpr uint64_t kPrefillFrames = 11025;         // sent at once, so that a late wake-up never reaches the receiver
constexpr uint64_t kMaxBehindFrames = kSampleRate; // later than this, the stream goes on from now
constexpr size_t kHistoryPackets = uint64_t{4} * kSampleRate / kFramesPerPacket; // for resends, and for what a pause flushed
constexpr auto kSyncEvery = seconds(1);
constexpr auto kPollSockets = milliseconds(20);
constexpr auto kWaitForFrames = milliseconds(5);
constexpr uint64_t kUnixToNtpSeconds = 2208988800ULL;
constexpr int kStatusOk = 200;
constexpr int kStatusUnauthorized = 401;
constexpr int kStatusForbidden = 403;
constexpr int kStatusBusy = 453;
constexpr auto kInUse = "the receiver is in use by another source";
constexpr auto kNotConnected = "the AirPlay receiver is not connected";

struct Packet {
    uint64_t number = 0; // from the start of the stream
    uint16_t sequence = 0;
    uint32_t rtpTime = 0;
    std::string alac;
};

struct TrackNotice {
    StreamTrack track;
    uint32_t rtpTime = 0;
};

SOCKET boundUdp(uint16_t& port) {
    const SOCKET udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    if (bind(udp, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
        log::warn("AirPlay: cannot open a UDP port (%d)", WSAGetLastError());
    int length = sizeof(address);
    getsockname(udp, reinterpret_cast<sockaddr*>(&address), &length);
    port = ntohs(address.sin_port);
    return udp;
}

uint16_t portIn(std::string_view transport, std::string_view name) {
    const size_t at = transport.find(name);
    if (at == std::string_view::npos) return 0;
    const auto value = transport.substr(at + name.size());
    uint16_t port = 0;
    std::from_chars(value.data(), value.data() + value.size(), port);
    return port;
}

void check(const RtspReply& reply, std::string_view step) {
    if (reply.status == kStatusOk) return;
    if (reply.status == kStatusBusy) throw Error(ErrorCode::DeviceInUse, kInUse);
    if (reply.status == kStatusForbidden || reply.status == kStatusUnauthorized)
        throw Error(ErrorCode::FormatNotSupported, "the receiver refuses this connection");
    throw Error(ErrorCode::DriverError, std::format("the receiver answered {} to {}", reply.status, step));
}

class AirPlayOutput final : public audio::IOutput {
public:
    AirPlayOutput(Receiver receiver, AirPlayOutputOptions options)
        : receiver_(std::move(receiver)), options_(std::move(options)) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        audio_ = boundUdp(audioPort_);
        control_ = boundUdp(controlPort_);
        timing_ = boundUdp(timingPort_);
        std::random_device random;
        sequence_ = uint16_t(random());
        rtpNext_ = random();
        ssrc_ = random();
        systemStart_ = std::chrono::system_clock::now();
        steadyStart_ = Clock::now();
        service_ = std::thread([this] { serviceLoop(); });
    }

    _No_competing_thread_ ~AirPlayOutput() override {
        close();
        stopping_ = true;
        service_.join();
        {
            std::scoped_lock lock(rtspMutex_);
            endSession();
        }
        closesocket(audio_);
        closesocket(control_);
        closesocket(timing_);
        WSACleanup();
    }
    AirPlayOutput(const AirPlayOutput&) = delete;
    AirPlayOutput& operator=(const AirPlayOutput&) = delete;

    bool supports(const DeviceFormat& f) override {
        return f.sampleRate == kSampleRate && f.channels == 2 && f.type == SampleType::Int16;
    }

    void open(const DeviceFormat& f, IRenderSource* source) override {
        if (!supports(f)) throw Error(ErrorCode::FormatNotSupported, "an AirPlay receiver takes only 44.1 kHz/16-bit stereo");
        close();
        startSession();
        std::scoped_lock lock(mutex_);
        source_ = source;
        rendered_ = stoppedAt_ = 0;
        nextPacket_ = endPacket_ = runPacket_ = 0;
        pendingFrames_ = 0;
        ended_ = closing_ = false;
        history_.clear();
        notices_.push_back({source->streamTrack(), rtpNext_});
        opened_ = true;
        sender_ = std::thread([this] { sendLoop(); });
    }

    void start() override {
        std::scoped_lock lock(mutex_);
        if (running_ || !opened_) return;
        // What the last FLUSH took from the receiver, it gets again.
        runPacket_ = std::min(stoppedAt_ / kFramesPerPacket, endPacket_);
        if (!history_.empty()) runPacket_ = std::clamp(runPacket_, history_.front().number, endPacket_);
        nextPacket_ = runPacket_;
        runStart_ = nextSync_ = Clock::now();
        runRtp_ = rtpNext_;
        firstPacket_ = true;
        running_ = true;
        moved_.notify_all();
    }

    void pause() override {
        {
            std::unique_lock lock(mutex_);
            if (!running_) return;
            stoppedAt_ = heard(Clock::now());
            running_ = false;
            moved_.notify_all();
            while (!parked_ && sender_.joinable()) moved_.wait(lock);
        }
        flush();
    }

    void close() override {
        {
            std::scoped_lock lock(mutex_);
            if (!sender_.joinable()) return;
            running_ = false;
            closing_ = true;
            moved_.notify_all();
        }
        sender_.join();
        {
            std::scoped_lock lock(mutex_);
            opened_ = false;
            source_ = nullptr;
            notices_.clear();
        }
        flush();
        closeAt_ = Clock::now() + options_.closeSessionAfter;
    }

    OutputStatus status() const override {
        OutputStatus status;
        status.mode = "AirPlay · " + receiver_.name;
        std::scoped_lock lock(mutex_);
        status.heldFrames = rendered_ - std::min(rendered_, heard(Clock::now()));
        return status;
    }

private:
    // ---- the RTSP session ----

    void startSession() {
        std::scoped_lock lock(rtspMutex_);
        closeAt_ = Clock::time_point::max();
        if (client_) return;
        auto connected = connectToReceiver();
        client_ = std::move(connected);
        nextHeartbeat_ = Clock::now() + options_.heartbeat;
        volumeSent_ = std::nullopt;
    }

    std::unique_ptr<RtspClient> connectToReceiver() {
        try {
            auto client = std::make_unique<RtspClient>(receiver_.address, receiver_.port, options_.requestTimeoutMs);
            check(client->request("OPTIONS", "*"), "OPTIONS");
            check(client->request("ANNOUNCE", client->url(), "application/sdp",
                                  sessionDescription(client->localAddress(), receiver_.address, client->session())),
                  "ANNOUNCE");
            const auto transport =
                std::format("Transport: RTP/AVP/UDP;unicast;interleaved=0-1;mode=record;control_port={};timing_port={}\r\n",
                            controlPort_, timingPort_);
            const auto setup = client->request("SETUP", client->url(), {}, {}, transport);
            check(setup, "SETUP");
            setReceiverPorts(setup);
            if (const auto session = setup.headers.find("session"); session != setup.headers.end())
                client->setSession(session->second);
            const auto record = client->request("RECORD", client->url(), {}, {}, std::format("Range: npt=0-\r\n{}", rtpInfo()));
            check(record, "RECORD");
            std::scoped_lock lock(mutex_);
            audioLatency_ = kDefaultAudioLatency;
            if (const auto latency = record.headers.find("audio-latency"); latency != record.headers.end())
                std::from_chars(latency->second.data(), latency->second.data() + latency->second.size(), audioLatency_);
            return client;
        } catch (const Error&) {
            throw;
        } catch (const std::exception& e) {
            log::info("AirPlay receiver %s: %s", receiver_.name.c_str(), e.what());
            throw Error(ErrorCode::DeviceNotFound, kNotConnected);
        }
    }

    void setReceiverPorts(const RtspReply& setup) {
        const auto transport = setup.headers.find("transport");
        if (transport == setup.headers.end()) throw Error(ErrorCode::DriverError, "the receiver gave no ports");
        const uint16_t server = portIn(transport->second, "server_port=");
        const uint16_t control = portIn(transport->second, "control_port=");
        if (!server) throw Error(ErrorCode::DriverError, "the receiver gave no ports");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        inet_pton(AF_INET, receiver_.address.c_str(), &address.sin_addr);
        std::scoped_lock lock(mutex_);
        receiverAudio_ = receiverControl_ = address;
        receiverAudio_.sin_port = htons(server);
        receiverControl_.sin_port = htons(control ? control : server);
    }

    std::string rtpInfo() {
        std::scoped_lock lock(mutex_);
        return std::format("RTP-Info: seq={};rtptime={}\r\n", sequence_, rtpNext_);
    }

    void flush() {
        std::scoped_lock lock(rtspMutex_);
        if (!client_ || !sentSinceFlush_.exchange(false)) return;
        try {
            client_->request("FLUSH", client_->url(), {}, {}, std::format("Range: npt=0-\r\n{}", rtpInfo()));
        } catch (const std::exception& e) {
            log::info("AirPlay receiver %s: %s", receiver_.name.c_str(), e.what()); // the service thread finds out why
        }
    }

    _Requires_lock_held_(rtspMutex_) void endSession() {
        if (!client_) return;
        try {
            client_->request("TEARDOWN", client_->url());
        } catch (const std::exception& e) {
            log::info("AirPlay receiver %s: %s", receiver_.name.c_str(), e.what());
        }
        client_.reset();
    }

    _Requires_lock_held_(rtspMutex_) void lose(ErrorCode code, const char* why) {
        client_.reset();
        if (opened_) raiseError(Error(code, why));
    }

    _Requires_lock_held_(rtspMutex_) void tellVolume() {
        const auto wanted = options_.volumeDb ? options_.volumeDb() : std::nullopt;
        if (!wanted || wanted == volumeSent_) return;
        client_->request("SET_PARAMETER", client_->url(), "text/parameters", std::format("volume: {:.6f}\r\n", *wanted));
        volumeSent_ = wanted;
    }

    _Requires_lock_held_(rtspMutex_) void tellTrack(const TrackNotice& notice, const TrackDetails& details) {
        const auto at = std::format("RTP-Info: rtptime={}\r\n", notice.rtpTime);
        const std::string url = client_->url();
        client_->request("SET_PARAMETER", url, "application/x-dmap-tagged",
                         dmapTags(details.title, details.artist, details.album), at);
        if (!details.coverJpeg.empty()) client_->request("SET_PARAMETER", url, "image/jpeg", details.coverJpeg, at);
        if (notice.track.frames) {
            const uint32_t start = notice.rtpTime - uint32_t(notice.track.startFrame);
            const uint32_t end = notice.rtpTime + uint32_t(notice.track.frames);
            client_->request("SET_PARAMETER", url, "text/parameters",
                             std::format("progress: {}/{}/{}\r\n", start, notice.rtpTime, end));
        }
    }

    // A connection the receiver closes means another sender took it; one that stops answering means it has gone.
    void tendSession(const std::optional<TrackNotice>& notice, const TrackDetails& details) {
        std::scoped_lock lock(rtspMutex_);
        if (!client_) return;
        try {
            if (client_->closed()) throw RtspClosed();
            tellVolume();
            if (notice) tellTrack(*notice, details);
            const auto now = Clock::now();
            if (now >= closeAt_.load()) {
                endSession();
            } else if (now >= nextHeartbeat_) {
                nextHeartbeat_ = now + options_.heartbeat;
                client_->request("OPTIONS", "*");
            }
        } catch (const RtspClosed&) {
            lose(ErrorCode::DeviceInUse, kInUse);
        } catch (const std::exception& e) {
            log::info("AirPlay receiver %s: %s", receiver_.name.c_str(), e.what());
            lose(ErrorCode::DeviceLost, kNotConnected);
        }
    }

    std::optional<TrackNotice> nextNotice() {
        std::scoped_lock lock(mutex_);
        if (notices_.empty()) return std::nullopt;
        auto notice = std::move(notices_.front());
        notices_.pop_front();
        return notice;
    }

    // ---- UDP ----

    uint64_t ntpNow() const {
        const auto now = systemStart_ + (Clock::now() - steadyStart_);
        const auto since = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
        const auto whole = uint64_t(since / 1000000000);
        const auto part = uint64_t(since % 1000000000);
        return (whole + kUnixToNtpSeconds) << 32 | (part << 32) / 1000000000;
    }

    void answerTiming() {
        std::array<char, 64> buffer{};
        sockaddr_in from{};
        int length = sizeof(from);
        const int got = recvfrom(timing_, buffer.data(), int(buffer.size()), 0, reinterpret_cast<sockaddr*>(&from), &length);
        if (got <= 0) return;
        const uint64_t received = ntpNow();
        const auto reply = timingReply(std::string_view(buffer.data(), size_t(got)), received, ntpNow());
        if (!reply.empty())
            sendto(timing_, reply.data(), int(reply.size()), 0, reinterpret_cast<const sockaddr*>(&from), sizeof(from));
    }

    void answerResend() {
        std::array<char, 64> buffer{};
        sockaddr_in from{};
        int length = sizeof(from);
        const int got = recvfrom(control_, buffer.data(), int(buffer.size()), 0, reinterpret_cast<sockaddr*>(&from), &length);
        if (got <= 0) return;
        const auto request = parseResend(std::string_view(buffer.data(), size_t(got)));
        if (!request) return;
        std::scoped_lock lock(mutex_);
        for (uint16_t i = 0; i < request->count; ++i) {
            const auto sequence = uint16_t(request->first + i);
            const auto found = std::ranges::find_if(history_, [&](const Packet& p) { return p.sequence == sequence; });
            if (found == history_.end()) continue;
            const auto packet = resendPacket(sequence, rtpPacket(sequence, found->rtpTime, ssrc_, false, found->alac));
            sendto(control_, packet.data(), int(packet.size()), 0, reinterpret_cast<const sockaddr*>(&from), sizeof(from));
        }
    }

    void serviceLoop() {
        while (!stopping_) {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(control_, &readable);
            FD_SET(timing_, &readable);
            const timeval wait{0, long(std::chrono::microseconds(kPollSockets).count())};
            if (select(0, &readable, nullptr, nullptr, &wait) > 0) {
                if (FD_ISSET(timing_, &readable)) answerTiming();
                if (FD_ISSET(control_, &readable)) answerResend();
            }
            const auto notice = nextNotice();
            const TrackDetails details = notice && options_.details ? options_.details(notice->track.file) : TrackDetails{};
            tendSession(notice, details);
        }
    }

    // ---- the stream; `mutex_` is held unless a function says otherwise ----

    _Requires_lock_held_(mutex_) uint64_t framesDue(Clock::time_point now) const {
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - runStart_).count();
        return kPrefillFrames + uint64_t(std::max<int64_t>(elapsed, 0)) * kSampleRate / 1000000;
    }

    _Requires_lock_held_(mutex_) uint64_t heard(Clock::time_point now) const {
        if (!running_) return stoppedAt_;
        const uint64_t due = framesDue(now);
        const uint64_t latency = kSyncLatency + audioLatency_;
        return std::min(runPacket_ * kFramesPerPacket + (due > latency ? due - latency : 0), rendered_);
    }

    void sendTo(const sockaddr_in& target, const std::string& packet) const {
        sendto(audio_, packet.data(), int(packet.size()), 0, reinterpret_cast<const sockaddr*>(&target), sizeof(target));
    }

    _Requires_lock_held_(mutex_) void sendSync(Clock::time_point now) {
        const uint32_t head = runRtp_ + uint32_t(framesDue(now));
        const auto packet = syncPacket(firstPacket_, head - kSyncLatency, ntpNow(), head);
        sendto(control_, packet.data(), int(packet.size()), 0, reinterpret_cast<const sockaddr*>(&receiverControl_),
               sizeof(receiverControl_));
        nextSync_ = now + kSyncEvery;
    }

    // The source ran dry for longer than the receiver's buffer, whose time has passed: what comes next is heard late.
    _Requires_lock_held_(mutex_) void goOnFromNow(Clock::time_point now) {
        const uint64_t sent = (nextPacket_ - runPacket_) * kFramesPerPacket;
        const uint64_t due = framesDue(now);
        if (ended_ || due < sent + kPrefillFrames + kMaxBehindFrames) return;
        runStart_ += std::chrono::microseconds((due - sent - kPrefillFrames) * 1000000 / kSampleRate);
    }

    _Requires_lock_held_(mutex_) IRenderSource* renderSource() const { return source_; }

    // Runs without `mutex_`, which close() needs: the source stays valid until this thread ends.
    bool encodeNext(IRenderSource* source) {
        const size_t want = kFramesPerPacket - pendingFrames_;
        const size_t frames = source->render(pcm_.data() + pendingFrames_ * kBytesPerFrame, want);
        const size_t boundary = source->boundaryInLastRender();
        const bool ended = source->renderEnded();
        std::optional<StreamTrack> next;
        if (boundary != IRenderSource::kNoBoundary) next = source->nextStreamTrack();

        std::scoped_lock lock(mutex_);
        if (next) {
            const uint64_t frame = rendered_ + boundary - runPacket_ * kFramesPerPacket;
            notices_.push_back({*next, runRtp_ + uint32_t(frame)});
        }
        rendered_ += frames;
        pendingFrames_ += frames;
        ended_ = ended;
        if (pendingFrames_ < kFramesPerPacket && (!ended || !pendingFrames_)) return false;
        std::fill(pcm_.begin() + std::ptrdiff_t(pendingFrames_ * kBytesPerFrame), pcm_.end(), uint8_t(0));
        history_.push_back({endPacket_++, 0, 0, alacFrame(pcm_.data())});
        if (history_.size() > kHistoryPackets) history_.pop_front();
        pendingFrames_ = 0;
        return true;
    }

    _Requires_lock_held_(mutex_)
        _Requires_lock_held_(lock) bool sendDue(Clock::time_point now, std::unique_lock<std::mutex>& lock) {
        if ((nextPacket_ - runPacket_) * kFramesPerPacket >= framesDue(now)) return false;
        if (nextPacket_ == endPacket_) {
            if (ended_) return false;
            auto* source = renderSource();
            lock.unlock();
            const bool encoded = encodeNext(source);
            lock.lock();
            if (!encoded) return false;
        }
        Packet& packet = history_[size_t(nextPacket_ - history_.front().number)];
        packet.sequence = sequence_++;
        packet.rtpTime = rtpNext_;
        rtpNext_ += kFramesPerPacket;
        sendTo(receiverAudio_, rtpPacket(packet.sequence, packet.rtpTime, ssrc_, firstPacket_, packet.alac));
        firstPacket_ = false;
        sentSinceFlush_ = true;
        ++nextPacket_;
        return true;
    }

    void sendLoop() {
        std::unique_lock lock(mutex_);
        while (!closing_) {
            if (!running_) {
                parked_ = true;
                moved_.notify_all();
                while (!running_ && !closing_) moved_.wait(lock);
                parked_ = false;
                continue;
            }
            const auto now = Clock::now();
            if (now >= nextSync_) sendSync(now);
            goOnFromNow(now);
            if (!sendDue(now, lock)) moved_.wait_for(lock, kWaitForFrames);
        }
        parked_ = false;
        moved_.notify_all();
    }

    Receiver receiver_;
    AirPlayOutputOptions options_;
    SOCKET audio_ = INVALID_SOCKET;
    SOCKET control_ = INVALID_SOCKET;
    SOCKET timing_ = INVALID_SOCKET;
    uint16_t audioPort_ = 0, controlPort_ = 0, timingPort_ = 0;
    std::chrono::system_clock::time_point systemStart_;
    Clock::time_point steadyStart_;
    std::thread service_, sender_;
    std::atomic<bool> stopping_ = false, opened_ = false, sentSinceFlush_ = false;

    std::mutex rtspMutex_; // taken before `mutex_`
    _Guarded_by_(rtspMutex_) std::unique_ptr<RtspClient> client_;
    _Guarded_by_(rtspMutex_) std::optional<double> volumeSent_;
    _Guarded_by_(rtspMutex_) Clock::time_point nextHeartbeat_;
    std::atomic<Clock::time_point> closeAt_ = Clock::time_point::max();

    mutable std::mutex mutex_;
    std::condition_variable moved_;
    _Guarded_by_(mutex_) IRenderSource* source_ = nullptr;
    _Guarded_by_(mutex_) sockaddr_in receiverAudio_ {};
    _Guarded_by_(mutex_) sockaddr_in receiverControl_ {};
    _Guarded_by_(mutex_) uint32_t audioLatency_ = kDefaultAudioLatency;
    _Guarded_by_(mutex_) bool running_ = false;
    _Guarded_by_(mutex_) bool parked_ = false;
    _Guarded_by_(mutex_) bool closing_ = false;
    _Guarded_by_(mutex_) bool ended_ = false;
    _Guarded_by_(mutex_) bool firstPacket_ = true;
    _Guarded_by_(mutex_) uint16_t sequence_ = 0;
    _Guarded_by_(mutex_) uint32_t rtpNext_ = 0;
    _Guarded_by_(mutex_) uint32_t ssrc_ = 0;
    _Guarded_by_(mutex_) uint32_t runRtp_ = 0;
    _Guarded_by_(mutex_) uint64_t rendered_ = 0;
    _Guarded_by_(mutex_) uint64_t stoppedAt_ = 0;
    _Guarded_by_(mutex_) uint64_t nextPacket_ = 0;
    _Guarded_by_(mutex_) uint64_t endPacket_ = 0;
    _Guarded_by_(mutex_) uint64_t runPacket_ = 0;
    _Guarded_by_(mutex_) Clock::time_point runStart_;
    _Guarded_by_(mutex_) Clock::time_point nextSync_;
    _Guarded_by_(mutex_) std::deque<Packet> history_;
    _Guarded_by_(mutex_) std::deque<TrackNotice> notices_;

    // The sender thread's alone, reset by open() before it starts.
    std::array<uint8_t, kFramesPerPacket * kBytesPerFrame> pcm_{};
    size_t pendingFrames_ = 0;
};

} // namespace

std::unique_ptr<audio::IOutput> createAirPlayOutput(airplay::Receiver receiver, AirPlayOutputOptions options) {
    return std::make_unique<AirPlayOutput>(std::move(receiver), std::move(options));
}

} // namespace camel::server
