// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Network/AirPlay2.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <stdexcept>

#include "Common/util/Log.h"
#include "Server/Network/AirPlay.h"
#include "Server/Network/Pairing.h"
#include "Server/Network/Rtsp.h"

namespace camel::server::airplay {

namespace {

constexpr int64_t kRealtimeStream = 96;
constexpr int64_t kAlac = 2;
constexpr int64_t kAlac44100x16x2 = 0x40000;
constexpr size_t kHeaderBytes = 12;
constexpr size_t kNonceBytes = 8;

// "02:A6:06:38:47:4C", the same six bytes as the device ID requests carry.
std::string senderAddress() {
    const std::string& id = senderDeviceId();
    std::string out;
    for (size_t i = 2; i + 1 < id.size(); i += 2) {
        if (!out.empty()) out += ':';
        out += char(std::toupper(static_cast<unsigned char>(id[i])));
        out += char(std::toupper(static_cast<unsigned char>(id[i + 1])));
    }
    return out;
}

std::string littleEndian64(uint64_t value) {
    std::string out;
    for (size_t i = 0; i < kNonceBytes; ++i) out.push_back(char((value >> (8 * i)) & 0xFF));
    return out;
}

} // namespace

std::string randomUuid() {
    const std::string r = randomBytes(16);
    std::string h;
    for (const char c : r) h += std::format("{:02X}", uint8_t(c));
    return std::format("{}-{}-{}-{}-{}", h.substr(0, 8), h.substr(8, 4), h.substr(12, 4), h.substr(16, 4), h.substr(20));
}

Plist sessionSetup(uint16_t timingPort, std::string_view groupUuid) {
    const std::string address = senderAddress();
    Plist setup = Plist::dict();
    setup.set("deviceID", Plist::of(std::string_view(address))).set("macAddress", Plist::of(std::string_view(address)));
    setup.set("sessionUUID", Plist::of(std::string_view(randomUuid()))).set("name", Plist::of("CamelPlayer"));
    setup.set("model", Plist::of("Windows")).set("osName", Plist::of("Windows")).set("osVersion", Plist::of("10.0"));
    setup.set("sourceVersion", Plist::of("999.0.0")).set("timingProtocol", Plist::of("NTP"));
    setup.set("timingPort", Plist::of(int64_t{timingPort})).set("isMultiSelectAirPlay", Plist::of(true));
    setup.set("groupContainsGroupLeader", Plist::of(false)).set("groupUUID", Plist::of(groupUuid));
    setup.set("senderSupportsRelay", Plist::of(false)).set("statsCollectionEnabled", Plist::of(false));
    return setup;
}

Plist streamSetup(std::string_view key, uint16_t controlPort, int64_t connectionId) {
    Plist stream = Plist::dict();
    stream.set("type", Plist::of(kRealtimeStream)).set("ct", Plist::of(kAlac)).set("audioFormat", Plist::of(kAlac44100x16x2));
    stream.set("spf", Plist::of(int64_t{kFramesPerPacket})).set("sr", Plist::of(int64_t{kSampleRate}));
    stream.set("shk", Plist::data(key)).set("controlPort", Plist::of(int64_t{controlPort}));
    stream.set("latencyMin", Plist::of(int64_t{kDefaultAudioLatency})).set("latencyMax", Plist::of(int64_t{88200}));
    stream.set("isMedia", Plist::of(true)).set("audioMode", Plist::of("default"));
    stream.set("streamConnectionID", Plist::of(connectionId)).set("supportsDynamicStreamID", Plist::of(false));
    Plist setup = Plist::dict();
    setup.set("streams", Plist::array({stream}));
    return setup;
}

namespace {

std::optional<uint16_t> portIn(const Plist& dict, std::string_view key) {
    const Plist* value = dict.find(key);
    if (!value || value->kind != Plist::Kind::Integer || value->integer <= 0 || value->integer > 0xFFFF) return std::nullopt;
    return uint16_t(value->integer);
}

} // namespace

std::optional<StreamPorts> streamPortsFrom(const Plist& answer) {
    const Plist* streams = answer.find("streams");
    if (!streams || streams->kind != Plist::Kind::Array || streams->items.empty()) return std::nullopt;
    const Plist& stream = streams->items.front();
    const auto data = portIn(stream, "dataPort");
    if (!data) return std::nullopt;
    return StreamPorts{*data, portIn(stream, "controlPort").value_or(*data)};
}

std::optional<uint16_t> eventPortFrom(const Plist& answer) {
    return portIn(answer, "eventPort");
}

std::optional<ReceiverEvent> receiverEventFrom(const Plist& command) {
    const Plist* type = command.find("type");
    const Plist* value = command.find("value");
    if (!type || type->text != "sendMediaRemoteCommand" || !value || value->kind != Plist::Kind::String) return std::nullopt;
    const std::string& code = value->text;
    if (code == "plps") return ReceiverEvent{ReceiverCommand::TogglePause};
    if (code == "pbpr" || code == "pbal") return ReceiverEvent{ReceiverCommand::Pause};
    if (code == "nitm") return ReceiverEvent{ReceiverCommand::Next};
    if (code == "pitm") return ReceiverEvent{ReceiverCommand::Previous};
    if (code != "dvlc") return std::nullopt;
    const Plist* volume = command.find("volume");
    const Plist* muted = command.find("isMuted");
    if (!volume || (volume->kind != Plist::Kind::Real && volume->kind != Plist::Kind::Integer)) return std::nullopt;
    const double level = volume->kind == Plist::Kind::Real ? volume->real : double(volume->integer);
    return ReceiverEvent{ReceiverCommand::Volume, std::clamp(level, 0.0, 1.0), muted && muted->boolean};
}

namespace {

constexpr size_t kMaxEventBytes = 65536;

std::string headerValue(std::string_view head, std::string_view name) {
    size_t at = 0;
    while ((at = head.find("\r\n", at)) != std::string_view::npos) {
        at += 2;
        const size_t end = std::min(head.find("\r\n", at), head.size());
        const std::string_view line = head.substr(at, end - at);
        if (line.size() > name.size() && line[name.size()] == ':' &&
            std::ranges::equal(line.substr(0, name.size()), name,
                               [](char a, char b) { return std::tolower(uint8_t(a)) == std::tolower(uint8_t(b)); })) {
            std::string_view value = line.substr(name.size() + 1);
            value.remove_prefix(std::min(value.find_first_not_of(' '), value.size()));
            return std::string(value);
        }
    }
    return {};
}

// The channel's socket does not block: what does not go at once waits until it can.
bool sendAll(SOCKET s, std::string_view data) {
    constexpr timeval kWait{1, 0};
    while (!data.empty()) {
        const int sent = send(s, data.data(), int(data.size()), 0);
        if (sent > 0) {
            data.remove_prefix(size_t(sent));
            continue;
        }
        if (WSAGetLastError() != WSAEWOULDBLOCK) return false;
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(s, &writable);
        if (select(0, nullptr, &writable, nullptr, &kWait) <= 0) return false;
    }
    return true;
}

} // namespace

EventChannel::EventChannel(const std::string& address, uint16_t port, std::string_view sessionKey, int timeoutMs)
    : channel_(hkdfSha512(sessionKey, "Events-Salt", "Events-Read-Encryption-Key", 32),
               hkdfSha512(sessionKey, "Events-Salt", "Events-Write-Encryption-Key", 32)) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    socket_ = s;
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(port);
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    bool connected =
        inet_pton(AF_INET, address.c_str(), &target.sin_addr) == 1 &&
        (connect(s, reinterpret_cast<const sockaddr*>(&target), sizeof(target)) == 0 || WSAGetLastError() == WSAEWOULDBLOCK);
    if (connected) {
        fd_set writable, failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(s, &writable);
        FD_SET(s, &failed);
        const timeval wait{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        connected = select(0, nullptr, &writable, &failed, &wait) > 0 && !FD_ISSET(s, &failed);
    }
    if (!connected) {
        closesocket(s);
        WSACleanup();
        throw std::runtime_error("the receiver's event channel does not accept a connection");
    }
}

EventChannel::~EventChannel() {
    closesocket(SOCKET(socket_));
    WSACleanup();
}

std::vector<ReceiverEvent> EventChannel::read() {
    const auto s = SOCKET(socket_);
    for (;;) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(s, &readable);
        const timeval now{0, 0};
        if (select(0, &readable, nullptr, nullptr, &now) <= 0) break;
        char buffer[4096];
        const int got = recv(s, buffer, int(sizeof(buffer)), 0);
        if (got <= 0) break;
        sealed_.append(buffer, size_t(got));
        if (sealed_.size() > kMaxEventBytes) sealed_.clear();
        if (const auto opened = channel_.open(sealed_)) {
            plain_ += *opened;
        } else {
            sealed_.clear(); // a damaged block: what follows cannot be opened either
            if (!damaged_) log::warn("AirPlay receiver: its event channel cannot be read, so its remote is followed no more");
            damaged_ = true;
        }
    }
    std::vector<ReceiverEvent> events;
    for (;;) {
        const size_t headEnd = plain_.find("\r\n\r\n");
        if (headEnd == std::string::npos) {
            if (plain_.size() > kMaxEventBytes) plain_.clear();
            break;
        }
        const std::string_view head(plain_.data(), headEnd);
        const std::string length = headerValue(head, "Content-Length");
        size_t bodyBytes = 0;
        std::from_chars(length.data(), length.data() + length.size(), bodyBytes);
        if (bodyBytes > kMaxEventBytes) {
            plain_.clear();
            break;
        }
        if (plain_.size() < headEnd + 4 + bodyBytes) break;
        // Only a request is answered, and only what is sent is sealed: each seal takes the next nonce.
        if (!head.starts_with("RTSP/")) {
            const std::string answer =
                std::format("RTSP/1.0 200 OK\r\nCSeq: {}\r\nContent-Length: 0\r\n\r\n", headerValue(head, "CSeq"));
            if (!sendAll(s, channel_.seal(answer))) log::info("AirPlay receiver: an answer on its event channel was not sent");
        }
        if (head.starts_with("POST /command ")) {
            const auto command = parseBinaryPlist(std::string_view(plain_).substr(headEnd + 4, bodyBytes));
            if (const auto event = command ? receiverEventFrom(*command) : std::nullopt) events.push_back(*event);
        }
        plain_.erase(0, headEnd + 4 + bodyBytes);
    }
    return events;
}

std::string sealedRtpPacket(std::string_view rtp, std::string_view key, uint64_t nonce) {
    const std::string tail = littleEndian64(nonce);
    const std::string sealed =
        seal(key, std::string(4, '\0') + tail, rtp.substr(4, kHeaderBytes - 4), rtp.substr(std::min(rtp.size(), kHeaderBytes)));
    return std::string(rtp.substr(0, kHeaderBytes)) + sealed + tail;
}

} // namespace camel::server::airplay
