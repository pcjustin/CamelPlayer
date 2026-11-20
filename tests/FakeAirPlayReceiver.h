// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// AirPlay receivers on 127.0.0.1: one mDNS responder answering for all of them, and for each an RTSP port that answers
// what a test says and records what it was asked, with UDP ports that keep what arrives (docs/Plan.md §5.10).
#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <chrono>
#include <thread>
#include <utility>
#include <vector>

#include "Server/Network/Pairing.h"
#include "Server/Network/Plist.h"

namespace testutil {

struct FakeRtp {
    uint16_t sequence = 0;
    uint32_t rtpTime = 0;
    bool marker = false;
    std::string alac;
    std::chrono::steady_clock::time_point arrived;
};

// The frames of an uncompressed ALAC frame, read bit by bit without the sender's code. Empty when it is not one.
inline std::string pcmOfAlac(const std::string& alac) {
    size_t bit = 0;
    const auto read = [&](int count) {
        uint32_t value = 0;
        for (int i = 0; i < count; ++i, ++bit) value = value << 1 | ((uint8_t(alac[bit / 8]) >> (7 - bit % 8)) & 1);
        return value;
    };
    if (alac.size() != 1412) return {};
    if (read(3) != 1 || read(4) != 0 || read(12) != 0 || read(1) != 0 || read(2) != 0 || read(1) != 1) return {};
    std::string pcm;
    for (int i = 0; i < 704; ++i) {
        const uint32_t sample = read(16);
        pcm.push_back(char(sample & 0xFF));
        pcm.push_back(char(sample >> 8));
    }
    return read(3) == 7 ? pcm : std::string();
}

class FakeRtsp {
public:
    FakeRtsp() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address = loopback();
        ::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        listen(listener_, 8);
        int length = sizeof(address);
        getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        audio_ = boundUdp(audioPort_);
        control_ = boundUdp(controlPort_);
        timing_ = boundUdp(timingPort_);
        thread_ = std::thread([this] { serve(); });
        audioThread_ = std::thread([this] { receiveAudio(); });
        controlThread_ = std::thread([this] { receiveInto(control_, controlPackets_); });
        timingThread_ = std::thread([this] { receiveInto(timing_, timingReplies_); });
    }
    ~FakeRtsp() {
        closesocket(listener_);
        takeOver();
        closesocket(audio_);
        closesocket(control_);
        closesocket(timing_);
        thread_.join();
        audioThread_.join();
        controlThread_.join();
        timingThread_.join();
        if (events_ != INVALID_SOCKET) closesocket(events_);
        for (const SOCKET client : eventClients_) closesocket(client);
        WSACleanup();
    }
    FakeRtsp(const FakeRtsp&) = delete;
    FakeRtsp& operator=(const FakeRtsp&) = delete;

    uint16_t port() const { return port_; }
    void answer(const std::string& method, int status) {
        std::scoped_lock lock(mutex_);
        statuses_[method] = status;
    }
    std::vector<std::string> requests() const {
        std::scoped_lock lock(mutex_);
        return requests_;
    }
    std::vector<std::string> methods() const {
        std::vector<std::string> names;
        for (const auto& request : requests()) names.push_back(request.substr(0, request.find(' ')));
        return names;
    }
    std::vector<std::string> requestsOf(const std::string& method) const {
        std::vector<std::string> found;
        for (const auto& request : requests())
            if (request.starts_with(method + " ")) found.push_back(request);
        return found;
    }
    int count(const std::string& method) const { return int(requestsOf(method).size()); }

    std::vector<FakeRtp> packets() const {
        std::scoped_lock lock(mutex_);
        return packets_;
    }
    size_t packetCount() const {
        std::scoped_lock lock(mutex_);
        return packets_.size();
    }
    std::vector<std::string> controlPackets() const {
        std::scoped_lock lock(mutex_);
        return controlPackets_;
    }
    std::vector<std::string> timingReplies() const {
        std::scoped_lock lock(mutex_);
        return timingReplies_;
    }

