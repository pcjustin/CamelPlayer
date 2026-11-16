// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// HQPlayer's control port and discovery on 127.0.0.1, both on one port number as HQPlayer has them, answering as
// HQPlayer Desktop 6 did (docs/reports/HQPlayer-control-2026-11-16.md), with the faults a test asks for.
#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <format>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "Server/Network/HQPlayer.h"

namespace testutil {

class FakeHQPlayer {
public:
    struct State {
        int state = 0;
        double position = 0.0, length = 0.0, volume = -3.0;
        uint32_t trackSerial = 0, transportSerial = 0, track = 0;
        bool volumeEnabled = false;
        double volumeMin = -12.0, volumeMax = 0.0;
        std::string activeMode = "[source]", activeFilter = "poly-sinc-gauss-long", activeShaper = "TPDF";
        uint32_t activeRate = 44100, activeBits = 32;
    };

    std::chrono::milliseconds firstReplyDelay{0};
    std::atomic<bool> splitReplies = false;    // each reply in two writes, a moment apart
    std::atomic<bool> joinReplies = false;     // a pushed Status and the reply in one write
    std::atomic<bool> pushBeforeReply = false; // a Status pushed between a command and its reply
    std::atomic<bool> answerDiscovery = true;
    std::string name = "FAKE-HQP", version = "Signalyst HQPlayer Desktop 6";

