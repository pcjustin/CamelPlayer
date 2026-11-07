// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// A UPnP AV or OpenHome renderer on 127.0.0.1: answers SSDP searches and description, SOAP and GENA requests, and sends
// events to whoever subscribed. It can move to another port, say goodbye or disappear (docs/Plan.md §5.10).
#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace testutil {

enum class RendererKind { UpnpAv, OpenHome };

class FakeRenderer {
public:
    explicit FakeRenderer(RendererKind kind, std::string udn = "uuid:fake-renderer", std::string name = "Fake Player")
        : kind_(kind), udn_(std::move(udn)), name_(std::move(name)) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        openHttp();
        udp_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in address = loopback();
        if (::bind(udp_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
            throw std::runtime_error("the fake cannot bind");
        int length = sizeof(address);
        getsockname(udp_, reinterpret_cast<sockaddr*>(&address), &length);
        ssdpPort_ = ntohs(address.sin_port);
        httpThread_ = std::thread([this] { serveHttp(); });
        udpThread_ = std::thread([this] { serveSsdp(); });
    }
    ~FakeRenderer() {
        disappear();
        WSACleanup();
    }
    FakeRenderer(const FakeRenderer&) = delete;
    FakeRenderer& operator=(const FakeRenderer&) = delete;

    uint16_t ssdpPort() const { return ssdpPort_; }
    std::string location() const { return "http://127.0.0.1:" + std::to_string(httpPort_) + "/description.xml"; }
    std::string descriptionXml() const {
        std::string services;
        const auto add = [&](const std::string& type, const std::string& kind) {
            services += "<service><serviceType>" + type + "</serviceType><serviceId>urn:upnp-org:serviceId:" + kind +
                        "</serviceId><SCPDURL>/" + kind + ".xml</SCPDURL><controlURL>/ctl/" + kind +
                        "</controlURL><eventSubURL>/evt/" + kind + "</eventSubURL></service>";
        };
        std::string deviceType = "urn:schemas-upnp-org:device:MediaRenderer:1";
        if (kind_ == RendererKind::OpenHome) {
            deviceType = "urn:linn-co-uk:device:Source:1";
            for (const char* kind : {"Product", "Playlist", "Time"})
                add(std::string("urn:av-openhome-org:service:") + kind + ":1", kind);
        } else {
            add("urn:schemas-upnp-org:service:AVTransport:1", "AVTransport");
            add("urn:schemas-upnp-org:service:RenderingControl:1", "RenderingControl");
            add("urn:schemas-upnp-org:service:ConnectionManager:1", "ConnectionManager");
        }
        return R"(<?xml version="1.0"?><root xmlns="urn:schemas-upnp-org:device-1-0"><specVersion><major>1</major><minor>0</minor></specVersion><device><deviceType>)" +
               deviceType + "</deviceType><friendlyName>" + name_ +
               "</friendlyName><manufacturer>Fake</manufacturer><modelName>Model</modelName><UDN>" + udn_ +
               "</UDN><serviceList>" + services + "</serviceList></device></root>";
    }

    // Set before a search: the first LOCATION sent, then the real one, as a player on two networks does.
    void answerAlsoFrom(std::string location) {
        std::scoped_lock lock(mutex_);
        extraLocation_ = std::move(location);
    }
    // What GetProtocolInfo declares.
    void declare(std::string sink) {
        std::scoped_lock lock(mutex_);
        sink_ = std::move(sink);
    }
    // A player that plays one track at a time.
    void withoutNextTrack() { noNext_ = true; }
    // Plays back at this many bytes a second instead of as fast as it fetches.
    void limitFetch(uint64_t bytesPerSecond) { fetchLimit_ = bytesPerSecond; }
    // Takes this much of a next track at once, as a player fills its buffer, before the limit and its clock begin.
    void fillNextTrack(uint64_t bytes) { nextTrackFill_ = bytes; }
    void reportNoPosition() { noPosition_ = true; }
    // After Play it stays TRANSITIONING this long, reporting the position it had, as the DMP-A6 does.
    void startSlowly(int milliseconds) { startMs_ = milliseconds; }
    // Goes on reporting the first URL after it moved on to the next, as the DMP-A6 does.
    void keepReportingFirstTrack() { oldTrack_ = true; }
    // Refuses SetAVTransportURI and Insert, as a player does a format it cannot play.
    void refuseStreams() { refuseStreams_ = true; }

    std::map<std::string, std::string> received() const {
        std::scoped_lock lock(mutex_);
        return received_;
    }
    std::vector<std::string> played() const {
        std::scoped_lock lock(mutex_);
        return played_;
    }
    std::string metadata() const {
        std::scoped_lock lock(mutex_);
        return metadata_;
    }
    std::string currentUri() const {
        std::scoped_lock lock(mutex_);
        return currentUri_;
    }
    std::string state() const {
        std::scoped_lock lock(mutex_);
        const auto found = state_.find("state");
        return found == state_.end() ? std::string() : found->second;
    }
    std::string fetchHeaders() const {
        std::scoped_lock lock(mutex_);
        return fetchHeaders_;
    }
    // Stops fetching and changes to another source's track, as Spotify Connect does.
    void takeOver(const std::string& uri) {
        stopFetch();
        set("track", uri);
        set("state", "PLAYING");
        emitLastChange(R"(<CurrentTrackURI val=")" + uri + R"("/><TransportState val="PLAYING"/>)");
    }

    // Stops answering searches, while its other services stay.
    void goQuiet() { quiet_ = true; }

    void announce(uint16_t notifyPort, const char* subtype) {
        const std::string text = std::string("NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nNT: ") + searchTarget() +
                                 "\r\nNTS: " + subtype + "\r\nUSN: " + udn_ + "::" + searchTarget() +
                                 "\r\nLOCATION: " + location() + "\r\nCACHE-CONTROL: max-age=1800\r\n\r\n";
        sockaddr_in target = loopback();
        target.sin_port = htons(notifyPort);
        sendto(udp_, text.data(), int(text.size()), 0, reinterpret_cast<sockaddr*>(&target), sizeof(target));
    }

    void disappear() {
        if (gone_.exchange(true)) return;
        closesocket(http_);
        closesocket(udp_);
        httpThread_.join();
        udpThread_.join();
        stopFetch();
    }

    // Sends the event to the service's subscriber.
    void emitLastChange(const std::string& variables) {
        emit("AVTransport", "<e:property><LastChange>" + escaped(lastChange(variables)) + "</LastChange></e:property>");
    }
    // As a player's remote does: changes the level and tells whoever subscribed.
    void changeVolume(int level, bool muted) {
        setVolume(level, muted);
        emitVolume(R"(<Volume channel="Master" val=")" + std::to_string(level) + R"("/><Mute channel="Master" val=")" +
                   (muted ? "1" : "0") + R"("/>)");
    }
    // Reports this level from now on without telling anyone, as a player without RenderingControl events does.
    void setVolume(int level, bool muted) {
        set("volume", std::to_string(level));
        set("mute", muted ? "1" : "0");
    }
    void emitVolume(const std::string& variables) {
        emit("RenderingControl", "<e:property><LastChange>" + escaped(lastChange(variables)) + "</LastChange></e:property>");
    }
    void emitVariable(const std::string& service, const std::string& name, const std::string& value) {
        emit(service, "<e:property><" + name + ">" + value + "</" + name + "></e:property>");
    }