    // The plists of the AirPlay 2 SETUPs it was sent, the session's and the stream's.
    std::vector<camel::server::airplay::Plist> setups() const {
        std::scoped_lock lock(mutex_);
        return setups_;
    }
    // AirPlay 2 packets opened with the stream's key, and those that did not open.
    int sealedPackets() const {
        std::scoped_lock lock(mutex_);
        return sealed_;
    }
    int unopenedPackets() const {
        std::scoped_lock lock(mutex_);
        return unopened_;
    }
    bool eventChannelOpened() const { return eventsAccepted_ > 0; }
    // Asks the sender on the event channel, as a receiver's remote does: `POST /command` with `sendMediaRemoteCommand` and
    // `value`, and `volume` and `isMuted` when `volume` is given. False before the sender has connected.
    bool sendCommand(const std::string& value, std::optional<double> volume = {}, bool muted = false) {
        using camel::server::airplay::Plist;
        Plist command = Plist::dict();
        command.set("type", Plist::of("sendMediaRemoteCommand")).set("value", Plist::of(std::string_view(value)));
        if (volume) command.set("volume", Plist::of(*volume)).set("isMuted", Plist::of(muted));
        const std::string body = camel::server::airplay::toBinaryPlist(command);
        std::scoped_lock lock(mutex_);
        if (eventClients_.empty() || sessionKey_.empty()) return false;
        if (!events_channel_)
            events_channel_.emplace(
                camel::server::airplay::hkdfSha512(sessionKey_, "Events-Salt", "Events-Write-Encryption-Key", 32),
                camel::server::airplay::hkdfSha512(sessionKey_, "Events-Salt", "Events-Read-Encryption-Key", 32));
        const std::string request =
            "POST /command RTSP/1.0\r\nCSeq: " + std::to_string(++eventSequence_) +
            "\r\nContent-Type: application/x-apple-binary-plist\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" +
            body;
        const std::string sealed = events_channel_->seal(request);
        return send(eventClients_.back(), sealed.data(), int(sealed.size()), 0) == int(sealed.size());
    }
    // Sends a reply on the event channel, which the sender does not answer. False before a command opened the channel.
    bool sendEventReply() {
        std::scoped_lock lock(mutex_);
        if (eventClients_.empty() || !events_channel_) return false;
        const std::string sealed = events_channel_->seal("RTSP/1.0 200 OK\r\nCSeq: 1\r\nContent-Length: 0\r\n\r\n");
        return send(eventClients_.back(), sealed.data(), int(sealed.size()), 0) == int(sealed.size());
    }
    // The sender's answers on the event channel that said 200.
    int eventAnswers() {
        std::scoped_lock lock(mutex_);
        if (eventClients_.empty() || !events_channel_) return 0;
        for (;;) {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(eventClients_.back(), &readable);
            const timeval now{0, 0};
            if (select(0, &readable, nullptr, nullptr, &now) <= 0) break;
            char buffer[2048];
            const int got = recv(eventClients_.back(), buffer, sizeof(buffer), 0);
            if (got <= 0) break;
            eventsSealed_.append(buffer, size_t(got));
            if (const auto opened = events_channel_->open(eventsSealed_)) eventsPlain_ += *opened;
        }
        int answers = 0;
        for (size_t at = 0; (at = eventsPlain_.find("RTSP/1.0 200 OK", at)) != std::string::npos; at += 15) ++answers;
        return answers;
    }
    // A sealed packet's payload, as a resend carries one; empty when it does not open.
    std::string openSealed(std::string_view packet) {
        std::scoped_lock lock(mutex_);
        return opened(packet);
    }

    void askTiming(const std::string& request) const {
        sockaddr_in target = loopback();
        target.sin_port = htons(senderTiming_ ? senderTiming_.load() : portAfter("timing_port="));
        sendto(timing_, request.data(), int(request.size()), 0, reinterpret_cast<const sockaddr*>(&target), sizeof(target));
    }
    void askResend(uint16_t sequence, uint16_t count) const {
        std::string request("\x80\xD5\x00\x01", 4);
        request += {char(sequence >> 8), char(sequence & 0xFF), char(count >> 8), char(count & 0xFF)};
        sockaddr_in target = loopback();
        target.sin_port = htons(senderControl_ ? senderControl_.load() : portAfter("control_port="));
        sendto(control_, request.data(), int(request.size()), 0, reinterpret_cast<const sockaddr*>(&target), sizeof(target));
    }