    FakeHQPlayer() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        EXPECT_EQ(bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
        int length = sizeof(address);
        getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        listen(listener_, 8);
        discovery_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        EXPECT_EQ(bind(discovery_, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
        acceptor_ = std::thread([this] { accept(); });
        answerer_ = std::thread([this] { answer(); });
    }
    ~FakeHQPlayer() {
        stopping_ = true;
        closesocket(listener_);
        closesocket(discovery_);
        dropConnections();
        acceptor_.join();
        answerer_.join();
        for (auto& t : connections_) t.join();
        WSACleanup();
    }
    FakeHQPlayer(const FakeHQPlayer&) = delete;
    FakeHQPlayer& operator=(const FakeHQPlayer&) = delete;

    uint16_t port() const { return port_; }
    std::string address() const { return camel::server::hqplayer::addressOf("localhost", port_); }

    // The commands received, by element name, oldest first.
    std::vector<std::string> commands() const {
        std::scoped_lock lock(mutex_);
        return commands_;
    }
    std::vector<std::string> raw() const {
        std::scoped_lock lock(mutex_);
        return raw_;
    }
    size_t accepted() const { return accepted_; }

    // The next command of this name is not answered.
    void dropNext(const std::string& command) {
        std::scoped_lock lock(mutex_);
        drop_.insert(command);
    }
    // Answers a command of this name with the reply `make` gives, instead of `result="OK"`.
    void replyTo(const std::string& command, std::function<std::string(const camel::server::upnp::XmlNode&)> make) {
        std::scoped_lock lock(mutex_);
        replies_[command] = std::move(make);
    }
    // What a command does to the state, as HQPlayer's own playing would.
    void onCommand(std::function<void(const std::string&, const camel::server::upnp::XmlNode&, State&)> act) {
        std::scoped_lock lock(mutex_);
        act_ = std::move(act);
    }
    void change(const std::function<void(State&)>& edit, bool push = true) {
        {
            std::scoped_lock lock(mutex_);
            edit(state_);
        }
        if (push) pushStatus();
    }
    State state() const {
        std::scoped_lock lock(mutex_);
        return state_;
    }
    // Sends every subscribed connection a Status, as HQPlayer does about once a second and on every change.
    void pushStatus() {
        std::vector<SOCKET> targets;
        std::string status;
        {
            std::scoped_lock lock(mutex_);
            targets.assign(subscribed_.begin(), subscribed_.end());
            status = statusDocument();
        }
        for (const SOCKET s : targets) send(s, status.data(), int(status.size()), 0);
    }
    // HQPlayer quitting: every connection closes at once.
    void dropConnections() {
        std::scoped_lock lock(mutex_);
        for (const SOCKET s : open_) {
            shutdown(s, SD_BOTH);
            closesocket(s);
        }
        open_.clear();
        subscribed_.clear();
    }

private:
    std::string statusDocument() const {
        return std::format(
            R"(<?xml version="1.0" encoding="utf-8"?><Status active_bits="{}" active_channels="2" active_filter="{}" )"
            R"(active_mode="{}" active_rate="{}" active_shaper="{}" length="{}" position="{}" state="{}" track="{}" )"
            R"(track_serial="{}" transport_serial="{}" volume="{}"/>)",
            state_.activeBits, state_.activeFilter, state_.activeMode, state_.activeRate, state_.activeShaper, state_.length,
            state_.position, state_.state, state_.track, state_.trackSerial, state_.transportSerial, state_.volume);
    }

    void accept() {
        while (!stopping_) {
            const SOCKET client = ::accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) return;
            {
                std::scoped_lock lock(mutex_);
                open_.insert(client);
            }
            const bool first = accepted_++ == 0;
            connections_.emplace_back([this, client, first] { serve(client, first); });
        }
    }

    void write(SOCKET s, const std::string& text) {
        if (splitReplies && text.size() > 4) {
            const size_t half = text.size() / 2;
            send(s, text.data(), int(half), 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            send(s, text.data() + half, int(text.size() - half), 0);
        } else {
            send(s, text.data(), int(text.size()), 0);
        }
    }

    void serve(SOCKET s, bool) {
        std::string buffer;
        char chunk[4096];
        bool answeredOnce = false;
        while (true) {
            const int length = recv(s, chunk, sizeof(chunk), 0);
            if (length <= 0) break;
            buffer.append(chunk, size_t(length));
            for (const auto& document : camel::server::hqplayer::takeDocuments(buffer)) {
                const auto root = camel::server::upnp::parseXml(document);
                if (!root) continue;
                const auto& element = *root;
                std::string reply;
                bool dropped = false;
                {
                    std::scoped_lock lock(mutex_);
                    commands_.push_back(element.name);
                    raw_.push_back(document);
                    if (act_) act_(element.name, element, state_);
                    dropped = drop_.erase(element.name) > 0;
                    if (element.name == "Status") {
                        const auto subscribe = element.attributes.find("subscribe");
                        if (subscribe == element.attributes.end() || subscribe->second != "0") subscribed_.insert(s);
                        reply = statusDocument();
                    } else if (const auto custom = replies_.find(element.name); custom != replies_.end()) {
                        reply = custom->second(element);
                    } else if (element.name == "GetInfo") {
                        reply = std::format(R"(<?xml version="1.0" encoding="utf-8"?><GetInfo engine="6.3.3" name="{}" )"
                                            R"(platform="Windows" product="Signalyst HQPlayer Desktop" version="6"/>)",
                                            name);
                    } else if (element.name == "VolumeRange") {
                        reply = std::format(R"(<?xml version="1.0" encoding="utf-8"?><VolumeRange adaptive="0" enabled="{}" )"
                                            R"(max="{}" min="{}"/>)",
                                            state_.volumeEnabled ? 1 : 0, state_.volumeMax, state_.volumeMin);
                    } else {
                        reply = std::format(R"(<?xml version="1.0" encoding="utf-8"?><{} result="OK"/>)", element.name);
                    }
                }
                if (dropped) continue;
                if (!answeredOnce && firstReplyDelay.count()) std::this_thread::sleep_for(firstReplyDelay);
                answeredOnce = true;
                std::string pushed;
                if (pushBeforeReply || joinReplies) {
                    std::scoped_lock lock(mutex_);
                    pushed = statusDocument() + "\n";
                }
                if (joinReplies) {
                    write(s, pushed + reply + "\n");
                } else {
                    if (!pushed.empty()) write(s, pushed);
                    write(s, reply + "\n");
                }
            }
        }
        std::scoped_lock lock(mutex_);
        subscribed_.erase(s);
        if (open_.erase(s)) closesocket(s);
    }

    void answer() {
        char buffer[2048];
        while (!stopping_) {
            sockaddr_in from{};
            int fromLength = sizeof(from);
            const int length = recvfrom(discovery_, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &fromLength);
            if (length <= 0) {
                if (stopping_) return;
                continue;
            }
            if (!answerDiscovery) continue;
            const std::string request(buffer, size_t(length));
            const bool ours = request.find(">hqplayer<") != std::string::npos;
            const std::string reply =
                std::format(R"(<?xml version="1.0" encoding="utf-8"?><discover name="{}" result="{}" version="{}">{}</discover>)",
                            name, ours ? "OK" : "NA", version, ours ? "hqplayer" : "other");
            sendto(discovery_, reply.data(), int(reply.size()), 0, reinterpret_cast<const sockaddr*>(&from), fromLength);
        }
    }

    SOCKET listener_ = INVALID_SOCKET, discovery_ = INVALID_SOCKET;
    uint16_t port_ = 0;
    std::atomic<bool> stopping_ = false;
    std::atomic<size_t> accepted_ = 0;
    std::thread acceptor_, answerer_;
    std::vector<std::thread> connections_;
    mutable std::mutex mutex_;
    State state_;
    std::set<SOCKET> open_, subscribed_;
    std::set<std::string> drop_;
    std::map<std::string, std::function<std::string(const camel::server::upnp::XmlNode&)>> replies_;
    std::function<void(const std::string&, const camel::server::upnp::XmlNode&, State&)> act_;
    std::vector<std::string> commands_, raw_;
};

} // namespace testutil
