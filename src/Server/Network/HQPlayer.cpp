// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Network/HQPlayer.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <condition_variable>
#include <cstring>
#include <format>
#include <mutex>
#include <thread>
#include <vector>

#include "Common/AudioTypes.h"
#include "Common/util/Log.h"
#include "Server/Network/LocalAddresses.h"
#include "Server/Network/SocketWait.h"

#pragma comment(lib, "ws2_32.lib")

namespace camel::server::hqplayer {

namespace {

using Clock = std::chrono::steady_clock;
using audio::Error;
using audio::ErrorCode;

constexpr SOCKET kNoSocket = INVALID_SOCKET;
constexpr auto kPollSockets = std::chrono::milliseconds(100);
constexpr std::string_view kDeclaration = "<?xml";

bool equalsNoCase(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) { return std::tolower(uint8_t(x)) == std::tolower(uint8_t(y)); });
}

double number(const upnp::XmlNode& node, const char* name) {
    const auto found = node.attributes.find(name);
    if (found == node.attributes.end()) return 0.0;
    double value = 0.0;
    std::from_chars(found->second.data(), found->second.data() + found->second.size(), value);
    return value;
}

std::string text(const upnp::XmlNode& node, const char* name) {
    const auto found = node.attributes.find(name);
    return found == node.attributes.end() ? std::string() : found->second;
}

// Where a whole document ends in `buffer`, which starts with one; npos while it is still arriving.
size_t documentEnd(std::string_view buffer) {
    const size_t declarationEnd = buffer.find("?>");
    if (declarationEnd == std::string_view::npos) return std::string_view::npos;
    int depth = 0;
    bool rootSeen = false;
    size_t at = declarationEnd + 2;
    while ((at = buffer.find('<', at)) != std::string_view::npos) {
        if (at + 1 >= buffer.size()) return std::string_view::npos;
        const char kind = buffer[at + 1];
        if (kind == '?' || kind == '!') {
            const size_t end = buffer.find('>', at);
            if (end == std::string_view::npos) return std::string_view::npos;
            at = end + 1;
            continue;
        }
        char quote = 0;
        size_t end = at + 1;
        for (; end < buffer.size(); ++end) {
            const char c = buffer[end];
            if (quote) {
                if (c == quote) quote = 0;
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '>') {
                break;
            }
        }
        if (end >= buffer.size()) return std::string_view::npos;
        if (kind == '/')
            --depth;
        else if (buffer[end - 1] != '/')
            ++depth;
        rootSeen = true;
        at = end + 1;
        if (rootSeen && depth <= 0) return at;
    }
    return std::string_view::npos;
}

std::string elementName(std::string_view command) {
    const size_t start = command.find('<');
    if (start == std::string_view::npos) return {};
    const size_t end = command.find_first_of(" />\t\r\n", start + 1);
    return std::string(command.substr(start + 1, end == std::string_view::npos ? std::string_view::npos : end - start - 1));
}

struct Resolved {
    std::vector<sockaddr_storage> addresses;
    std::vector<int> lengths;
};

Resolved resolve(const std::string& host, uint16_t port) {
    Resolved resolved;
    const std::string name = equalsNoCase(host, "localhost") ? std::string("127.0.0.1") : host;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* found = nullptr;
    if (getaddrinfo(name.c_str(), std::to_string(port).c_str(), &hints, &found) != 0) return resolved;
    for (const addrinfo* a = found; a; a = a->ai_next) {
        sockaddr_storage address{};
        std::memcpy(&address, a->ai_addr, std::min(sizeof(address), size_t(a->ai_addrlen)));
        resolved.addresses.push_back(address);
        resolved.lengths.push_back(int(a->ai_addrlen));
    }
    freeaddrinfo(found);
    return resolved;
}

enum class Connected : uint8_t { Yes, Refused, TimedOut, NoAddress };

