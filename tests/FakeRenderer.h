// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// A UPnP AV or OpenHome renderer on 127.0.0.1: answers SSDP searches and description, SOAP and GENA requests, and sends
// events to whoever subscribed. It can move to another port, say goodbye or disappear (docs/Plan.md §5.10).
#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
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
        ::bind(udp_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
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
    }

    // Sends the event to the service's subscriber.
    void emitLastChange(const std::string& variables) {
        const std::string inner =
            R"(<Event xmlns="urn:schemas-upnp-org:metadata-1-0/AVT/"><InstanceID val="0">)" + variables + "</InstanceID></Event>";
        std::string escaped;
        for (const char c : inner) {
            if (c == '<')
                escaped += "&lt;";
            else if (c == '>')
                escaped += "&gt;";
            else if (c == '&')
                escaped += "&amp;";
            else if (c == '"')
                escaped += "&quot;";
            else
                escaped += c;
        }
        emit("AVTransport", "<e:property><LastChange>" + escaped + "</LastChange></e:property>");
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
        ::bind(http_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
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
            if (client == INVALID_SOCKET) return;
            std::string request;
            char buffer[8192];
            size_t headEnd = std::string::npos, total = 0;
            for (;;) {
                const int read = recv(client, buffer, sizeof(buffer), 0);
                if (read <= 0) break;
                request.append(buffer, size_t(read));
                if (headEnd == std::string::npos && (headEnd = request.find("\r\n\r\n")) != std::string::npos) {
                    const auto length = header(request.substr(0, headEnd), "content-length");
                    total = headEnd + 4 + size_t(atoll(length.c_str()));
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
    std::string soap(std::string action, const std::string&) {
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
        const auto add = [&](const char* tag, const char* key, const char* fallback) {
            const auto found = state_.find(key);
            values += std::string("<") + tag + ">" + (found == state_.end() ? fallback : found->second) + "</" + tag + ">";
        };
        if (name == "GetTransportInfo") {
            add("CurrentTransportState", "state", "STOPPED");
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
        target.sin_port = htons(uint16_t(atoi(callback.substr(colon + 1, slash - colon - 1).c_str())));
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
    std::atomic<bool> gone_ = false, quiet_ = false, refuseSoap_ = false;
    std::atomic<int> searches_ = 0;
    mutable std::mutex mutex_;
    std::map<std::string, std::string> state_;
    std::map<std::string, Subscriber> subscribers_;
    std::vector<std::string> actions_, requests_;
    std::string extraLocation_, grantedTimeout_ = "Second-1800";
    int nextSid_ = 0;
};

} // namespace testutil
