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
    std::string pcm;
    std::array<std::string, 3> alac{}; // by AirPlayChannels, made when a receiver first needs it

    const std::string& frame(AirPlayChannels channels) {
        auto& made = alac[size_t(channels)];
        if (made.empty()) made = alacFrame(reinterpret_cast<const uint8_t*>(oneChannel(pcm, channels).data()));
        return made;
    }

    // One channel in both, as a receiver set to it plays.
    static std::string oneChannel(std::string pcm, AirPlayChannels channels) {
        if (channels == AirPlayChannels::Stereo) return pcm;
        const size_t from = channels == AirPlayChannels::Left ? 0 : 2, to = 2 - from;
        for (size_t at = 0; at + 4 <= pcm.size(); at += 4) {
            pcm[at + to] = pcm[at + from];
            pcm[at + to + 1] = pcm[at + from + 1];
        }
        return pcm;
    }
};

struct TrackNotice {
    StreamTrack track;
    uint32_t rtpTime = 0;
};

// Where a speaker takes its packets.
struct Destination {
    sockaddr_in audio{}, control{};
};

struct Connected {
    std::unique_ptr<RtspClient> client;
    Destination destination;
    uint32_t latency = kDefaultAudioLatency;
};

// A receiver of the group: one session for it, or one for each speaker of a stereo pair.
struct Member {
    std::string id, name;
    AirPlayChannels channels = AirPlayChannels::Stereo;
    std::vector<std::unique_ptr<RtspClient>> sessions;
    std::vector<Destination> destinations;
    uint32_t latency = kDefaultAudioLatency;
    std::optional<double> volumeSent;
};

// A speaker as the stream sends to it.
struct Target {
    std::string member;
    Destination destination;
    AirPlayChannels channels = AirPlayChannels::Stereo;
    bool firstSync = true, firstPacket = true; // since its RECORD or the last start
};