// A connection within `timeout`, to the first of the host's addresses that takes it.
Connected connectTo(const std::string& host, uint16_t port, std::chrono::milliseconds timeout, SOCKET& out) {
    const auto resolved = resolve(host, port);
    if (resolved.addresses.empty()) return Connected::NoAddress;
    bool refused = false;
    for (size_t i = 0; i < resolved.addresses.size(); ++i) {
        const auto& address = resolved.addresses[i];
        const SOCKET s = socket(address.ss_family, SOCK_STREAM, IPPROTO_TCP);
        if (s == kNoSocket) continue;
        u_long nonBlocking = 1;
        ioctlsocket(s, FIONBIO, &nonBlocking);
        if (connect(s, reinterpret_cast<const sockaddr*>(&address), resolved.lengths[i]) == SOCKET_ERROR &&
            WSAGetLastError() != WSAEWOULDBLOCK) {
            refused = refused || WSAGetLastError() == WSAECONNREFUSED;
            closesocket(s);
            continue;
        }
        fd_set writable, failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(s, &writable);
        FD_SET(s, &failed);
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(timeout).count();
        const timeval wait{long(us / 1000000), long(us % 1000000)};
        const int ready = select(0, nullptr, &writable, &failed, &wait);
        if (ready > 0 && FD_ISSET(s, &writable)) {
            nonBlocking = 0;
            ioctlsocket(s, FIONBIO, &nonBlocking);
            const BOOL on = TRUE;
            setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&on), sizeof(on));
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
            out = s;
            return Connected::Yes;
        }
        if (ready > 0) refused = true;
        closesocket(s);
    }
    return refused ? Connected::Refused : Connected::TimedOut;
}

struct WinsockScope {
    WinsockScope() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    ~WinsockScope() { WSACleanup(); }
    WinsockScope(const WinsockScope&) = delete;
    WinsockScope& operator=(const WinsockScope&) = delete;
};

} // namespace

std::string addressOf(std::string_view host, uint16_t port) {
    if (host.find(':') != std::string_view::npos) return std::format("[{}]:{}", host, port);
    return std::format("{}:{}", host, port);
}

bool splitAddress(std::string_view address, std::string& host, uint16_t& port) {
    size_t colon = 0;
    if (address.starts_with('[')) {
        const size_t close = address.find(']');
        if (close == std::string_view::npos || close + 1 >= address.size() || address[close + 1] != ':') return false;
        host = std::string(address.substr(1, close - 1));
        colon = close + 1;
    } else {
        colon = address.rfind(':');
        if (colon == std::string_view::npos) return false;
        host = std::string(address.substr(0, colon));
    }
    unsigned value = 0;
    const auto digits = address.substr(colon + 1);
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (host.empty() || error != std::errc() || end != digits.data() + digits.size() || value == 0 || value > 65535) return false;
    port = uint16_t(value);
    return true;
}

bool isLocalHost(std::string_view host) {
    if (equalsNoCase(host, "localhost") || host == "::1") return true;
    in_addr address{};
    const std::string text(host);
    if (inet_pton(AF_INET, text.c_str(), &address) != 1) return false;
    if ((ntohl(address.s_addr) >> 24) == 127) return true;
    return std::ranges::any_of(localAddresses(), [&](const in_addr& local) { return local.s_addr == address.s_addr; });
}

std::string ipv4Of(const std::string& host) {
    WinsockScope winsock;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || !found) return {};
    char text[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(found->ai_addr)->sin_addr, text, sizeof(text));
    freeaddrinfo(found);
    return text;
}

std::vector<std::string> takeDocuments(std::string& buffer) {
    std::vector<std::string> documents;
    while (true) {
        const size_t start = buffer.find(kDeclaration);
        if (start == std::string::npos) {
            const size_t partial = buffer.rfind('<');
            buffer.erase(0, partial == std::string::npos ? buffer.size() : partial);
            break;
        }
        buffer.erase(0, start);
        const size_t end = documentEnd(buffer);
        if (end == std::string::npos) break;
        documents.push_back(buffer.substr(0, end));
        buffer.erase(0, end);
    }
    if (buffer.size() > upnp::kMaxMessageBytes) buffer.clear();
    return documents;
}

std::string escape(std::string_view value) {
    std::string out;
    for (const char c : value) {
        if (c == '&')
            out += "&amp;";
        else if (c == '<')
            out += "&lt;";
        else if (c == '>')
            out += "&gt;";
        else if (c == '"')
            out += "&quot;";
        else
            out += c;
    }
    return out;
}

Status parseStatus(const upnp::XmlNode& element) {
    Status s;
    s.state = int(number(element, "state"));
    s.track = uint32_t(number(element, "track"));
    s.trackSerial = uint32_t(number(element, "track_serial"));
    s.transportSerial = uint32_t(number(element, "transport_serial"));
    s.tracksTotal = uint32_t(number(element, "tracks_total"));
    s.position = number(element, "position");
    s.length = number(element, "length");
    s.volume = number(element, "volume");
    s.activeRate = uint32_t(number(element, "active_rate"));
    s.activeBits = uint32_t(number(element, "active_bits"));
    s.activeMode = text(element, "active_mode");
    s.activeFilter = text(element, "active_filter");
    s.activeShaper = text(element, "active_shaper");
    return s;
}

