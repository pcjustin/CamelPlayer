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
#include <cstring>
#include <format>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "Server/Network/HQPlayer.h"
#include "Server/Support/Http.h"

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
    // Plays as HQPlayer does: probes each URL added (HEAD, then a GET broken off), and on Play fetches the tracks of its
    // playlist one after the other, the position moving with the time played, a new track_serial at each.
    std::atomic<bool> plays = false;
    std::atomic<bool> outputFails = false; // as with its DAC off: Play is OK, a moment of playing, then stopped
    // That moment; on macOS with DoP off, DSD stopped about 3 s after Play (2026-11-17).
    std::chrono::milliseconds outputFailsAfter{300};
    // No Status pushed for this long after Play, as HQPlayer starting DSD (about 4 s on 2026-11-16).
    std::chrono::milliseconds quietAfterPlay{0};
    // Play taking effect this long after it is answered, with no Status meanwhile, and Pause or Stop sent meanwhile only
    // after it, as HQPlayer starting DSD did on 2026-11-19 (about 4 s).
    std::chrono::milliseconds startTakes{0};
    // Playing at position 0 for this long after Play, as HQPlayer on macOS starting DSD (about 7 s on 2026-11-17).
    std::chrono::milliseconds positionHeldFor{0};
    // PlaylistAdd answered only once the probe read this many bytes of the stream, as HQPlayer on macOS did with DSF
    // on 2026-11-17.
    std::atomic<size_t> readBeforeAddReply = 0;

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
        player_ = std::thread([this] { play(); });
        answerer_ = std::thread([this] { answer(); });
    }
    ~FakeHQPlayer() {
        stopping_ = true;
        if (player_.joinable()) player_.join();
        for (auto& f : fetchers_) f.join();
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
    // The streams fetched whole, by URL, and the URLs as they were added.
    std::map<std::string, std::string> received() const {
        std::scoped_lock lock(mutex_);
        return received_;
    }
    std::vector<std::string> added() const {
        std::scoped_lock lock(mutex_);
        return added_;
    }

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
            if (quietAfterPlay.count() && std::chrono::steady_clock::now() - playStarted_ < quietAfterPlay) return;
            if (starting_) return;
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
                    // As HQPlayer Desktop 6 did on 2026-11-11: Volume sets the level in dB, VolumeMute the minimum.
                    if (element.name == "Volume" && state_.volumeEnabled)
                        state_.volume = std::stod(element.attributes.at("value"));
                    if (element.name == "VolumeMute" && state_.volumeEnabled) state_.volume = state_.volumeMin;
                    if (plays) simulate(element);
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
                if (element.name == "PlaylistAdd" && plays && readBeforeAddReply)
                    probe(element.attributes.at("uri"), readBeforeAddReply);
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

    void simulate(const camel::server::upnp::XmlNode& element) {
        if (element.name == "PlaylistAdd") {
            const std::string uri = element.attributes.at("uri");
            playlist_.push_back(uri);
            added_.push_back(uri);
            ++state_.transportSerial;
            if (!readBeforeAddReply) probes_.push_back(uri);
        } else if (element.name == "PlaylistClear") {
            playlist_.clear();
            current_ = 0;
            ++state_.transportSerial;
            ++state_.trackSerial; // as HQPlayer did on 2026-11-16
        } else if (starting_ && (element.name == "Play" || element.name == "Pause" || element.name == "Stop")) {
            afterStart_.push_back(element.name);
        } else if (element.name == "Play" && startTakes.count() && state_.state != 2) {
            starting_ = std::chrono::steady_clock::now() + startTakes;
        } else {
            transport(element.name);
        }
    }

    void transport(const std::string& command) {
        if (command == "Play") {
            if (state_.state != 1) playedBefore_ = state_.position;
            state_.state = 2;
            playStarted_ = std::chrono::steady_clock::now();
            wantPlay_ = true;
        } else if (command == "Pause") {
            state_.state = 1;
            playedBefore_ = state_.position;
        } else if (command == "Stop") {
            state_.state = 0;
            state_.position = 0;
        }
    }

    // Play taking effect once HQPlayer has started, then what was sent meanwhile, each with its own Status.
    void finishStarting() {
        std::vector<std::string> after;
        {
            std::scoped_lock lock(mutex_);
            if (!starting_ || std::chrono::steady_clock::now() < *starting_) return;
            starting_.reset();
            transport("Play");
            after.swap(afterStart_);
        }
        pushStatus();
        for (const auto& command : after) {
            {
                std::scoped_lock lock(mutex_);
                transport(command);
            }
            pushStatus();
        }
    }

    static uint64_t le64(const std::string& bytes, size_t at) {
        uint64_t value = 0;
        std::memcpy(&value, bytes.data() + at, sizeof value);
        return value;
    }

    // A WAV or DSF body's length in seconds, from its header; 0 for anything else.
    static double secondsOf(const std::string& body) {
        const auto le32 = [&](size_t at) { return *reinterpret_cast<const uint32_t*>(body.data() + at); };
        if (body.size() >= 92 && body.compare(0, 4, "DSD ") == 0) {
            const uint32_t rate = le32(56);
            return rate ? double(le64(body, 64)) / rate : 0.0;
        }
        if (body.size() < 44 || body.compare(0, 4, "RIFF") != 0) return 0.0;
        const uint32_t byteRate = le32(28);
        return byteRate ? double(body.size() - 44) / byteRate : 0.0;
    }

    // A DSF whose header points to no metadata, which HQPlayer did not follow with the next track.
    static bool dsfWithoutMetadata(const std::string& body) {
        return body.size() >= 92 && body.compare(0, 4, "DSD ") == 0 && le64(body, 20) == 0;
    }

    // Probes, fetches and position, as HQPlayer's own playing: a track is fetched on a thread of its own while the
    // position moves on, and lasts as long as its samples.
    void play() {
        while (!stopping_) {
            finishStarting();
            std::vector<std::string> probes;
            std::string fetch;
            bool failNow = false;
            {
                std::scoped_lock lock(mutex_);
                probes.swap(probes_);
                if (state_.state == 2 && wantPlay_ && current_ < playlist_.size()) {
                    const std::string& uri = playlist_[current_];
                    if (outputFails) {
                        failNow = true;
                    } else if (!fetching_.contains(uri) && !received_.contains(uri)) {
                        fetching_.insert(uri);
                        fetch = uri;
                    }
                }
            }
            for (const auto& uri : probes) probe(uri);
            if (failNow) {
                std::this_thread::sleep_for(outputFailsAfter);
                change([](State& s) { s.state = 0; });
                std::scoped_lock lock(mutex_);
                wantPlay_ = false;
                continue;
            }
            if (!fetch.empty())
                fetchers_.emplace_back([this, fetch] {
                    std::string body;
                    try {
                        body = camel::server::http::send("GET", fetch, "", "", 30000, size_t{256} << 20).body;
                    } catch (const std::exception&) {
                    }
                    std::scoped_lock lock(mutex_);
                    fetching_.erase(fetch);
                    received_[fetch] = std::move(body);
                });
            {
                std::scoped_lock lock(mutex_);
                if (state_.state == 2 && wantPlay_) {
                    const auto since = std::chrono::steady_clock::now() - playStarted_;
                    state_.position = since < positionHeldFor
                                          ? playedBefore_
                                          : playedBefore_ + 0.1 + std::chrono::duration<double>(since - positionHeldFor).count();
                    if (current_ < playlist_.size())
                        if (const auto known = lengths_.find(playlist_[current_]); known != lengths_.end())
                            state_.length = known->second;
                    const auto got = current_ < playlist_.size() ? received_.find(playlist_[current_]) : received_.end();
                    if (got != received_.end()) {
                        state_.length = secondsOf(got->second);
                        if (state_.position >= state_.length) {
                            if (current_ + 1 < playlist_.size() && dsfWithoutMetadata(got->second)) {
                                // As HQPlayer Desktop 6.3.3 on Windows did on 2026-11-19: it stops where it is.
                                state_.state = 0;
                                state_.position = 0;
                                wantPlay_ = false;
                            } else if (current_ + 1 < playlist_.size()) {
                                ++current_;
                                ++state_.trackSerial;
                                state_.position = 0;
                                state_.length = 0;
                                playStarted_ = std::chrono::steady_clock::now();
                                playedBefore_ = 0;
                            } else {
                                state_.state = 0;
                                state_.position = 0;
                                ++state_.trackSerial; // as HQPlayer does when its playlist runs out
                                wantPlay_ = false;
                            }
                        }
                    }
                }
            }
            pushStatus();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    // HEAD, then a GET read for its first bytes and broken off, as HQPlayer probes a URL it is given.
    void probe(const std::string& uri, size_t want = 128 * 1024) {
        try {
            camel::server::http::send("HEAD", uri, "", "", 3000, 1 << 16);
        } catch (const std::exception&) {
        }
        std::string host;
        uint16_t port = 0;
        const auto start = uri.find("://") + 3, slash = uri.find('/', start);
        if (!camel::server::hqplayer::splitAddress(uri.substr(start, slash - start), host, port)) return;
        const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &address.sin_addr);
        const DWORD patience = 5000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&patience), sizeof(patience));
        if (connect(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
            const std::string request = "GET " + uri.substr(slash) + " HTTP/1.1\r\nHost: " + host + "\r\n\r\n";
            send(s, request.data(), int(request.size()), 0);
            char chunk[8192];
            std::string head;
            size_t read = 0;
            while (read < want) {
                const int n = recv(s, chunk, sizeof(chunk), 0);
                if (n <= 0) break;
                if (head.size() < 4096) head.append(chunk, size_t(n));
                read += size_t(n);
            }
            // The length HQPlayer reports comes from the stream's header.
            if (const auto body = head.find("\r\n\r\n"); body != std::string::npos && head.size() >= body + 4 + 44) {
                const std::string wav = head.substr(body + 4);
                const auto le32 = [&](size_t at) { return *reinterpret_cast<const uint32_t*>(wav.data() + at); };
                if (wav.compare(0, 4, "RIFF") == 0 && le32(28)) {
                    std::scoped_lock lock(mutex_);
                    lengths_[uri] = double(le32(40)) / le32(28);
                }
            }
        }
        closesocket(s);
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
    std::thread acceptor_, answerer_, player_;
    std::vector<std::string> playlist_, added_, probes_;
    size_t current_ = 0;
    bool wantPlay_ = false;
    double playedBefore_ = 0;
    std::chrono::steady_clock::time_point playStarted_;
    std::optional<std::chrono::steady_clock::time_point> starting_;
    std::vector<std::string> afterStart_;
    std::map<std::string, std::string> received_;
    std::set<std::string> fetching_;
    std::map<std::string, double> lengths_;
    std::vector<std::thread> fetchers_;
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