bool sameAddress(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

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
    explicit AirPlayOutput(AirPlayOutputOptions options) : options_(std::move(options)) {
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
        session_ = std::thread([this] { sessionLoop(); });
    }

    _No_competing_thread_ ~AirPlayOutput() override {
        close();
        stopping_ = true;
        service_.join();
        session_.join();
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
        for (auto& target : targets_) target.firstSync = target.firstPacket = true;
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
        std::scoped_lock lock(mutex_);
        status.mode = "AirPlay · " + names_;
        status.heldFrames = rendered_ - std::min(rendered_, heard(Clock::now()));
        return status;
    }

private:
    // ---- the RTSP session ----

    // Every receiver of the group that is found; the stream opens when one of them takes it, and the others are told
    // as leaving. With none, the first one's reason is the output's.
    void startSession() {
        std::scoped_lock lock(rtspMutex_);
        closeAt_ = Clock::time_point::max();
        if (!members_.empty()) return;
        std::vector<std::pair<std::string, Error>> refused;
        for (const auto& wanted : options_.members()) {
            if (!wanted.receiver) continue;
            try {
                members_.push_back(connectMember(wanted));
            } catch (const Error& e) {
                refused.emplace_back(wanted.id, e);
            }
        }
        if (members_.empty()) throw refused.empty() ? Error(ErrorCode::DeviceNotFound, kNotConnected) : refused.front().second;
        for (const auto& [id, why] : refused) tellLeft(id, why);
        publishTargets();
        nextHeartbeat_ = Clock::now() + options_.heartbeat;
    }

    Member connectMember(const AirPlayMember& wanted) {
        const Receiver& receiver = *wanted.receiver;
        Member member{wanted.id, receiver.name, receiver.others.empty() ? wanted.channels : AirPlayChannels::Stereo};
        std::vector<Speaker> speakers{{receiver.address, receiver.port}};
        speakers.insert(speakers.end(), receiver.others.begin(), receiver.others.end());
        for (const auto& speaker : speakers) {
            auto connected = connectToSpeaker(speaker, receiver.name);
            member.sessions.push_back(std::move(connected.client));
            member.destinations.push_back(connected.destination);
            member.latency = std::max(member.latency, connected.latency);
        }
        return member;
    }

    // The stream's view of the members: where to send, in which channels, and the longest latency.
    _Requires_lock_held_(rtspMutex_) void publishTargets() {
        std::string names;
        for (const auto& member : members_) names += (names.empty() ? "" : " + ") + member.name;
        std::scoped_lock lock(mutex_);
        std::vector<Target> targets;
        uint32_t latency = kDefaultAudioLatency;
        bool added = false;
        for (const auto& member : members_) {
            latency = std::max(latency, member.latency);
            for (const auto& destination : member.destinations) {
                const auto was = std::ranges::find_if(targets_, [&](const Target& t) {
                    return t.member == member.id && sameAddress(t.destination.audio, destination.audio);
                });
                Target target = was == targets_.end() ? Target{member.id, destination} : *was;
                target.channels = member.channels;
                added |= was == targets_.end();
                targets.push_back(std::move(target));
            }
        }
        targets_ = std::move(targets);
        audioLatency_ = latency;
        names_ = std::move(names);
        if (added) nextSync_ = Clock::now(); // a receiver added hears the clock before its first packet
    }

    void tellLeft(const std::string& id, const Error& why) const {
        if (options_.memberLeft) options_.memberLeft(id, why);
    }

    Connected connectToSpeaker(const Speaker& speaker, const std::string& name) {
        try {
            auto client = std::make_unique<RtspClient>(speaker.address, speaker.port, options_.requestTimeoutMs);
            check(client->request("OPTIONS", "*"), "OPTIONS");
            check(client->request("ANNOUNCE", client->url(), "application/sdp",
                                  sessionDescription(client->localAddress(), speaker.address, client->session())),
                  "ANNOUNCE");
            const auto transport =
                std::format("Transport: RTP/AVP/UDP;unicast;interleaved=0-1;mode=record;control_port={};timing_port={}\r\n",
                            controlPort_, timingPort_);
            const auto setup = client->request("SETUP", client->url(), {}, {}, transport);
            check(setup, "SETUP");
            const auto destination = destinationFrom(setup, speaker.address);
            if (const auto session = setup.headers.find("session"); session != setup.headers.end())
                client->setSession(session->second);
            const auto record = client->request("RECORD", client->url(), {}, {}, std::format("Range: npt=0-\r\n{}", rtpInfo()));
            check(record, "RECORD");
            uint32_t audioLatency = kDefaultAudioLatency;
            if (const auto latency = record.headers.find("audio-latency"); latency != record.headers.end())
                std::from_chars(latency->second.data(), latency->second.data() + latency->second.size(), audioLatency);
            return {std::move(client), destination, audioLatency};
        } catch (const Error&) {
            throw;
        } catch (const std::exception& e) {
            log::info("AirPlay receiver %s: %s", name.c_str(), e.what());
            throw Error(ErrorCode::DeviceNotFound, kNotConnected);
        }
    }

    static Destination destinationFrom(const RtspReply& setup, const std::string& speaker) {
        const auto transport = setup.headers.find("transport");
        if (transport == setup.headers.end()) throw Error(ErrorCode::DriverError, "the receiver gave no ports");
        const uint16_t server = portIn(transport->second, "server_port=");
        const uint16_t control = portIn(transport->second, "control_port=");
        if (!server) throw Error(ErrorCode::DriverError, "the receiver gave no ports");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        inet_pton(AF_INET, speaker.c_str(), &address.sin_addr);
        Destination destination{address, address};
        destination.audio.sin_port = htons(server);
        destination.control.sin_port = htons(control ? control : server);
        return destination;
    }

    std::string rtpInfo() {
        std::scoped_lock lock(mutex_);
        return std::format("RTP-Info: seq={};rtptime={}\r\n", sequence_, rtpNext_);
    }

    void flush() {
        std::scoped_lock lock(rtspMutex_);
        if (members_.empty() || !sentSinceFlush_.exchange(false)) return;
        for (const auto& member : members_) {
            try {
                for (const auto& client : member.sessions)
                    client->request("FLUSH", client->url(), {}, {}, std::format("Range: npt=0-\r\n{}", rtpInfo()));
            } catch (const std::exception& e) {
                log::info("AirPlay receiver %s: %s", member.name.c_str(), e.what()); // the session thread finds out why
            }
        }
    }

    static void teardown(const Member& member) {
        for (const auto& client : member.sessions) {
            try {
                client->request("TEARDOWN", client->url());
            } catch (const std::exception& e) {
                log::info("AirPlay receiver %s: %s", member.name.c_str(), e.what());
            }
        }
    }

    _Requires_lock_held_(rtspMutex_) void endSession() {
        for (const auto& member : members_) teardown(member);
        members_.clear();
        publishTargets();
    }

    // A receiver lost leaves the group; with the last one the stream ends with its reason.
    _Requires_lock_held_(rtspMutex_) void lose(const std::string& id, ErrorCode code, const char* why) {
        std::erase_if(members_, [&](const Member& m) { return m.id == id; });
        publishTargets();
        if (members_.empty()) {
            if (opened_) raiseError(Error(code, why));
        } else {
            tellLeft(id, Error(code, why));
        }
    }

    static void tellVolume(Member& member, const std::optional<double>& wanted) {
        if (!wanted || wanted == member.volumeSent) return;
        for (const auto& client : member.sessions)
            client->request("SET_PARAMETER", client->url(), "text/parameters", std::format("volume: {:.6f}\r\n", *wanted));
        member.volumeSent = wanted;
    }

    static void tellTrack(const Member& member, const TrackNotice& notice, const TrackDetails& details) {
        const auto at = std::format("RTP-Info: rtptime={}\r\n", notice.rtpTime);
        for (const auto& client : member.sessions) {
            const std::string url = client->url();
            client->request("SET_PARAMETER", url, "application/x-dmap-tagged",
                            dmapTags(details.title, details.artist, details.album), at);
            if (!details.coverJpeg.empty()) client->request("SET_PARAMETER", url, "image/jpeg", details.coverJpeg, at);
            if (notice.track.frames) {
                const uint32_t start = notice.rtpTime - uint32_t(notice.track.startFrame);
                const uint32_t end = notice.rtpTime + uint32_t(notice.track.frames);
                client->request("SET_PARAMETER", url, "text/parameters",
                                std::format("progress: {}/{}/{}\r\n", start, notice.rtpTime, end));
            }
        }
    }

    // The receivers the Server wants: those left go, those added join the stream, channels follow.
    _Requires_lock_held_(rtspMutex_) void followMembers(const std::vector<AirPlayMember>& wanted) {
        bool changed = false;
        for (auto it = members_.begin(); it != members_.end();) {
            if (std::ranges::none_of(wanted, [&](const AirPlayMember& w) { return w.id == it->id; })) {
                teardown(*it);
                it = members_.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
        for (const auto& w : wanted) {
            const auto member = std::ranges::find_if(members_, [&](const Member& m) { return m.id == w.id; });
            if (member != members_.end()) {
                const auto channels = member->destinations.size() > 1 ? AirPlayChannels::Stereo : w.channels;
                changed |= std::exchange(member->channels, channels) != channels;
                continue;
            }
            if (!w.receiver || !opened_) continue;
            try {
                members_.push_back(connectMember(w));
                changed = true;
            } catch (const Error& e) {
                tellLeft(w.id, e);
            }
        }
        if (changed) publishTargets();
    }

    // A connection the receiver closes means another sender took it; one that stops answering means it has gone.
    void tendSession(const std::optional<TrackNotice>& notice, const TrackDetails& details) {
        std::scoped_lock lock(rtspMutex_);
        if (members_.empty()) return;
        const auto wanted = options_.members();
        followMembers(wanted);
        const auto now = Clock::now();
        const bool heartbeat = now >= nextHeartbeat_;
        if (heartbeat) nextHeartbeat_ = now + options_.heartbeat;
        std::vector<std::pair<std::string, ErrorCode>> lost;
        for (auto& member : members_) {
            try {
                for (const auto& client : member.sessions)
                    if (client->closed()) throw RtspClosed();
                const auto w = std::ranges::find_if(wanted, [&](const AirPlayMember& m) { return m.id == member.id; });
                if (w != wanted.end()) tellVolume(member, w->volumeDb);
                if (notice) tellTrack(member, *notice, details);
                if (heartbeat)
                    for (const auto& client : member.sessions) client->request("OPTIONS", "*");
            } catch (const RtspClosed&) {
                lost.emplace_back(member.id, ErrorCode::DeviceInUse);
            } catch (const std::exception& e) {
                log::info("AirPlay receiver %s: %s", member.name.c_str(), e.what());
                lost.emplace_back(member.id, ErrorCode::DeviceLost);
            }
        }
        for (const auto& [id, code] : lost) lose(id, code, code == ErrorCode::DeviceInUse ? kInUse : kNotConnected);
        if (!members_.empty() && now >= closeAt_.load()) endSession();
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
        // The speaker asking, by its control port, else by its address alone.
        auto asking = std::ranges::find_if(targets_, [&](const Target& t) { return sameAddress(t.destination.control, from); });
        if (asking == targets_.end())
            asking = std::ranges::find_if(
                targets_, [&](const Target& t) { return t.destination.control.sin_addr.s_addr == from.sin_addr.s_addr; });
        const AirPlayChannels channels = asking == targets_.end() ? AirPlayChannels::Stereo : asking->channels;
        for (uint16_t i = 0; i < request->count; ++i) {
            const auto sequence = uint16_t(request->first + i);
            const auto found = std::ranges::find_if(history_, [&](const Packet& p) { return p.sequence == sequence; });
            if (found == history_.end()) continue;
            const auto packet = resendPacket(sequence, rtpPacket(sequence, found->rtpTime, ssrc_, false, found->frame(channels)));
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
        }
    }

    // Apart from the UDP ports: a request here can take seconds, and a HomePod answers SETUP only after its timing requests.
    void sessionLoop() {
        while (!stopping_) {
            std::this_thread::sleep_for(kPollSockets);
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
        const uint64_t ntp = ntpNow();
        for (auto& target : targets_) {
            const auto packet = syncPacket(std::exchange(target.firstSync, false), head - kSyncLatency, ntp, head);
            sendto(control_, packet.data(), int(packet.size()), 0, reinterpret_cast<const sockaddr*>(&target.destination.control),
                   sizeof(target.destination.control));
        }
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
        history_.push_back({endPacket_++, 0, 0, std::string(pcm_.begin(), pcm_.end())});
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
        for (auto& target : targets_)
            sendTo(target.destination.audio, rtpPacket(packet.sequence, packet.rtpTime, ssrc_,
                                                       std::exchange(target.firstPacket, false), packet.frame(target.channels)));
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

    AirPlayOutputOptions options_;
    SOCKET audio_ = INVALID_SOCKET;
    SOCKET control_ = INVALID_SOCKET;
    SOCKET timing_ = INVALID_SOCKET;
    uint16_t audioPort_ = 0, controlPort_ = 0, timingPort_ = 0;
    std::chrono::system_clock::time_point systemStart_;
    Clock::time_point steadyStart_;
    std::thread service_, session_, sender_;
    std::atomic<bool> stopping_ = false, opened_ = false, sentSinceFlush_ = false;

    std::mutex rtspMutex_; // taken before `mutex_`
    _Guarded_by_(rtspMutex_) std::vector<Member> members_;
    _Guarded_by_(rtspMutex_) Clock::time_point nextHeartbeat_;
    std::atomic<Clock::time_point> closeAt_ = Clock::time_point::max();

    mutable std::mutex mutex_;
    std::condition_variable moved_;
    _Guarded_by_(mutex_) IRenderSource* source_ = nullptr;
    _Guarded_by_(mutex_) std::vector<Target> targets_;
    _Guarded_by_(mutex_) std::string names_; // the members', for the status
    _Guarded_by_(mutex_) uint32_t audioLatency_ = kDefaultAudioLatency;
    _Guarded_by_(mutex_) bool running_ = false;
    _Guarded_by_(mutex_) bool parked_ = false;
    _Guarded_by_(mutex_) bool closing_ = false;
    _Guarded_by_(mutex_) bool ended_ = false;
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
    options.members = [receiver = std::move(receiver), volumeDb = options.volumeDb] {
        return std::vector<AirPlayMember>{{receiver.id, receiver, AirPlayChannels::Stereo, volumeDb ? volumeDb() : std::nullopt}};
    };
    return std::make_unique<AirPlayOutput>(std::move(options));
}

std::unique_ptr<audio::IOutput> createAirPlayGroupOutput(AirPlayOutputOptions options) {
    return std::make_unique<AirPlayOutput>(std::move(options));
}

} // namespace camel::server