std::string discoverRequest() {
    return R"(<?xml version="1.0"?><discover>hqplayer</discover>)";
}

std::optional<Found> parseDiscoverReply(std::string_view datagram, const std::string& host, uint16_t port) {
    const auto root = upnp::parseXml(datagram);
    if (!root) return std::nullopt;
    const auto& reply = *root;
    if (reply.name != "discover" || text(reply, "result") != "OK" || reply.text != "hqplayer") return std::nullopt;
    return Found{addressOf(isLocalHost(host) ? "localhost" : host, port), text(reply, "name"), text(reply, "version")};
}

struct Client::Impl {
    WinsockScope winsock;
    std::string address, host;
    uint16_t port = kDefaultPort;
    std::function<void(const Status&)> onStatus;
    std::function<void()> onLost;
    ClientOptions options;

    std::mutex commandMutex; // one command at a time, a connection made under it
    mutable std::mutex mutex;
    std::condition_variable answered;
    SOCKET sock = kNoSocket;
    bool broken = false, closing = false;
    std::string waitingFor;
    std::optional<upnp::XmlNode> reply;
    std::optional<Status> last;
    std::atomic<uint64_t> made = 0;
    bool established = false; // the connection has answered, so losing it is news
    std::thread reader;

    Impl(std::string target, std::function<void(const Status&)> statusHandler, std::function<void()> lostHandler,
         ClientOptions clientOptions)
        : address(std::move(target)), onStatus(std::move(statusHandler)), onLost(std::move(lostHandler)), options(clientOptions) {
        if (!splitAddress(address, host, port)) host = address;
    }

    void read(SOCKET s) {
        std::string buffer;
        char chunk[16384];
        while (true) {
            {
                std::scoped_lock lock(mutex);
                if (closing) return;
            }
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(s, &readable);
            if (!waitReadable(readable, kPollSockets)) continue;
            const int length = recv(s, chunk, sizeof(chunk), 0);
            if (length <= 0) {
                bool news = false;
                {
                    std::scoped_lock lock(mutex);
                    news = established && !closing;
                    broken = true;
                }
                answered.notify_all();
                if (news) {
                    log::warn("HQPlayer at %s: the connection closed", address.c_str());
                    if (onLost) onLost();
                }
                return;
            }
            buffer.append(chunk, size_t(length));
            for (const auto& document : takeDocuments(buffer)) {
                const auto root = upnp::parseXml(document);
                if (!root) continue;
                const auto& element = *root;
                std::optional<Status> pushed;
                {
                    std::scoped_lock lock(mutex);
                    if (element.name == "Status") last = pushed = parseStatus(element);
                    if (!waitingFor.empty() && element.name == waitingFor && !reply) reply = element;
                }
                answered.notify_all();
                if (pushed && onStatus) onStatus(*pushed);
            }
        }
    }

    _Requires_lock_held_(commandMutex) void close() {
        {
            std::scoped_lock lock(mutex);
            closing = true;
        }
        if (reader.joinable()) reader.join();
        std::scoped_lock lock(mutex);
        if (sock != kNoSocket) closesocket(sock);
        sock = kNoSocket;
        broken = closing = established = false;
        waitingFor.clear();
        reply.reset();
    }

    _Requires_lock_held_(commandMutex) std::optional<upnp::XmlNode> transact(const std::string& command,
                                                                             std::chrono::milliseconds limit) {
        std::unique_lock lock(mutex);
        if (sock == kNoSocket || broken) return std::nullopt;
        waitingFor = elementName(command);
        reply.reset();
        const std::string document = R"(<?xml version="1.0"?>)" + command + "\n";
        const SOCKET s = sock;
        lock.unlock();
        if (::send(s, document.data(), int(document.size()), 0) != int(document.size())) return std::nullopt;
        lock.lock();
        answered.wait_for(lock, limit, [&] { return reply.has_value() || broken; });
        waitingFor.clear();
        auto answer = std::move(reply);
        reply.reset();
        return answer;
    }