    // What SOAP answers report from now on.
    void set(const std::string& name, const std::string& value) {
        std::scoped_lock lock(mutex_);
        state_[name] = value;
    }
    void refuseSoap(bool refuse) { refuseSoap_ = refuse; }
    // Forgets its subscriptions, as a restart does; a renewal then gets 412.
    void forgetSubscriptions() {
        std::scoped_lock lock(mutex_);
        subscribers_.clear();
    }

    std::vector<std::string> actions() const {
        std::scoped_lock lock(mutex_);
        return actions_;
    }
    std::vector<std::string> requests() const {
        std::scoped_lock lock(mutex_);
        return requests_;
    }
    size_t subscribers() const {
        std::scoped_lock lock(mutex_);
        return subscribers_.size();
    }
    int searches() const { return searches_; }

private:
    static std::string lastChange(const std::string& variables) {
        return R"(<Event xmlns="urn:schemas-upnp-org:metadata-1-0/AVT/"><InstanceID val="0">)" + variables +
               "</InstanceID></Event>";
    }
    static std::string escaped(const std::string& text) {
        std::string out;
        for (const char c : text) {
            if (c == '<')
                out += "&lt;";
            else if (c == '>')
                out += "&gt;";
            else if (c == '&')
                out += "&amp;";
            else if (c == '"')
                out += "&quot;";
            else
                out += c;
        }
        return out;
    }
    static sockaddr_in loopback() {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return address;
    }
    const char* searchTarget() const {
        return kind_ == RendererKind::OpenHome ? "urn:av-openhome-org:service:Product:1"
                                               : "urn:schemas-upnp-org:device:MediaRenderer:1";
    }