    // As another machine on the network would, from `address`: the packets that came back to it within `wait`.
    size_t askResendFrom(const char* address, uint16_t sequence, uint16_t count, std::chrono::milliseconds wait) const {
        const SOCKET udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in from{};
        from.sin_family = AF_INET;
        inet_pton(AF_INET, address, &from.sin_addr);
        ::bind(udp, reinterpret_cast<sockaddr*>(&from), sizeof(from));
        const DWORD timeout = 50;
        setsockopt(udp, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        std::string request("\x80\xD5\x00\x01", 4);
        request += {char(sequence >> 8), char(sequence & 0xFF), char(count >> 8), char(count & 0xFF)};
        sockaddr_in target = loopback();
        target.sin_port = htons(portAfter("control_port="));
        sendto(udp, request.data(), int(request.size()), 0, reinterpret_cast<const sockaddr*>(&target), sizeof(target));
        size_t answers = 0;
        char buffer[2048];
        const auto until = std::chrono::steady_clock::now() + wait;
        while (std::chrono::steady_clock::now() < until)
            if (recv(udp, buffer, sizeof(buffer), 0) > 0) ++answers;
        closesocket(udp);
        return answers;
    }

    // Another sender takes the receiver, which closes the connection.
    void takeOver() {
        const SOCKET client = client_.exchange(INVALID_SOCKET);
        if (client != INVALID_SOCKET) {
            shutdown(client, SD_BOTH);
            closesocket(client);
        }
    }
    void stopAnswering() { silent_ = true; }
    // Closes a connection made while it serves another, as the owner's DMP-A6 closed a session's while it served the
    // finder's check (2026-11-18).
    void dropOtherConnections() { dropOthers_ = true; }
    int droppedConnections() const { return dropped_; }
    // Holds the answer to ANNOUNCE, AirPlay 2's session SETUP and the request to show a code for `wait`, as a Mac does
    // until its user accepts the sender in a notification.
    void askUser(std::chrono::milliseconds wait) { askUserMs_ = int(wait.count()); }
    // Answers SETUP only once the sender has answered a timing request, as a HomePod does.
    void timeBeforeSetup() { timeBeforeSetup_ = true; }
    // Refuses every request until the sender has paired with `pin`; from then on the connection is encrypted.
    void demandPairing(std::string pin = "3939") {
        std::scoped_lock lock(mutex_);
        pin_ = std::move(pin);
        demandPairing_ = true;
    }
    // Takes only pair-setup with a code, which leaves keys for pair-verify, as a receiver that shows a code does; transient
    // pair-setup fails as with a wrong code.
    void pairOnlyWithKeys() {
        std::scoped_lock lock(mutex_);
        keysOnly_ = true;
    }
    // Forgets every sender paired with it, as a receiver reset does.
    void forgetPairings() {
        std::scoped_lock lock(mutex_);
        controllers_.clear();
    }
    // The requests that arrived encrypted.
    int encryptedRequests() const {
        std::scoped_lock lock(mutex_);
        return encryptedRequests_;
    }
    // The senders paired with keys, by pairing ID, with their public keys.
    std::map<std::string, std::string> controllers() const {
        std::scoped_lock lock(mutex_);
        return controllers_;
    }
    int pinsShown() const {
        std::scoped_lock lock(mutex_);
        return pinsShown_;
    }

private:
    static sockaddr_in loopback() {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return address;
    }

    static SOCKET boundUdp(uint16_t& port) {
        const SOCKET udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in address = loopback();
        ::bind(udp, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        int length = sizeof(address);
        getsockname(udp, reinterpret_cast<sockaddr*>(&address), &length);
        port = ntohs(address.sin_port);
        return udp;
    }

    uint16_t portAfter(const std::string& name) const {
        std::scoped_lock lock(mutex_);
        for (const auto& request : requests_)
            if (const size_t at = request.find(name); request.starts_with("SETUP") && at != std::string::npos)
                return uint16_t(std::stoi(request.substr(at + name.size())));
        return 0;
    }

    void receiveAudio() {
        char buffer[2048];
        for (;;) {
            const int got = recv(audio_, buffer, sizeof(buffer), 0);
            if (got <= 0) return;
            const auto byte = [&](int at) { return uint32_t(uint8_t(buffer[at])); };
            FakeRtp packet;
            packet.marker = (byte(1) & 0x80) != 0;
            packet.sequence = uint16_t(byte(2) << 8 | byte(3));
            packet.rtpTime = byte(4) << 24 | byte(5) << 16 | byte(6) << 8 | byte(7);
            packet.alac.assign(buffer + 12, size_t(got) - 12);
            packet.arrived = std::chrono::steady_clock::now();
            std::scoped_lock lock(mutex_);
            if (!shk_.empty()) packet.alac = opened(std::string_view(buffer, size_t(got)));
            packets_.push_back(std::move(packet));
        }
    }

    // An AirPlay 2 packet's payload, opened with the stream's key: the header's timestamp and SSRC as AAD, the nonce last.
    std::string opened(std::string_view packet) {
        if (packet.size() < 12 + 16 + 8) {
            ++unopened_;
            return {};
        }
        const auto nonce = std::string(4, '\0') + std::string(packet.substr(packet.size() - 8));
        const auto plain = camel::server::airplay::open(shk_, nonce, packet.substr(4, 8), packet.substr(12, packet.size() - 20));
        ++(plain ? sealed_ : unopened_);
        return plain.value_or(std::string());
    }

    // The session's SETUP opens the event channel; the stream's gives the ports and keeps its key.
    std::string answerSetup(const std::string& body) {
        using camel::server::airplay::Plist;
        const auto setup = camel::server::airplay::parseBinaryPlist(body);
        Plist answer = Plist::dict();
        if (!setup) return {};
        std::scoped_lock lock(mutex_);
        setups_.push_back(*setup);
        if (const Plist* streams = setup->find("streams"); streams && !streams->items.empty()) {
            const Plist& stream = streams->items.front();
            if (const Plist* key = stream.find("shk")) shk_ = key->text;
            if (const Plist* port = stream.find("controlPort")) senderControl_ = uint16_t(port->integer);
            Plist ports = Plist::dict();
            ports.set("type", Plist::of(96)).set("dataPort", Plist::of(int64_t{audioPort_}));
            ports.set("controlPort", Plist::of(int64_t{controlPort_}));
            answer.set("streams", Plist::array({ports}));
        } else {
            if (const Plist* port = setup->find("timingPort")) senderTiming_ = uint16_t(port->integer);
            if (events_ == INVALID_SOCKET) {
                events_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                sockaddr_in address = loopback();
                ::bind(events_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
                listen(events_, 4);
                int length = sizeof(address);
                getsockname(events_, reinterpret_cast<sockaddr*>(&address), &length);
                eventsPort_ = ntohs(address.sin_port);
            }
            answer.set("eventPort", Plist::of(int64_t{eventsPort_})).set("timingPort", Plist::of(0));
        }
        return camel::server::airplay::toBinaryPlist(answer);
    }

    // Whether the sender connected to the event channel, without taking the connection.
    void watchEvents() {
        if (events_ == INVALID_SOCKET) return;
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(events_, &readable);
        const timeval now{0, 0};
        if (select(0, &readable, nullptr, nullptr, &now) > 0) {
            const SOCKET accepted = accept(events_, nullptr, nullptr);
            if (accepted != INVALID_SOCKET) {
                ++eventsAccepted_;
                std::scoped_lock lock(mutex_);
                eventClients_.push_back(accepted);
            }
        }
    }

    void receiveInto(SOCKET udp, std::vector<std::string>& into) {
        char buffer[2048];
        for (;;) {
            const int got = recv(udp, buffer, sizeof(buffer), 0);
            if (got <= 0) return;
            std::scoped_lock lock(mutex_);
            into.emplace_back(buffer, size_t(got));
        }
    }

    // Closes a connection that waits while another is served, as the DMP-A6 does with a second session.
    void dropWaiting() {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listener_, &readable);
        const timeval now{0, 0};
        if (select(0, &readable, nullptr, nullptr, &now) <= 0) return;
        const SOCKET other = accept(listener_, nullptr, nullptr);
        if (other == INVALID_SOCKET) return;
        ++dropped_;
        closesocket(other);
    }

    void serve() {
        std::vector<char> buffer(65536);
        for (;;) {
            const SOCKET client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) return;
            client_ = client;
            std::string received, sealed;
            channel_.reset();
            for (;;) {
                if (dropOthers_) dropWaiting();
                const int got = recv(client, buffer.data(), int(buffer.size()), 0);
                if (got <= 0) break;
                if (channel_) {
                    sealed.append(buffer.data(), size_t(got));
                    const auto opened = channel_->open(sealed);
                    if (!opened) break;
                    received += *opened;
                } else {
                    received.append(buffer.data(), size_t(got));
                }
                while (answerOne(client, received)) {
                }
            }
            SOCKET expected = client;
            if (client_.compare_exchange_strong(expected, INVALID_SOCKET)) closesocket(client);
        }
    }

    bool timedSender() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // a network's delay
        std::string request("\x80\xD2\x00\x07", 4);
        request.resize(32, '\0');
        askTiming(request);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        while (timingReplies().empty() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return !timingReplies().empty();
    }

    bool answerOne(SOCKET client, std::string& received) {
        const size_t headEnd = received.find("\r\n\r\n");
        if (headEnd == std::string::npos) return false;
        size_t bodyBytes = 0;
        if (const size_t length = received.find("Content-Length: "); length != std::string::npos && length < headEnd)
            bodyBytes = size_t(std::stoi(received.substr(length + 16)));
        const size_t total = headEnd + 4 + bodyBytes;
        if (received.size() < total) return false;
        const std::string request = received.substr(0, total);
        received.erase(0, total);
        const std::string method = request.substr(0, request.find(' '));
        watchEvents();
        int status = 200;
        bool demanded = false;
        {
            std::scoped_lock lock(mutex_);
            requests_.push_back(request);
            if (channel_) ++encryptedRequests_;
            if (const auto chosen = statuses_.find(method); chosen != statuses_.end()) status = chosen->second;
            demanded = demandPairing_;
        }
        if (silent_ || (method == "SETUP" && timeBeforeSetup_ && !timedSender())) return true;
        const bool plist = request.find("Content-Type: application/x-apple-binary-plist\r\n") < headEnd;
        const bool sessionSetup = method == "SETUP" && plist && request.find("streams", headEnd) == std::string::npos;
        if ((method == "ANNOUNCE" || sessionSetup || request.starts_with("POST /pair-pin-start ")) && askUserMs_ > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(askUserMs_.load()));
        const size_t sequence = request.find("CSeq: ");
        std::string reply = "RTSP/1.0 " + std::to_string(status) +
                            " Reason\r\nCSeq: " + request.substr(sequence + 6, request.find("\r\n", sequence) - sequence - 6) +
                            "\r\nServer: Fake\r\n";
        if (request.starts_with("POST /pair-setup ")) {
            pairSetup(client, reply, request.substr(headEnd + 4));
            return true;
        }
        if (request.starts_with("POST /pair-verify ")) {
            pairVerify(client, reply, request.substr(headEnd + 4));
            return true;
        }
        if (request.starts_with("POST /pair-pin-start ")) {
            {
                std::scoped_lock lock(mutex_);
                ++pinsShown_;
            }
            sendOut(client, reply + "\r\n");
            return true;
        }
        if (demanded && !channel_) {
            reply.replace(9, 3, "403");
            reply += "\r\n";
            sendOut(client, reply);
            return true;
        }
        if (method == "SETUP" && plist) {
            const std::string answer = answerSetup(request.substr(headEnd + 4));
            sendOut(client, reply + "Content-Type: application/x-apple-binary-plist\r\nContent-Length: " +
                                std::to_string(answer.size()) + "\r\n\r\n" + answer);
            return true;
        }
        if (method == "SETUP")
            reply += "Session: 1\r\nTransport: RTP/AVP/UDP;unicast;mode=record;server_port=" + std::to_string(audioPort_) +
                     ";control_port=" + std::to_string(controlPort_) + ";timing_port=" + std::to_string(timingPort_) + "\r\n";
        if (method == "RECORD") reply += "Audio-Latency: 11025\r\n";
        reply += "\r\n";
        sendOut(client, reply);
        return true;
    }

    void sendOut(SOCKET client, const std::string& reply) {
        const std::string out = channel_ ? channel_->seal(reply) : reply;
        send(client, out.data(), int(out.size()), 0);
    }

    void sendTlv(SOCKET client, std::string reply, const std::string& answer) {
        reply +=
            "Content-Type: application/octet-stream\r\nContent-Length: " + std::to_string(answer.size()) + "\r\n\r\n" + answer;
        sendOut(client, reply);
    }

    static std::string nonceOf(const char* name) { return std::string(4, '\0') + name; }

    // The receiver's side of pair-setup, transient or with a code, with the code it was given.
    void pairSetup(SOCKET client, const std::string& reply, const std::string& body) {
        namespace ap = camel::server::airplay;
        auto items = ap::parseTlv(body);
        std::string pin;
        bool keysOnly = false;
        {
            std::scoped_lock lock(mutex_);
            pin = pin_;
            keysOnly = keysOnly_;
        }
        if (items[6] == "\x01") {
            transient_ = items.contains(19);
            salt_ = ap::randomBytes(16);
            secret_ = ap::randomBytes(32);
            server_ = ap::srpServer("Pair-Setup", pin, salt_, secret_);
            sendTlv(client, reply, ap::tlv({{6, "\x02"}, {2, salt_}, {3, server_.publicKey}}));
            return;
        }
        if (items[6] == "\x03") {
            const auto checked = ap::srpServerCheck(server_, "Pair-Setup", salt_, items[3], items[4], secret_);
            if (!checked || (transient_ && keysOnly)) {
                sendTlv(client, reply, ap::tlv({{6, "\x04"}, {7, "\x02"}}));
                return;
            }
            sendTlv(client, reply, ap::tlv({{6, "\x04"}, {4, checked->second}}));
            if (transient_) {
                channel_ = ap::SecureChannel::forReceiver(checked->first);
                sessionKey_ = checked->first;
            } else
                setupKey_ = checked->first;
            return;
        }
        const std::string key = ap::hkdfSha512(setupKey_, "Pair-Setup-Encrypt-Salt", "Pair-Setup-Encrypt-Info", 32);
        const auto opened = ap::open(key, nonceOf("PS-Msg05"), {}, items[5]);
        auto theirs = ap::parseTlv(opened.value_or(""));
        const std::string signedInfo =
            ap::hkdfSha512(setupKey_, "Pair-Setup-Controller-Sign-Salt", "Pair-Setup-Controller-Sign-Info", 32) + theirs[1] +
            theirs[3];
        if (!opened || !ap::ed25519Verify(theirs[3], signedInfo, theirs[10])) {
            sendTlv(client, reply, ap::tlv({{6, "\x06"}, {7, "\x02"}}));
            return;
        }
        {
            std::scoped_lock lock(mutex_);
            controllers_[theirs[1]] = theirs[3];
        }
        const std::string publicKey = ap::ed25519PublicKey(accessorySeed_);
        const std::string mine =
            ap::hkdfSha512(setupKey_, "Pair-Setup-Accessory-Sign-Salt", "Pair-Setup-Accessory-Sign-Info", 32) + kAccessoryId +
            publicKey;
        const std::string sub = ap::tlv({{1, kAccessoryId}, {3, publicKey}, {10, ap::ed25519Sign(accessorySeed_, mine)}});
        sendTlv(client, reply, ap::tlv({{6, "\x06"}, {5, ap::seal(key, nonceOf("PS-Msg06"), {}, sub)}}));
    }

    // The receiver's side of pair-verify, for a sender paired with keys.
    void pairVerify(SOCKET client, const std::string& reply, const std::string& body) {
        namespace ap = camel::server::airplay;
        auto items = ap::parseTlv(body);
        if (items[6] == "\x01") {
            const std::string secret = ap::randomBytes(32);
            verifyMine_ = ap::x25519Base(secret);
            verifyTheirs_ = items[3];
            verifyShared_ = ap::x25519(secret, verifyTheirs_);
            const std::string key = ap::hkdfSha512(verifyShared_, "Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info", 32);
            const std::string sub =
                ap::tlv({{1, kAccessoryId}, {10, ap::ed25519Sign(accessorySeed_, verifyMine_ + kAccessoryId + verifyTheirs_)}});
            sendTlv(client, reply, ap::tlv({{6, "\x02"}, {3, verifyMine_}, {5, ap::seal(key, nonceOf("PV-Msg02"), {}, sub)}}));
            return;
        }
        const std::string key = ap::hkdfSha512(verifyShared_, "Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info", 32);
        const auto opened = ap::open(key, nonceOf("PV-Msg03"), {}, items[5]);
        auto theirs = ap::parseTlv(opened.value_or(""));
        std::string known;
        {
            std::scoped_lock lock(mutex_);
            if (const auto found = controllers_.find(theirs[1]); found != controllers_.end()) known = found->second;
        }
        if (!opened || known.empty() || !ap::ed25519Verify(known, verifyTheirs_ + theirs[1] + verifyMine_, theirs[10])) {
            sendTlv(client, reply, ap::tlv({{6, "\x04"}, {7, "\x02"}}));
            return;
        }
        sendTlv(client, reply, ap::tlv({{6, "\x04"}}));
        channel_ = ap::SecureChannel::forReceiver(verifyShared_);
        sessionKey_ = verifyShared_;
    }

    static constexpr const char* kAccessoryId = "FA:KE:00:00:00:01";

    SOCKET listener_ = INVALID_SOCKET;
    SOCKET audio_ = INVALID_SOCKET;
    SOCKET control_ = INVALID_SOCKET;
    SOCKET timing_ = INVALID_SOCKET;
    std::atomic<SOCKET> client_ = INVALID_SOCKET;
    std::atomic<bool> silent_ = false, timeBeforeSetup_ = false, dropOthers_ = false;
    std::atomic<int> askUserMs_ = 0, dropped_ = 0;
    std::vector<camel::server::airplay::Plist> setups_;
    std::string shk_;
    int sealed_ = 0, unopened_ = 0;
    std::atomic<uint16_t> senderControl_ = 0, senderTiming_ = 0;
    SOCKET events_ = INVALID_SOCKET;
    uint16_t eventsPort_ = 0;
    std::atomic<int> eventsAccepted_ = 0;
    std::vector<SOCKET> eventClients_;
    std::string sessionKey_; // the pairing's, which the event channel's keys derive from
    std::optional<camel::server::airplay::SecureChannel> events_channel_;
    std::string eventsSealed_, eventsPlain_;
    int eventSequence_ = 0;
    uint16_t port_ = 0, audioPort_ = 0, controlPort_ = 0, timingPort_ = 0;
    mutable std::mutex mutex_;
    std::map<std::string, int> statuses_;
    std::vector<std::string> requests_;
    std::string pin_ = "3939"; // the code transient pairing uses, which a receiver takes though it does not demand it
    bool demandPairing_ = false, keysOnly_ = false;
    int encryptedRequests_ = 0, pinsShown_ = 0;
    std::map<std::string, std::string> controllers_;
    // Only the serving thread.
    std::optional<camel::server::airplay::SecureChannel> channel_;
    std::string salt_, secret_, setupKey_, verifyMine_, verifyTheirs_, verifyShared_;
    bool transient_ = false;
    camel::server::airplay::SrpServer server_;
    std::string accessorySeed_ = camel::server::airplay::randomBytes(32);
    std::vector<FakeRtp> packets_;
    std::vector<std::string> controlPackets_, timingReplies_;
    std::thread thread_, audioThread_, controlThread_, timingThread_;
};

struct FakeReceiverRecord {
    std::string deviceId, name, host;
    uint16_t port = 0;
    std::string address = "127.0.0.1";
    std::vector<std::string> otherAddresses; // answered in turn with `address`, as a host on several networks does
    std::vector<std::string> raopTxt = {"am=AudioAccessory5,1", "cn=0,1", "et=0,1", "pw=false", "vs=105.1"};
    std::vector<std::string> airplayTxt; // empty for a receiver without _airplay._tcp
    bool terse = false;                  // answers a browse with the name alone
    bool raop = true;                    // false for one with _airplay._tcp alone, as an AirPlay 2 TV
};

class FakeMdns {
public:
    FakeMdns() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        udp_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(udp_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        int length = sizeof(address);
        getsockname(udp_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { serve(); });
    }
    // A responder in `group` on `port` over the loopback interface, as the Chord Poly: it answers only a query from the
    // group's port, and only to the group.
    FakeMdns(const std::string& group, uint16_t port) : group_(group), port_(port) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        udp_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        const BOOL reuse = TRUE;
        setsockopt(udp_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        ::bind(udp_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        ip_mreq membership{};
        inet_pton(AF_INET, group.c_str(), &membership.imr_multiaddr);
        membership.imr_interface.s_addr = htonl(INADDR_LOOPBACK);
        setsockopt(udp_, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&membership), sizeof(membership));
        in_addr loopback{};
        loopback.s_addr = htonl(INADDR_LOOPBACK);
        setsockopt(udp_, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&loopback), sizeof(loopback));
        thread_ = std::thread([this] { serve(); });
    }
    ~FakeMdns() {
        closesocket(udp_);
        thread_.join();
        WSACleanup();
    }
    FakeMdns(const FakeMdns&) = delete;
    FakeMdns& operator=(const FakeMdns&) = delete;