    _Requires_lock_held_(commandMutex) bool open() {
        {
            std::scoped_lock lock(mutex);
            if (sock != kNoSocket && !broken) return false;
        }
        close();
        const bool local = isLocalHost(host);
        SOCKET s = kNoSocket;
        switch (connectTo(host, port, options.connectTimeout, s)) {
        case Connected::Yes:
            break;
        case Connected::NoAddress:
            throw Error(ErrorCode::DeviceNotFound, std::format("HQPlayer at {} cannot be found on the network", address));
        case Connected::Refused:
            if (!local)
                throw Error(ErrorCode::AccessDenied,
                            std::format("HQPlayer at {} refused the connection: turn on \"Allow control from network\" in "
                                        "HQPlayer",
                                        address));
            throw Error(ErrorCode::DeviceNotFound, std::format("HQPlayer is not running at {}", address));
        case Connected::TimedOut:
            throw Error(ErrorCode::DeviceNotFound, std::format("HQPlayer at {} does not answer: is it running?", address));
        }
        {
            std::scoped_lock lock(mutex);
            sock = s;
        }
        reader = std::thread([this, s] { read(s); });
        if (!transact("<GetInfo/>", options.firstReply)) {
            close();
            if (!local)
                throw Error(ErrorCode::AccessDenied,
                            std::format("HQPlayer at {} took the connection but did not answer: turn on \"Allow control "
                                        "from network\" in HQPlayer",
                                        address));
            throw Error(ErrorCode::DeviceNotFound, std::format("HQPlayer at {} did not answer", address));
        }
        {
            std::scoped_lock lock(mutex);
            established = true;
        }
        ++made;
        // A restarted HQPlayer keeps its last playlist and fetches its URLs.
        std::vector<const char*> first{R"(<Status subscribe="1"/>)"};
        if (options.clearOnConnect) first.insert(first.begin(), {"<Stop/>", "<PlaylistClear/>"});
        for (const char* command : first)
            if (!transact(command, options.reply)) {
                close();
                throw Error(ErrorCode::DeviceNotFound, std::format("HQPlayer at {} stopped answering", address));
            }
        log::info("HQPlayer at %s: connected", address.c_str());
        return true;
    }
};

Client::Client(std::string address, std::function<void(const Status&)> onStatus, std::function<void()> onLost,
               ClientOptions options)
    : impl_(std::make_unique<Impl>(std::move(address), std::move(onStatus), std::move(onLost), options)) {}

Client::~Client() {
    disconnect();
}

void Client::connect() {
    std::scoped_lock command(impl_->commandMutex);
    impl_->open();
}

upnp::XmlNode Client::send(const std::string& command) {
    std::scoped_lock commandLock(impl_->commandMutex);
    for (int attempt = 0; attempt < 2; ++attempt) {
        impl_->open();
        if (auto reply = impl_->transact(command, impl_->options.reply)) return std::move(*reply);
        log::warn("HQPlayer at %s: no reply to %s", impl_->address.c_str(), elementName(command).c_str());
        impl_->close();
    }
    throw Error(ErrorCode::DeviceNotFound, std::format("HQPlayer at {} is not connected", impl_->address));
}

bool Client::ok(const std::string& command) {
    const auto reply = send(command);
    const auto result = reply.attributes.find("result");
    return result != reply.attributes.end() && result->second == "OK";
}

void Client::disconnect() {
    std::scoped_lock command(impl_->commandMutex);
    impl_->close();
}

bool Client::connected() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->sock != kNoSocket && !impl_->broken && impl_->established;
}

std::optional<Status> Client::status() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->last;
}

uint64_t Client::connections() const {
    return impl_->made;
}

struct Finder::Impl {
    struct Known {
        Found found;
        Clock::time_point seen;
    };

    WinsockScope winsock;
    FinderOptions options;
    std::function<void()> onChange;
    SOCKET searcher = kNoSocket;
    mutable std::mutex mutex;
    std::vector<Known> known;
    std::vector<std::string> watched, listening;
    std::atomic<bool> stopping = false, searchWanted = true;
    std::thread thread;

