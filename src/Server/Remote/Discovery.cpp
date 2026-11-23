// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Remote/Discovery.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <map>
#include <thread>

#include "Common/util/Log.h"
#include "Server/Network/AirPlay.h"
#include "Server/Network/LocalAddresses.h"
#include "Server/Network/SocketWait.h"

namespace camel::server {
namespace {

using Labels = std::vector<std::string>;

constexpr uint16_t kMdnsPort = 5353;
constexpr char kGroup[] = "224.0.0.251";

std::string lower(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

bool same(const Labels& a, const Labels& b) {
    return a.size() == b.size() &&
           std::ranges::equal(a, b, [](const std::string& x, const std::string& y) { return lower(x) == lower(y); });
}

Labels serviceLabels() {
    return {"_camelplayer", "_tcp", "local"};
}

Labels instanceLabels(const std::string& name) {
    Labels labels = serviceLabels();
    labels.insert(labels.begin(), name.substr(0, 63));
    return labels;
}

void put16(std::string& out, uint16_t value) {
    out += char(value >> 8);
    out += char(value & 0xFF);
}

void put32(std::string& out, uint32_t value) {
    put16(out, uint16_t(value >> 16));
    put16(out, uint16_t(value & 0xFFFF));
}

void putName(std::string& out, const Labels& labels) {
    for (const auto& label : labels) {
        out += char(std::min<size_t>(label.size(), 63));
        out += label.substr(0, 63);
    }
    out += '\0';
}

void putRecord(std::string& out, const Labels& name, uint16_t type, bool unique, uint32_t ttl, const std::string& data) {
    putName(out, name);
    put16(out, type);
    put16(out, unique ? 0x8001 : 0x0001); // a unique record flushes caches
    put32(out, ttl);
    put16(out, uint16_t(data.size()));
    out += data;
}

// A name at `at`, following compression pointers; empty when it cannot be read.
Labels readName(std::string_view message, size_t& at) {
    Labels labels;
    size_t position = at;
    bool jumped = false;
    for (int hops = 0; hops < 32; ++hops) {
        if (position >= message.size()) return {};
        const auto length = uint8_t(message[position]);
        if (length == 0) {
            if (!jumped) at = position + 1;
            return labels;
        }
        if ((length & 0xC0) == 0xC0) {
            if (position + 1 >= message.size()) return {};
            if (!jumped) at = position + 2;
            jumped = true;
            position = size_t(length & 0x3F) << 8 | uint8_t(message[position + 1]);
            continue;
        }
        if (length > 63 || position + 1 + length > message.size()) return {};
        labels.emplace_back(message.substr(position + 1, length));
        position += 1 + length;
    }
    return {};
}

uint16_t read16(std::string_view message, size_t at) {
    return uint16_t(uint8_t(message[at]) << 8 | uint8_t(message[at + 1]));
}

std::string hostName() {
    char name[256] = {};
    gethostname(name, sizeof(name));
    std::string host(name);
    return host.empty() ? std::string("camelplayer") : host.substr(0, host.find('.'));
}

std::vector<std::string> addressTexts() {
    std::vector<std::string> out;
    for (const auto& address : localAddresses()) {
        char text[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &address, text, sizeof(text));
        out.emplace_back(text);
    }
    return out;
}

sockaddr_in groupAddress() {
    sockaddr_in group{};
    group.sin_family = AF_INET;
    group.sin_port = htons(kMdnsPort);
    inet_pton(AF_INET, kGroup, &group.sin_addr);
    return group;
}

// Sends `message` to the group through every interface, as each has its own subnet.
void sendToGroup(SOCKET s, const std::string& message) {
    const auto group = groupAddress();
    for (const auto& address : localAddresses()) {
        setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&address), sizeof(address));
        sendto(s, message.data(), int(message.size()), 0, reinterpret_cast<const sockaddr*>(&group), sizeof(group));
    }
}

} // namespace