    uint16_t port() const { return port_; }
    void add(FakeReceiverRecord record) {
        std::scoped_lock lock(mutex_);
        records_.push_back(std::move(record));
    }
    void clear() {
        std::scoped_lock lock(mutex_);
        records_.clear();
    }
    void moveTo(const std::string& deviceId, const std::string& address) {
        std::scoped_lock lock(mutex_);
        for (auto& record : records_)
            if (record.deviceId == deviceId) record.address = address;
    }
    void announce(const std::string& deviceId, std::vector<std::string> airplayTxt) {
        std::scoped_lock lock(mutex_);
        for (auto& record : records_)
            if (record.deviceId == deviceId) record.airplayTxt = airplayTxt;
    }
    void setRaop(const std::string& deviceId, std::vector<std::string> raopTxt) {
        std::scoped_lock lock(mutex_);
        for (auto& record : records_)
            if (record.deviceId == deviceId) record.raopTxt = std::move(raopTxt);
    }
    void rename(const std::string& deviceId, const std::string& name) {
        std::scoped_lock lock(mutex_);
        for (auto& record : records_)
            if (record.deviceId == deviceId) record.name = name;
    }

private:
    using Records = std::vector<std::string>;

    static std::string name(const std::vector<std::string>& labels) {
        std::string out;
        for (const auto& label : labels) {
            out.push_back(char(label.size()));
            out += label;
        }
        out.push_back('\0');
        return out;
    }
    static std::string service(const std::string& type) { return name({type, "_tcp", "local"}); }
    static std::string instance(const std::string& label, const std::string& type) {
        return name({label, type, "_tcp", "local"});
    }
    static std::string u16(int value) { return {char(value >> 8), char(value & 0xFF)}; }
    static std::string record(const std::string& owner, int type, const std::string& data) {
        return owner + u16(type) + u16(1) + std::string(3, '\0') + char(120) + u16(int(data.size())) + data;
    }
    static std::string txt(const std::vector<std::string>& entries) {
        std::string out;
        for (const auto& entry : entries) {
            out.push_back(char(entry.size()));
            out += entry;
        }
        return out;
    }
    static std::string srv(uint16_t port, const std::string& host) { return u16(0) + u16(0) + u16(port) + name({host, "local"}); }
    static std::string addressBytes(const std::string& address) {
        in_addr parsed{};
        inet_pton(AF_INET, address.c_str(), &parsed);
        return std::string(reinterpret_cast<const char*>(&parsed), 4);
    }
    static void details(Records& out, const FakeReceiverRecord& r, size_t turn) {
        const std::string raop = r.deviceId + "@" + r.name;
        if (r.raop) {
            out.push_back(record(instance(raop, "_raop"), 33, srv(r.port, r.host)));
            out.push_back(record(instance(raop, "_raop"), 16, txt(r.raopTxt)));
        }
        if (!r.airplayTxt.empty()) {
            out.push_back(record(instance(r.name, "_airplay"), 33, srv(r.port, r.host)));
            out.push_back(record(instance(r.name, "_airplay"), 16, txt(r.airplayTxt)));
        }
        const size_t which = turn % (1 + r.otherAddresses.size());
        out.push_back(record(name({r.host, "local"}), 1, addressBytes(which ? r.otherAddresses[which - 1] : r.address)));
    }