    Impl(FinderOptions finderOptions, std::function<void()> changed)
        : options(std::move(finderOptions)), onChange(std::move(changed)) {
        searcher = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        if (bind(searcher, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
            log::warn("HQPlayer: cannot search (%d)", WSAGetLastError());
        const DWORD loop = 1;
        setsockopt(searcher, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&loop), sizeof(loop));
        thread = std::thread([this] { run(); });
    }

    ~Impl() {
        stopping = true;
        thread.join();
        closesocket(searcher);
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void sendSearch() {
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(options.port);
        inet_pton(AF_INET, options.group.c_str(), &target.sin_addr);
        const std::string request = discoverRequest();
        const auto send = [&] {
            sendto(searcher, request.data(), int(request.size()), 0, reinterpret_cast<const sockaddr*>(&target), sizeof(target));
        };
        send();
        if ((ntohl(target.sin_addr.s_addr) >> 28) != 0xE) return; // a test's unicast address
        for (const auto& local : localAddresses()) {
            setsockopt(searcher, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&local), sizeof(local));
            send();
        }
    }

    void receive() {
        char buffer[4096];
        sockaddr_in from{};
        int fromLength = sizeof(from);
        const int length = recvfrom(searcher, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &fromLength);
        if (length <= 0) return;
        char host[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &from.sin_addr, host, sizeof(host));
        const auto found = parseDiscoverReply(std::string_view(buffer, size_t(length)), host, ntohs(from.sin_port));
        if (!found) return;
        bool changed = false;
        {
            std::scoped_lock lock(mutex);
            const auto same = std::ranges::find(known, found->address, [](const Known& k) { return k.found.address; });
            if (same == known.end()) {
                known.push_back({*found, Clock::now()});
                changed = true;
            } else {
                changed = same->found != *found;
                same->found = *found;
                same->seen = Clock::now();
            }
        }
        if (changed && onChange) onChange();
    }

    void forgetSilent() {
        size_t removed = 0;
        {
            std::scoped_lock lock(mutex);
            const auto limit = Clock::now() - options.forgetAfter;
            removed = std::erase_if(known, [&](const Known& k) { return k.seen < limit; });
        }
        if (removed && onChange) onChange();
    }

    void probeWatched() {
        std::vector<std::string> addresses;
        {
            std::scoped_lock lock(mutex);
            addresses = watched;
        }
        if (options.localPort != 0) {
            const std::string local = addressOf("localhost", options.localPort);
            if (std::ranges::find(addresses, local) == addresses.end()) addresses.push_back(local);
        }
        std::vector<std::string> answering;
        for (const auto& address : addresses)
            if (!stopping && reachable(address, options.probeTimeout)) answering.push_back(address);
        bool changed = false;
        {
            std::scoped_lock lock(mutex);
            changed = listening != answering;
            listening = std::move(answering);
        }
        if (changed && onChange) onChange();
    }

    void run() {
        auto nextSearch = Clock::now();
        while (!stopping) {
            if (searchWanted.exchange(false) || Clock::now() >= nextSearch) {
                forgetSilent();
                sendSearch();
                probeWatched();
                nextSearch = Clock::now() + options.searchInterval;
            }
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(searcher, &readable);
            if (waitReadable(readable, kPollSockets)) receive();
        }
    }
};

Finder::Finder(FinderOptions options, std::function<void()> onChange)
    : impl_(std::make_unique<Impl>(std::move(options), std::move(onChange))) {}

Finder::~Finder() = default;

std::vector<Found> Finder::found() const {
    std::scoped_lock lock(impl_->mutex);
    std::vector<Found> list;
    list.reserve(impl_->known.size());
    for (const auto& k : impl_->known) list.push_back(k.found);
    for (const auto& address : impl_->listening)
        if (std::ranges::find(list, address, &Found::address) == list.end()) list.push_back({address, {}, {}});
    return list;
}

void Finder::search() {
    impl_->searchWanted = true;
}

void Finder::watch(std::vector<std::string> addresses) {
    std::scoped_lock lock(impl_->mutex);
    if (impl_->watched == addresses) return;
    impl_->watched = std::move(addresses);
    const std::string local = impl_->options.localPort ? addressOf("localhost", impl_->options.localPort) : std::string();
    std::erase_if(impl_->listening, [&](const std::string& a) {
        return a != local && std::ranges::find(impl_->watched, a) == impl_->watched.end();
    });
    impl_->searchWanted = true;
}

bool reachable(const std::string& address, std::chrono::milliseconds timeout) {
    WinsockScope winsock;
    std::string host;
    uint16_t port = kDefaultPort;
    if (!splitAddress(address, host, port)) return false;
    SOCKET s = kNoSocket;
    if (connectTo(host, port, timeout, s) != Connected::Yes) return false;
    closesocket(s);
    return true;
}

} // namespace camel::server::hqplayer