    void openHttp() {
        http_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address = loopback();
        if (::bind(http_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
            throw std::runtime_error("the fake cannot bind");
        int length = sizeof(address);
        getsockname(http_, reinterpret_cast<sockaddr*>(&address), &length);
        httpPort_ = ntohs(address.sin_port);
        listen(http_, 16);
    }

    void serveSsdp() {
        for (;;) {
            char buffer[2048];
            sockaddr_in from{};
            int length = sizeof(from);
            const int read = recvfrom(udp_, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &length);
            if (read == SOCKET_ERROR || read == 0) {
                if (gone_) return;
                continue;
            }
            const std::string text(buffer, size_t(read));
            if (!text.starts_with("M-SEARCH") || text.find(std::string("ST: ") + searchTarget()) == std::string::npos) continue;
            ++searches_;
            if (quiet_) continue;
            std::string extra;
            {
                std::scoped_lock lock(mutex_);
                extra = extraLocation_;
            }
            for (const auto& location : {extra, this->location()}) {
                if (location.empty()) continue;
                const std::string reply = std::string("HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\nST: ") + searchTarget() +
                                          "\r\nUSN: " + udn_ + "::" + searchTarget() + "\r\nLOCATION: " + location + "\r\n\r\n";
                sendto(udp_, reply.data(), int(reply.size()), 0, reinterpret_cast<sockaddr*>(&from), sizeof(from));
            }
        }
    }

    void serveHttp() {
        for (;;) {
            const SOCKET client = accept(http_, nullptr, nullptr);
            if (std::cmp_equal(client, INVALID_SOCKET)) return;
            std::string request;
            char buffer[8192];
            size_t headEnd = std::string::npos, total = 0;
            for (;;) {
                const int read = recv(client, buffer, sizeof(buffer), 0);
                if (read <= 0) break;
                request.append(buffer, size_t(read));
                if (headEnd == std::string::npos) {
                    headEnd = request.find("\r\n\r\n");
                    if (headEnd != std::string::npos) {
                        const auto length = header(request.substr(0, headEnd), "content-length");
                        total = headEnd + 4 + size_t(std::strtoull(length.c_str(), nullptr, 10));
                    }
                }
                if (headEnd != std::string::npos && request.size() >= total) break;
            }
            const std::string reply = answer(request, headEnd);
            for (size_t sent = 0; sent < reply.size();) {
                const int n = ::send(client, reply.data() + sent, int(reply.size() - sent), 0);
                if (n <= 0) break;
                sent += size_t(n);
            }
            shutdown(client, SD_SEND);
            closesocket(client);
            applyTransport();
        }
    }

    static std::string header(const std::string& head, const std::string& name) {
        std::string lowered = head;
        for (auto& c : lowered) c = char(tolower(static_cast<unsigned char>(c)));
        const auto at = lowered.find("\r\n" + name + ":");
        if (at == std::string::npos) return {};
        auto start = at + 3 + name.size();
        while (start < head.size() && head[start] == ' ') ++start;
        return head.substr(start, head.find("\r\n", start) - start);
    }

    static std::string http(int status, const std::string& body, const std::string& extra = {}) {
        return "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK" : " Error") + "\r\n" + extra +
               "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    }

    std::string answer(const std::string& request, size_t headEnd) {
        if (headEnd == std::string::npos) return http(400, "");
        const std::string head = request.substr(0, headEnd), body = request.substr(headEnd + 4);
        const auto space = head.find(' ');
        const std::string method = head.substr(0, space);
        const std::string path = head.substr(space + 1, head.find(' ', space + 1) - space - 1);
        {
            std::scoped_lock lock(mutex_);
            requests_.push_back(method + " " + path);
        }
        if (method == "GET" && path == "/description.xml") return http(200, descriptionXml());
        if (method == "GET" && path == "/AVTransport.xml") {
            std::string actions;
            for (const char* name : {"SetAVTransportURI", "Play", "Pause", "Stop", "Seek"})
                actions += std::string("<action><name>") + name + "</name></action>";
            if (!noNext_) actions += "<action><name>SetNextAVTransportURI</name></action>";
            return http(200, "<scpd><actionList>" + actions + "</actionList></scpd>");
        }
        if (method == "POST" && path.starts_with("/ctl/")) return soap(header(head, "soapaction"), body);
        if (method == "SUBSCRIBE" && path.starts_with("/evt/")) return subscribe(path.substr(5), head);
        if (method == "UNSUBSCRIBE" && path.starts_with("/evt/")) {
            std::scoped_lock lock(mutex_);
            subscribers_.erase(path.substr(5));
            return http(200, "");
        }
        return http(404, "");
    }

    std::string subscribe(const std::string& service, const std::string& head) {
        std::scoped_lock lock(mutex_);
        const auto sid = header(head, "sid");
        if (!sid.empty()) {
            const auto found = subscribers_.find(service);
            if (found == subscribers_.end() || found->second.sid != sid) return http(412, "");
            return http(200, "", "SID: " + sid + "\r\nTIMEOUT: " + grantedTimeout_ + "\r\n");
        }
        auto callback = header(head, "callback");
        callback = callback.substr(1, callback.size() - 2);
        const std::string fresh = "uuid:sub-" + std::to_string(++nextSid_);
        subscribers_[service] = {fresh, callback};
        return http(200, "", "SID: " + fresh + "\r\nTIMEOUT: " + grantedTimeout_ + "\r\n");
    }

public:
    void grantTimeout(const std::string& timeout) {
        std::scoped_lock lock(mutex_);
        grantedTimeout_ = timeout;
    }

private:
    static std::string between(const std::string& body, const std::string& tag) {
        const auto start = body.find("<" + tag + ">");
        if (start == std::string::npos) return {};
        const auto from = start + tag.size() + 2;
        std::string value = body.substr(from, body.find("</" + tag + ">", from) - from);
        for (const auto& [escaped, plain] :
             std::vector<std::pair<std::string, std::string>>{{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&amp;", "&"}})
            for (auto at = value.find(escaped); at != std::string::npos; at = value.find(escaped, at + plain.size()))
                value.replace(at, escaped.size(), plain);
        return value;
    }

    void startFetch() {
        std::scoped_lock lock(fetchControl_);
        joinFetch();
        fetchStop_ = false;
        fetchThread_ = std::thread([this] { fetchLoop(); });
    }

    void stopFetch() {
        std::scoped_lock lock(fetchControl_);
        joinFetch();
    }

    void joinFetch() {
        fetchStop_ = true;
        const SOCKET open = fetchSocket_.exchange(INVALID_SOCKET);
        if (!std::cmp_equal(open, INVALID_SOCKET)) closesocket(open);
        if (fetchThread_.joinable()) fetchThread_.join();
    }

    void fetchLoop() {
        for (;;) {
            std::string uri;
            {
                std::scoped_lock lock(mutex_);
                uri = currentUri_;
                nextPending_ = false;
            }
            if (uri.empty() || !fetch(uri) || fetchStop_) {
                fetching_ = false;
                return;
            }
            std::string next;
            {
                std::scoped_lock lock(mutex_);
                next = nextUri_;
                nextUri_.clear();
                if (!next.empty()) {
                    currentUri_ = next;
                    state_["time"] = "0:00:00";
                    state_["seconds"] = "0";
                    if (!oldTrack_) state_["track"] = next;
                    for (const auto& [id, entry] : playlist_)
                        if (entry == next) state_["track"] = id;
                } else {
                    state_["state"] = kind_ == RendererKind::OpenHome ? "Stopped" : "STOPPED";
                }
            }
            if (next.empty()) {
                fetching_ = false;
                if (kind_ == RendererKind::UpnpAv) emitLastChange(R"(<TransportState val="STOPPED"/>)");
                return;
            }
            if (oldTrack_) continue;
            if (kind_ == RendererKind::OpenHome)
                emitVariable("Playlist", "Id", state("track"));
            else
                emitLastChange(R"(<CurrentTrackURI val=")" + next + R"("/>)");
        }
    }

    std::string state(const char* key) {
        std::scoped_lock lock(mutex_);
        return state_[key];
    }

    bool fetch(const std::string& uri) {
        const auto hostStart = uri.find("://") + 3, colon = uri.find(':', hostStart), slash = uri.find('/', colon);
        const SOCKET in = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        fetchSocket_ = in;
        sockaddr_in target = loopback();
        target.sin_port = htons(uint16_t(std::strtol(uri.substr(colon + 1, slash - colon - 1).c_str(), nullptr, 10)));
        inet_pton(AF_INET, uri.substr(hostStart, colon - hostStart).c_str(), &target.sin_addr);
        if (connect(in, reinterpret_cast<sockaddr*>(&target), sizeof(target)) != 0) return false;
        const std::string request = "GET " + uri.substr(slash) +
                                    " HTTP/1.1\r\nHost: " + uri.substr(hostStart, slash - hostStart) +
                                    "\r\nConnection: close\r\n\r\n";
        ::send(in, request.data(), int(request.size()), 0);
        std::string data;
        char buffer[32768];
        size_t headEnd = std::string::npos;
        uint64_t bytesPerSecond = 0;
        const auto started = std::chrono::steady_clock::now();
        const uint64_t fill = [&] {
            std::scoped_lock lock(mutex_);
            return played_.empty() ? 0 : nextTrackFill_.load();
        }();
        for (;;) {
            const int read = recv(in, buffer, sizeof(buffer), 0);
            if (read <= 0) break;
            data.append(buffer, size_t(read));
            if (headEnd == std::string::npos) {
                headEnd = data.find("\r\n\r\n");
                if (headEnd == std::string::npos) continue;
                {
                    std::scoped_lock lock(mutex_);
                    fetchHeaders_ = data.substr(0, headEnd);
                }
                if (const int wait = startMs_.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(wait));
                    set("state", "PLAYING");
                }
            }
            const size_t body = data.size() - headEnd - 4;
            if (!bytesPerSecond && body >= 44 && data.compare(headEnd + 4, 4, "RIFF") == 0) {
                uint32_t rate = 0;
                std::memcpy(&rate, data.data() + headEnd + 4 + 28, sizeof(rate));
                bytesPerSecond = rate;
            }
            if (bytesPerSecond && !noPosition_) {
                const uint64_t ms = (body - std::min<uint64_t>(body, fill)) * 1000 / bytesPerSecond;
                char text[32];
                snprintf(text, sizeof(text), "%llu:%02llu:%02llu", ms / 3600000, ms / 60000 % 60, ms / 1000 % 60);
                set("time", text);
                set("seconds", std::to_string(ms / 1000));
            }
            if (const uint64_t limit = fetchLimit_.load()) {
                const auto due = started + std::chrono::milliseconds((body - std::min<uint64_t>(body, fill)) * 1000 / limit);
                std::this_thread::sleep_until(due);
            }
            if (fetchStop_) break;
        }
        if (fetchSocket_.exchange(INVALID_SOCKET) != INVALID_SOCKET) closesocket(in);
        if (headEnd == std::string::npos) return false;
        std::scoped_lock lock(mutex_);
        received_[uri] = data.substr(headEnd + 4);
        played_.push_back(uri);
        return true;
    }

    std::string soap(std::string action, const std::string& body) {
        if (action.size() >= 2 && action.front() == '"') action = action.substr(1, action.size() - 2);
        const auto hash = action.find('#');
        const std::string type = action.substr(0, hash), name = action.substr(hash + 1);
        std::scoped_lock lock(mutex_);
        actions_.push_back(name);
        if (refuseSoap_) {
            return http(
                500,
                R"(<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body><s:Fault><detail><UPnPError xmlns="urn:schemas-upnp-org:control-1-0"><errorCode>501</errorCode></UPnPError></detail></s:Fault></s:Body></s:Envelope>)");
        }
        std::string values;
        if (refuseStreams_ && (name == "SetAVTransportURI" || name == "Insert")) {
            return http(
                500,
                R"(<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body><s:Fault><detail><UPnPError xmlns="urn:schemas-upnp-org:control-1-0"><errorCode>714</errorCode></UPnPError></detail></s:Fault></s:Body></s:Envelope>)");
        }
        if (name == "SetAVTransportURI") {
            currentUri_ = between(body, "CurrentURI");
            metadata_ = between(body, "CurrentURIMetaData");
            state_["track"] = currentUri_;
            nextUri_.clear();
        } else if (name == "SetNextAVTransportURI") {
            nextUri_ = between(body, "NextURI");
            metadata_ = between(body, "NextURIMetaData");
        } else if (name == "Insert") {
            const std::string id = std::to_string(++nextPlaylistId_);
            playlist_[id] = between(body, "Uri");
            metadata_ = between(body, "Metadata");
            if (between(body, "AfterId") == "0") {
                currentUri_ = playlist_[id];
                state_["track"] = id;
            } else {
                nextUri_ = playlist_[id];
            }
            values = "<NewId>" + id + "</NewId>";
        } else if (name == "SeekId") {
            state_["track"] = between(body, "Value");
        } else if (name == "DeleteAll") {
            playlist_.clear();
            nextUri_.clear();
        } else if (name == "Play") {
            state_["state"] = kind_ == RendererKind::OpenHome ? "Playing" : startMs_.load() ? "TRANSITIONING" : "PLAYING";
            if (!fetching_.exchange(true)) playRequested_ = true;
        } else if (name == "Pause") {
            state_["state"] = kind_ == RendererKind::OpenHome ? "Paused" : "PAUSED_PLAYBACK";
        } else if (name == "Stop") {
            state_["state"] = kind_ == RendererKind::OpenHome ? "Stopped" : "STOPPED";
            stopRequested_ = true;
        } else if (name == "SetVolume") {
            state_["volume"] = between(body, "DesiredVolume");
        } else if (name == "SetMute") {
            state_["mute"] = between(body, "DesiredMute");
        } else if (name == "GetProtocolInfo") {
            values = "<Source></Source><Sink>" + sink_ + "</Sink>";
        }
        const auto add = [&](const char* tag, const char* key, const char* fallback) {
            const auto found = state_.find(key);
            values += std::string("<") + tag + ">" + (found == state_.end() ? fallback : found->second) + "</" + tag + ">";
        };
        if (name == "GetTransportInfo") {
            add("CurrentTransportState", "state", "STOPPED");
        } else if (name == "GetVolume") {
            add("CurrentVolume", "volume", "48");
        } else if (name == "GetMute") {
            add("CurrentMute", "mute", "0");
        } else if (name == "GetPositionInfo") {
            add("TrackURI", "track", "");
            add("RelTime", "time", "0:00:00");
        } else if (name == "TransportState") {
            add("Value", "state", "Stopped");
        } else if (name == "Id") {
            add("Value", "track", "");
        } else if (name == "Time") {
            add("Seconds", "seconds", "0");
        } else if (name == "SourceIndex") {
            add("Value", "source", "0");
        }
        return http(200, R"(<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body><u:)" + name +
                             R"(Response xmlns:u=")" + type + R"(">)" + values + "</u:" + name +
                             "Response></s:Body></s:Envelope>");
    }

    // SOAP answers come first, as a real player's do: what they start waits for the next request.
    void applyTransport() {
        if (stopRequested_.exchange(false)) {
            stopFetch();
            fetching_ = false;
        }
        if (playRequested_.exchange(false)) startFetch();
    }

    void emit(const std::string& service, const std::string& properties) {
        std::string sid, callback;
        int sequence = 0;
        {
            std::scoped_lock lock(mutex_);
            const auto found = subscribers_.find(service);
            if (found == subscribers_.end()) return;
            sid = found->second.sid;
            callback = found->second.callback;
            sequence = found->second.sequence++;
        }
        const auto hostStart = callback.find("://") + 3, colon = callback.find(':', hostStart), slash = callback.find('/', colon);
        const SOCKET out = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in target = loopback();
        target.sin_port = htons(uint16_t(std::strtol(callback.substr(colon + 1, slash - colon - 1).c_str(), nullptr, 10)));
        inet_pton(AF_INET, callback.substr(hostStart, colon - hostStart).c_str(), &target.sin_addr);
        if (connect(out, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0) {
            const std::string body = R"(<?xml version="1.0"?><e:propertyset xmlns:e="urn:schemas-upnp-org:event-1-0">)" +
                                     properties + "</e:propertyset>";
            const std::string text = "NOTIFY " + callback.substr(slash) +
                                     " HTTP/1.1\r\nHOST: " + callback.substr(hostStart, slash - hostStart) +
                                     "\r\nCONTENT-TYPE: text/xml\r\nCONTENT-LENGTH: " + std::to_string(body.size()) +
                                     "\r\nNT: upnp:event\r\nNTS: upnp:propchange\r\nSID: " + sid +
                                     "\r\nSEQ: " + std::to_string(sequence) + "\r\n\r\n" + body;
            ::send(out, text.data(), int(text.size()), 0);
            char reply[256];
            recv(out, reply, sizeof(reply), 0);
        }
        closesocket(out);
    }

    struct Subscriber {
        std::string sid, callback;
        int sequence = 0;
    };

    RendererKind kind_;
    std::string udn_, name_;
    SOCKET http_ = INVALID_SOCKET, udp_ = INVALID_SOCKET;
    uint16_t httpPort_ = 0, ssdpPort_ = 0;
    std::thread httpThread_, udpThread_;
    std::atomic<bool> gone_ = false, quiet_ = false, refuseSoap_ = false, noNext_ = false, noPosition_ = false,
                      refuseStreams_ = false;
    std::atomic<bool> fetchStop_ = false, fetching_ = false, playRequested_ = false, stopRequested_ = false, nextPending_ = false;
    std::atomic<bool> oldTrack_ = false;
    std::atomic<uint64_t> nextTrackFill_ = 0;
    std::atomic<uint64_t> fetchLimit_ = 0;
    std::atomic<int> startMs_ = 0;
    std::atomic<SOCKET> fetchSocket_ = INVALID_SOCKET;
    std::mutex fetchControl_; // the HTTP thread and the test both start and stop the fetch
    std::thread fetchThread_;
    std::string sink_ = "http-get:*:audio/wav:*,http-get:*:audio/L16:*", currentUri_, nextUri_, metadata_, fetchHeaders_;
    std::map<std::string, std::string> received_, playlist_;
    std::vector<std::string> played_;
    int nextPlaylistId_ = 0;
    std::atomic<int> searches_ = 0;
    mutable std::mutex mutex_;
    std::map<std::string, std::string> state_;
    std::map<std::string, Subscriber> subscribers_;
    std::vector<std::string> actions_, requests_;
    std::string extraLocation_, grantedTimeout_ = "Second-1800";
    int nextSid_ = 0;
};

} // namespace testutil
