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
#include <string>
#include <chrono>
#include <thread>
#include <utility>
#include <vector>

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

    void askTiming(const std::string& request) const {
        sockaddr_in target = loopback();
        target.sin_port = htons(portAfter("timing_port="));
        sendto(timing_, request.data(), int(request.size()), 0, reinterpret_cast<const sockaddr*>(&target), sizeof(target));
    }
    void askResend(uint16_t sequence, uint16_t count) const {
        std::string request("\x80\xD5\x00\x01", 4);
        request += {char(sequence >> 8), char(sequence & 0xFF), char(count >> 8), char(count & 0xFF)};
        sockaddr_in target = loopback();
        target.sin_port = htons(portAfter("control_port="));
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
    // Answers SETUP only once the sender has answered a timing request, as a HomePod does.
    void timeBeforeSetup() { timeBeforeSetup_ = true; }

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
            packets_.push_back(std::move(packet));
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

    void serve() {
        std::vector<char> buffer(65536);
        for (;;) {
            const SOCKET client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) return;
            client_ = client;
            std::string received;
            for (;;) {
                const int got = recv(client, buffer.data(), int(buffer.size()), 0);
                if (got <= 0) break;
                received.append(buffer.data(), size_t(got));
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
        int status = 200;
        {
            std::scoped_lock lock(mutex_);
            requests_.push_back(request);
            if (const auto chosen = statuses_.find(method); chosen != statuses_.end()) status = chosen->second;
        }
        if (silent_ || (method == "SETUP" && timeBeforeSetup_ && !timedSender())) return true;
        const size_t sequence = request.find("CSeq: ");
        std::string reply = "RTSP/1.0 " + std::to_string(status) +
                            " Reason\r\nCSeq: " + request.substr(sequence + 6, request.find("\r\n", sequence) - sequence - 6) +
                            "\r\nServer: Fake\r\n";
        if (method == "SETUP")
            reply += "Session: 1\r\nTransport: RTP/AVP/UDP;unicast;mode=record;server_port=" + std::to_string(audioPort_) +
                     ";control_port=" + std::to_string(controlPort_) + ";timing_port=" + std::to_string(timingPort_) + "\r\n";
        if (method == "RECORD") reply += "Audio-Latency: 11025\r\n";
        reply += "\r\n";
        send(client, reply.data(), int(reply.size()), 0);
        return true;
    }

    SOCKET listener_ = INVALID_SOCKET;
    SOCKET audio_ = INVALID_SOCKET;
    SOCKET control_ = INVALID_SOCKET;
    SOCKET timing_ = INVALID_SOCKET;
    std::atomic<SOCKET> client_ = INVALID_SOCKET;
    std::atomic<bool> silent_ = false, timeBeforeSetup_ = false;
    uint16_t port_ = 0, audioPort_ = 0, controlPort_ = 0, timingPort_ = 0;
    mutable std::mutex mutex_;
    std::map<std::string, int> statuses_;
    std::vector<std::string> requests_;
    std::vector<FakeRtp> packets_;
    std::vector<std::string> controlPackets_, timingReplies_;
    std::thread thread_, audioThread_, controlThread_, timingThread_;
};

struct FakeReceiverRecord {
    std::string deviceId, name, host;
    uint16_t port = 0;
    std::string address = "127.0.0.1";
    std::vector<std::string> raopTxt = {"am=AudioAccessory5,1", "cn=0,1", "et=0,1", "pw=false", "vs=105.1"};
    std::vector<std::string> airplayTxt; // empty for a receiver without _airplay._tcp
    bool terse = false;                  // answers a browse with the name alone
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
    static void details(Records& out, const FakeReceiverRecord& r) {
        const std::string raop = r.deviceId + "@" + r.name;
        out.push_back(record(instance(raop, "_raop"), 33, srv(r.port, r.host)));
        out.push_back(record(instance(raop, "_raop"), 16, txt(r.raopTxt)));
        if (!r.airplayTxt.empty()) {
            out.push_back(record(instance(r.name, "_airplay"), 33, srv(r.port, r.host)));
            out.push_back(record(instance(r.name, "_airplay"), 16, txt(r.airplayTxt)));
        }
        out.push_back(record(name({r.host, "local"}), 1, addressBytes(r.address)));
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
                for (const auto& r : records_) {
                    const std::string raop = r.deviceId + "@" + r.name;
                    answers.push_back(record(service("_raop"), 12, instance(raop, "_raop")));
                    if (!r.airplayTxt.empty()) answers.push_back(record(service("_airplay"), 12, instance(r.name, "_airplay")));
                    if (!r.terse || query.find(raop) != std::string::npos) details(answers, r);
                }
            }
            std::string reply = u16(0) + u16(0x8400) + u16(0) + u16(int(answers.size())) + u16(0) + u16(0);
            for (const auto& answer : answers) reply += answer;
            sendto(udp_, reply.data(), int(reply.size()), 0, reinterpret_cast<sockaddr*>(&from), sizeof(from));
        }
    }

    SOCKET udp_ = INVALID_SOCKET;
    uint16_t port_ = 0;
    std::mutex mutex_;
    std::vector<FakeReceiverRecord> records_;
    std::thread thread_;
};

} // namespace testutil