    void serve() {
        for (;;) {
            char buffer[2048];
            sockaddr_in from{};
            int length = sizeof(from);
            const int got = recvfrom(udp_, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &length);
            if (got <= 0) return;
            const std::string query(buffer, size_t(got));
            Records answers;
            {
                std::scoped_lock lock(mutex_);
                const size_t turn = replies_++;
                for (const auto& r : records_) {
                    const std::string raop = r.deviceId + "@" + r.name;
                    if (r.raop) answers.push_back(record(service("_raop"), 12, instance(raop, "_raop")));
                    if (!r.airplayTxt.empty()) answers.push_back(record(service("_airplay"), 12, instance(r.name, "_airplay")));
                    if (!r.terse || query.find(raop) != std::string::npos) details(answers, r, turn);
                }
            }
            std::string reply = u16(0) + u16(0x8400) + u16(0) + u16(int(answers.size())) + u16(0) + u16(0);
            for (const auto& answer : answers) reply += answer;
            if (group_.empty()) {
                sendto(udp_, reply.data(), int(reply.size()), 0, reinterpret_cast<sockaddr*>(&from), sizeof(from));
                continue;
            }
            // Its own answers come back through the group; only a question is answered, and only from the group's port.
            if (ntohs(from.sin_port) != port_ || query.size() < 12 || (uint8_t(query[2]) & 0x80)) continue;
            sockaddr_in to{};
            to.sin_family = AF_INET;
            to.sin_port = htons(port_);
            inet_pton(AF_INET, group_.c_str(), &to.sin_addr);
            sendto(udp_, reply.data(), int(reply.size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
        }
    }

    SOCKET udp_ = INVALID_SOCKET;
    std::string group_; // empty: answers each query to its own port
    uint16_t port_ = 0;
    std::mutex mutex_;
    std::vector<FakeReceiverRecord> records_;
    size_t replies_ = 0;
    std::thread thread_;
};

} // namespace testutil