namespace discovery {

std::string responseMessage(const ServerAnnouncement& announcement, std::string_view host,
                            const std::vector<std::string>& addresses, uint32_t ttl, uint16_t id, std::string_view question) {
    std::string out;
    put16(out, id);
    put16(out, 0x8400); // a response, authoritative
    put16(out, question.empty() ? 0 : 1);
    put16(out, uint16_t(3 + addresses.size()));
    put16(out, 0);
    put16(out, 0);
    out += question;
    const auto instance = instanceLabels(announcement.name.empty() ? std::string(host) : announcement.name);
    const Labels hostLabels = {std::string(host), "local"};
    std::string ptr;
    putName(ptr, instance);
    putRecord(out, serviceLabels(), uint16_t(airplay::DnsType::Ptr), false, ttl, ptr);
    std::string srv;
    put16(srv, 0);
    put16(srv, 0);
    put16(srv, announcement.port);
    putName(srv, hostLabels);
    putRecord(out, instance, uint16_t(airplay::DnsType::Srv), true, ttl, srv);
    std::string txt;
    for (const auto& entry : {"id=" + announcement.id, "name=" + announcement.name,
                              "api=" + std::to_string(announcement.apiMin) + "-" + std::to_string(announcement.apiMax)}) {
        txt += char(std::min<size_t>(entry.size(), 255));
        txt += entry.substr(0, 255);
    }
    putRecord(out, instance, uint16_t(airplay::DnsType::Txt), true, ttl, txt);
    for (const auto& address : addresses) {
        in_addr ip{};
        if (inet_pton(AF_INET, address.c_str(), &ip) != 1) continue;
        putRecord(out, hostLabels, uint16_t(airplay::DnsType::A), true, ttl, std::string(reinterpret_cast<const char*>(&ip), 4));
    }
    return out;
}

bool asksFor(std::string_view message, std::string_view host, std::string* question) {
    if (message.size() < 12 || (uint8_t(message[2]) & 0x80)) return false; // too short, or a response
    const uint16_t questions = read16(message, 4);
    size_t at = 12;
    const Labels hostLabels = {std::string(host), "local"};
    for (uint16_t i = 0; i < questions; ++i) {
        const auto name = readName(message, at);
        if (name.empty() || at + 4 > message.size()) return false;
        const auto type = read16(message, at);
        const auto klass = read16(message, at + 2);
        at += 4;
        const bool service = same(name, serviceLabels());
        const bool instance = name.size() == 4 && same(Labels(name.begin() + 1, name.end()), serviceLabels());
        if (service || instance || same(name, hostLabels)) {
            if (question) {
                question->clear();
                putName(*question, name);
                put16(*question, type);
                put16(*question, uint16_t(klass & 0x7FFF));
            }
            return true;
        }
    }
    return false;
}

std::vector<FoundServer> serversIn(const std::vector<std::string>& messages) {
    std::vector<airplay::DnsRecord> records;
    for (const auto& message : messages) {
        auto parsed = airplay::parseRecords(message);
        records.insert(records.end(), parsed.begin(), parsed.end());
    }
    std::vector<FoundServer> found;
    for (const auto& ptr : records) {
        if (ptr.type != airplay::DnsType::Ptr || !same(ptr.name, serviceLabels())) continue;
        FoundServer server;
        Labels host;
        for (const auto& r : records) {
            if (!same(r.name, ptr.target)) continue;
            if (r.type == airplay::DnsType::Srv) {
                server.announced.port = r.port;
                host = r.target;
            } else if (r.type == airplay::DnsType::Txt) {
                const auto value = [&](const char* key) {
                    const auto f = r.txt.find(key);
                    return f == r.txt.end() ? std::string() : f->second;
                };
                server.announced.id = value("id");
                server.announced.name = value("name");
                const auto api = value("api");
                if (const auto dash = api.find('-'); dash != std::string::npos) {
                    std::from_chars(api.data(), api.data() + dash, server.announced.apiMin);
                    std::from_chars(api.data() + dash + 1, api.data() + api.size(), server.announced.apiMax);
                }
            }
        }
        for (const auto& r : records)
            if (r.type == airplay::DnsType::A && !host.empty() && same(r.name, host)) {
                server.address = r.address;
                break;
            }
        if (server.announced.id.empty() || server.address.empty() || server.announced.port == 0) continue;
        if (std::ranges::none_of(found, [&](const FoundServer& f) { return f.announced.id == server.announced.id; }))
            found.push_back(std::move(server));
    }
    return found;
}

} // namespace discovery

