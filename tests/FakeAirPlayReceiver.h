// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// AirPlay receivers on 127.0.0.1: one mDNS responder answering for all of them, and an RTSP port each that answers what
// a test says and records what it was asked (docs/Plan.md §5.10).
#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace testutil {

class FakeRtsp {
public:
    FakeRtsp() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        listen(listener_, 8);
        int length = sizeof(address);
        getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { serve(); });
    }
    ~FakeRtsp() {
        closesocket(listener_);
        thread_.join();
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

private:
    void serve() {
        for (;;) {
            const SOCKET client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) return;
            std::string received;
            char buffer[4096];
            for (;;) {
                const int got = recv(client, buffer, sizeof(buffer), 0);
                if (got <= 0) break;
                received.append(buffer, size_t(got));
                while (answerOne(client, received)) {
                }
            }
            closesocket(client);
        }
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
        int status = 200;
        {
            std::scoped_lock lock(mutex_);
            requests_.push_back(request);
            if (const auto chosen = statuses_.find(request.substr(0, request.find(' '))); chosen != statuses_.end())
                status = chosen->second;
        }
        const size_t sequence = request.find("CSeq: ");
        const std::string reply = "RTSP/1.0 " + std::to_string(status) + " Reason\r\nCSeq: " +
                                  request.substr(sequence + 6, request.find("\r\n", sequence) - sequence - 6) +
                                  "\r\nServer: Fake\r\n\r\n";
        send(client, reply.data(), int(reply.size()), 0);
        return true;
    }

    SOCKET listener_ = INVALID_SOCKET;
    uint16_t port_ = 0;
    mutable std::mutex mutex_;
    std::map<std::string, int> statuses_;
    std::vector<std::string> requests_;
    std::thread thread_;
};

struct FakeReceiverRecord {
    std::string deviceId, name, host;
    uint16_t port = 0;
    std::string address = "127.0.0.1";
    std::vector<std::string> raopTxt = {"cn=0,1", "et=0,1", "pw=false", "vs=105.1"};
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