struct ServerAnnouncer::Impl {
    explicit Impl(ServerAnnouncement a) : announcement(std::move(a)), host(hostName()) {}
    ServerAnnouncement announcement;
    std::string host;
    WinsockScope winsock;
    SOCKET socket = INVALID_SOCKET;
    std::jthread thread;

    std::string response(uint32_t ttl, uint16_t id = 0, std::string_view question = {}) const {
        return discovery::responseMessage(announcement, host, addressTexts(), ttl, id, question);
    }

    void run(const std::stop_token& stop) {
        sendToGroup(socket, response(120));
        auto again = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!stop.stop_requested()) {
            if (again.time_since_epoch().count() && std::chrono::steady_clock::now() >= again) {
                sendToGroup(socket, response(120));
                again = {};
            }
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(socket, &readable);
            if (!waitReadable(readable, std::chrono::milliseconds(250))) continue;
            char buffer[9000];
            sockaddr_in from{};
            int length = sizeof(from);
            const int got = recvfrom(socket, buffer, int(sizeof(buffer)), 0, reinterpret_cast<sockaddr*>(&from), &length);
            if (got <= 0) continue;
            const std::string_view message(buffer, size_t(got));
            std::string question;
            if (!discovery::asksFor(message, host, &question)) continue;
            // A query from a port of its own wants its answer there, with its ID and question back.
            if (ntohs(from.sin_port) != kMdnsPort) {
                const auto reply = response(10, read16(message, 0), question);
                sendto(socket, reply.data(), int(reply.size()), 0, reinterpret_cast<const sockaddr*>(&from), length);
            } else {
                sendToGroup(socket, response(120));
            }
        }
        sendToGroup(socket, response(0));
    }
};

ServerAnnouncer::ServerAnnouncer(ServerAnnouncement announcement) : impl_(std::make_unique<Impl>(std::move(announcement))) {
    impl_->socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    BOOL reuse = TRUE;
    setsockopt(impl_->socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in any{};
    any.sin_family = AF_INET;
    any.sin_port = htons(kMdnsPort);
    any.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(impl_->socket, reinterpret_cast<sockaddr*>(&any), sizeof(any)) != 0)
        log::warn("Server API: mDNS port 5353 cannot be used (%d); this Server is found by its address alone", WSAGetLastError());
    const auto group = groupAddress();
    for (const auto& address : localAddresses()) {
        ip_mreq membership{};
        membership.imr_multiaddr = group.sin_addr;
        membership.imr_interface = address;
        setsockopt(impl_->socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&membership), sizeof(membership));
    }
    impl_->thread = std::jthread([impl = impl_.get()](const std::stop_token& stop) { impl->run(stop); });
}

ServerAnnouncer::~ServerAnnouncer() {
    impl_->thread = {};
    closesocket(impl_->socket);
}

std::vector<FoundServer> findServers(std::chrono::milliseconds wait) {
    WinsockScope winsock;
    const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in any{};
    any.sin_family = AF_INET;
    if (bind(s, reinterpret_cast<sockaddr*>(&any), sizeof(any)) != 0) {
        closesocket(s);
        return {};
    }
    sendToGroup(s, airplay::queryMessage({{airplay::serviceName(discovery::kService), airplay::DnsType::Ptr}}));
    std::vector<std::string> messages;
    const auto deadline = std::chrono::steady_clock::now() + wait;
    for (auto now = std::chrono::steady_clock::now(); now < deadline; now = std::chrono::steady_clock::now()) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(s, &readable);
        if (!waitReadable(readable, std::chrono::duration_cast<std::chrono::microseconds>(deadline - now))) continue;
        char buffer[9000];
        const int got = recv(s, buffer, int(sizeof(buffer)), 0);
        if (got > 0) messages.emplace_back(buffer, size_t(got));
    }
    closesocket(s);
    return discovery::serversIn(messages);
}

} // namespace camel::server
