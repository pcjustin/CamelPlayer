// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Remote/RemoteServer.h"

#include <ws2tcpip.h>

#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>

#include "Common/util/Log.h"
#include "Common/util/Text.h"
#include "Server/Api/ApiCalls.h"
#include "Server/Api/ApiJson.h"
#include "Server/Api/ApiTypesJson.h"
#include "Server/Artwork/ArtworkCache.h"
#include "Server/Integration/DiscordPresence.h"
#include "Server/Network/SocketWait.h"
#include "Server/Playback/Outputs.h"
#include "Server/Remote/Connection.h"
#include "Server/Remote/Discovery.h"
#include "Server/Remote/Pairings.h"
#include "Server/Remote/RemoteAccess.h"

namespace camel::server {

namespace {

constexpr size_t kHeldEvents = 64; // events kept for a listener that falls behind, the oldest going first

} // namespace

using nlohmann::json;
using namespace remote;

namespace {

constexpr SOCKET kNoSocket = INVALID_SOCKET;
constexpr size_t kMaxMessage = size_t{256} * 1024 * 1024; // a first snapshot carries every queue whole
constexpr size_t kMaxHead = size_t{16} * 1024;
constexpr auto kTransferTimeout = std::chrono::seconds(120);

template <class... A> json args(const A&... values) {
    return json::array({json(values)...});
}

template <class R> R as(const json& j) {
    try {
        return j.get<R>();
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("Server API: an answer that does not fit: ") + e.what());
    }
}

} // namespace

struct RemoteServer::Impl {
    struct Reply {
        bool done = false;
        json result, error;
    };
    struct HttpReply {
        int status = 0;
        websocket::HttpHead head;
        std::string body;
    };

    explicit Impl(Options o) : options(std::move(o)), host(options.host), port(options.port) {
        if (!options.ownSettings.empty()) own = loadSettings(options.ownSettings);
    }

    Options options;
    WinsockScope winsock;
    std::thread reader;
    // The listener hears events on a thread of its own, so it may call the Server back as it may on the Server itself;
    // the reader would otherwise wait for itself to read the answer.
    std::thread dispatcher;
    std::deque<Event> events; // under mutex, not yet told
    bool telling = false;     // under mutex: the listener is being told one
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::shared_ptr<Connection> connection; // replaced when the connection is made again
    uint64_t generation = 0;                // connections made so far; a call answers only on its own
    std::string host;                       // where the Server was last reached, which mDNS may find anew
    uint16_t port = 0;
    std::map<uint64_t, Reply> pending;
    uint64_t lastCall = 0;
    uint64_t lastSubmitted = 0; // the command this client submitted last, which waitIdle() waits to see done
    bool connected = false, refused = false, stopping = false;
    std::optional<bool> serverOlder;      // they share no API version: whether the Server is the one to update
    std::optional<Snapshot> current, raw; // as shown, and as the Server sent it
    std::function<void(const Event&)> listener;
    std::function<void(const json&)> noticeListener;
    json hello;
    api::QueueDecoder decoder;         // the reader's alone
    std::optional<Settings> own;       // a Control point's own settings, with options.ownSettings
    std::optional<SettingsDto> theirs; // the Server's, as it last sent them
    std::string following;             // the zone this Control point follows
    uint64_t followSerial = 0;         // follow calls sent, for the Server to keep the last
    bool told = false;                 // this connection's Server knows the zone followed
    uint64_t transfers = UINT64_MAX;   // the last Transfer to… taken into account; none before the first snapshot
    // Covers fetched before, by what was asked, with the library's revision then: one asked for again while the library is
    // as it was is read from the disk cache with no request (docs/Plan.md §17.8). Albums' and folders' only, which change
    // with the library; an artist's picture or a playing file's can change without it.
    struct KeptCover {
        std::string key;
        uint64_t library = 0;
    };
    std::mutex coversMutex;
    std::map<std::string, KeptCover> covers;
    std::atomic<uint64_t> coversWritten = 0; // names each partial cover file, so that no two askers write the same one
    std::atomic<uint64_t> library = 0;       // the library's revision, as the last page showed it
    // A Control point's own Discord, from the zone it follows; last, so it goes first, its cover loads with it.
    std::unique_ptr<DiscordPresence> discord;

    template <class Page> Page sawLibrary(Page page) {
        library = page.revision;
        return page;
    }

    // The Control point's Discord shows on its own computer: the zone it follows, its own settings and its own cover
    // tunnel, the covers coming from the Server (docs/Plan.md §17.8).
    void startDiscord() {
        if (!own) return;
        discord = std::make_unique<DiscordPresence>(
            options.ownSettings.parent_path() / L"tools" / L"cloudflared.exe",
            [this](const std::string& path, const CancellationToken& token) -> std::vector<uint8_t> {
                if (token.cancelled()) return {};
                const auto reply = http("GET", "/artwork?kind=" + json(ArtworkKind::Track).get<std::string>() +
                                                   "&id=0&path=" + websocket::percentEncode(path) + "&name=&pixels=512");
                if (reply.status != 200 || reply.body.empty()) return {};
                return squarePicture(std::vector<uint8_t>(reply.body.begin(), reply.body.end()), 512);
            },
            options.discord);
        discord->configure(own->discord);
    }

    // The settings shown: the Server's, with what belongs to this computer from its own.
    SettingsDto shown(const SettingsDto& server) const {
        auto settings = nativeSettings(server);
        copyComputerSettings(*own, settings);
        return settingsDto(settings);
    }

    static const ZoneSnapshot* zoneOf(const Snapshot& s, const std::string& id) {
        const auto found = std::ranges::find(s.zones, id, &ZoneSnapshot::id);
        return found == s.zones.end() ? nullptr : &*found;
    }

    // The zone a Control point follows (docs/Plan.md §17.8): its own choice while it is there, the queue it followed
    // after a Transfer to…, the group its zone joined, else the zone playing that started last, else the one played last.
    std::string chooseZone(const Snapshot& s) {
        if (s.transfer.serial != transfers) {
            if (transfers != UINT64_MAX && s.transfer.from == following) following = s.transfer.to;
            transfers = s.transfer.serial;
        }
        if (!following.empty() && zoneOf(s, following)) return following;
        for (const auto& zone : s.zones)
            if (std::ranges::any_of(zone.playback.receivers,
                                    [&](const ReceiverState& r) { return zoneId(Backend::AirPlay, r.id) == following; }))
                return zone.id;
        const ZoneSnapshot* best = nullptr;
        for (const auto& zone : s.zones) {
            const bool playing = zone.playback.state == PlaybackState::Playing;
            const bool bestPlaying = best && best->playback.state == PlaybackState::Playing;
            if (!best || playing > bestPlaying || (playing == bestPlaying && zone.played > best->played)) best = &zone;
        }
        return best && best->played ? best->id : s.selectedZone;
    }

    // The Server's state as this Control point shows it: its own settings, and the zone it follows as the selected
    // one, its output the one in use. Under the mutex.
    Snapshot project(Snapshot s) {
        if (!own) return s;
        Settings& mine = *own;
        const auto chosen = chooseZone(s);
        if (chosen != following || !told) {
            following = chosen;
            if (mine.followedZone != chosen) {
                mine.followedZone = chosen;
                saveQuietly();
            }
            tellFollowing();
            told = true;
        }
        if (const ZoneSnapshot* zone = zoneOf(s, following); zone && following != s.selectedZone) {
            s.selectedZone = zone->id;
            s.queueEntryId = zone->queueEntryId;
            s.track = zone->track;
            s.queue = zone->queue;
            s.playback = zone->playback;
            auto settings = nativeSettings(s.settings);
            selectOutput(settings, zone->backend, zone->output, s.devices);
            s.settings = settingsDto(settings);
        }
        s.settings = shown(s.settings);
        return s;
    }

    // A Control point's settings on their way to the Server (docs/Plan.md §17.8): what belongs to this computer kept
    // here, a zone chosen in them followed, and the Server's own output in use put back. Under the mutex.
    void toServer(Command& command) {
        if (!own || !command.settings) return;
        Settings& mine = *own;
        auto wanted = nativeSettings(*command.settings);
        copyComputerSettings(wanted, mine);
        if (raw) {
            const Snapshot& server = *raw;
            const auto chosen = zoneId(wanted.backend, outputInUse(wanted));
            const ZoneSnapshot* serverZone = zoneOf(server, server.selectedZone);
            if (zoneOf(server, chosen)) {
                if (chosen != following) {
                    following = chosen;
                    mine.followedZone = chosen;
                    tellFollowing();
                }
                if (serverZone && chosen != server.selectedZone) {
                    selectOutput(wanted, serverZone->backend, serverZone->output, server.devices);
                    wanted.airplayGroup = nativeSettings(server.settings).airplayGroup;
                }
            }
        }
        saveQuietly();
        if (discord) discord->configure(mine.discord);
        if (theirs) copyComputerSettings(nativeSettings(*theirs), wanted);
        command.settings = settingsDto(wanted);
        if (raw) current = project(*raw);
    }

    void saveQuietly() const {
        if (!own) return;
        try {
            saveSettings(*own, options.ownSettings);
        } catch (const std::exception& e) {
            log::warn("Server API: this computer's settings: %s", e.what());
        }
    }

    // Tells the Server which zone this Control point follows, for its list of paired devices; not waited for.
    void tellFollowing() {
        if (!connected || !connection) return;
        try {
            connection->send(
                json{{"id", ++lastCall}, {"call", "follow"}, {"args", json::array({following, ++followSerial})}}.dump());
        } catch (const std::exception&) {
            // The connection is gone; it says so again once it is back.
        }
    }

    std::string authorization() const {
        return options.token.empty() ? std::string() : "Authorization: Bearer " + options.token + "\r\n";
    }

    std::pair<std::string, uint16_t> address() const {
        std::lock_guard lock(mutex);
        return {host, port};
    }

    SOCKET connect() const {
        const auto [to, at] = address();
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* found = nullptr;
        if (getaddrinfo(to.c_str(), std::to_string(at).c_str(), &hints, &found) != 0 || !found)
            throw std::runtime_error("Server API: " + to + " is not found");
        const SOCKET s = socket(found->ai_family, found->ai_socktype, found->ai_protocol);
        const auto reach =
            s == kNoSocket ? Reach::Failed : connectWithin(s, found->ai_addr, int(found->ai_addrlen), options.timeout);
        freeaddrinfo(found);
        if (reach != Reach::Connected) {
            if (s != kNoSocket) closesocket(s);
            throw std::runtime_error("Server API: " + to + " does not answer");
        }
        u_long blocking = 0;
        ioctlsocket(s, FIONBIO, &blocking);
        return s;
    }

    void open() {
        const SOCKET s = connect();
        const auto [to, at] = address();
        std::array<uint8_t, 16> nonce{};
        std::mt19937 generator{std::random_device{}()};
        for (auto& byte : nonce) byte = uint8_t(generator());
        const auto key = websocket::base64(nonce);
        std::string rest;
        const bool sent = sendAll(s, "GET /api HTTP/1.1\r\nHost: " + to + ":" + std::to_string(at) +
                                         "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
                                         "\r\nSec-WebSocket-Version: 13\r\n" + authorization() + "\r\n");
        const auto head = sent ? readHead(s, rest, kMaxHead, options.timeout) : std::nullopt;
        if (!head) {
            closesocket(s);
            throw std::runtime_error("Server API: the Server did not answer the connection");
        }
        const auto answer = websocket::parseHead(*head);
        if (answer.second == "401") {
            closesocket(s);
            throw NotPaired();
        }
        if (answer.second != "101" || answer.header("sec-websocket-accept") != websocket::acceptKey(key)) {
            closesocket(s);
            throw std::runtime_error("Server API: the Server refused the connection (" + answer.second + ")");
        }
        auto made = std::make_shared<Connection>(s, true, std::move(rest));
        std::lock_guard lock(mutex);
        connection = std::move(made);
        ++generation;
        connected = true;
        told = false;
        decoder = {};
    }

    // Reads the connection until it ends, then makes it again until the Server answers (docs/Plan.md §17.8): every two
    // seconds at the address it had, and through mDNS by its ID from time to time; never again once it refuses this
    // device.
    void run() {
        for (int attempt = 0;; ++attempt) {
            if (attempt == 0 || reconnect(attempt)) readAll();
            std::unique_lock lock(mutex);
            if (stopping || refused) return;
            changed.wait_for(lock, std::chrono::seconds(2), [this] { return stopping; });
            if (stopping) return;
        }
    }

    bool reconnect(int attempt) {
        try {
            open();
            std::lock_guard lock(mutex);
            if (stopping) {
                connection->shut();
                return false;
            }
            log::info("Server API: connected to the Server again");
            return true;
        } catch (const NotPaired&) {
            log::warn("Server API: the Server no longer knows this device");
            std::lock_guard lock(mutex);
            refused = true;
        } catch (const std::exception&) {
            if (attempt % 5 == 0) findAgain();
        }
        changed.notify_all();
        return false;
    }

    void findAgain() {
        std::string id;
        {
            std::lock_guard lock(mutex);
            id = hello.contains("server") ? hello["server"].value("id", std::string()) : std::string();
        }
        if (id.empty()) return;
        for (const auto& found : findServers(std::chrono::milliseconds(1500)))
            if (found.announced.id == id) {
                std::lock_guard lock(mutex);
                host = found.address;
                port = found.announced.port;
                if (own) {
                    own->controlled.host = host;
                    own->controlled.port = port;
                    saveQuietly();
                }
            }
    }

    void readAll() {
        std::shared_ptr<Connection> reading;
        {
            std::lock_guard lock(mutex);
            reading = connection;
        }
        try {
            while (const auto message = reading->read(kMaxMessage)) handle(*message);
        } catch (const std::exception& e) {
            log::warn("Server API: the connection ended: %s", e.what());
        }
        reading->shut(); // the Server's side waits for this one to close
        {
            std::lock_guard lock(mutex);
            connected = false;
        }
        changed.notify_all();
    }

    void handle(const std::string& text) {
        json j = json::parse(text);
        if (j.contains("hello")) {
            {
                std::lock_guard lock(mutex);
                hello = j["hello"];
                const auto api = hello.value("api", json::object());
                if (options.apiVersion > api.value("max", 0U))
                    serverOlder = true;
                else if (options.apiVersion < api.value("min", 0U))
                    serverOlder = false;
                if (serverOlder) refused = true;
            }
            changed.notify_all();
            if (serverOlder) throw std::runtime_error("Server API: no version in common");
            return;
        }
        if (j.contains("notice")) {
            std::function<void(const json&)> tell;
            {
                std::lock_guard lock(mutex);
                tell = noticeListener;
            }
            if (tell) tell(j["notice"]);
            return;
        }
        if (j.contains("event")) {
            decoder.decode(j.at("snapshot"));
            auto snapshot = snapshotFromJson(j["snapshot"]);
            const auto kind = j["event"] == "commandCompleted" ? EventKind::CommandCompleted : EventKind::StateChanged;
            {
                std::lock_guard lock(mutex);
                theirs = snapshot.settings;
                raw = snapshot;
                snapshot = project(std::move(snapshot));
                current = snapshot;
                if (listener) {
                    if (events.size() >= kHeldEvents) events.pop_front();
                    events.push_back(Event{kind, snapshot});
                }
            }
            changed.notify_all();
            if (discord) discord->observe(snapshot);
            return;
        }
        const auto id = j.at("id").get<uint64_t>();
        {
            std::lock_guard lock(mutex);
            const auto found = pending.find(id);
            if (found == pending.end()) return;
            found->second.done = true;
            if (j.contains("error"))
                found->second.error = j["error"];
            else
                found->second.result = j.contains("result") ? j["result"] : json();
        }
        changed.notify_all();
    }

    // Tells the listener each event in turn; one that throws is logged, as the Server's own listener's failures are.
    void dispatch() {
        std::unique_lock lock(mutex);
        for (;;) {
            changed.wait(lock, [&] { return stopping || !events.empty(); });
            if (stopping) return;
            const Event event = std::move(events.front());
            events.pop_front();
            const auto tell = listener;
            telling = true;
            lock.unlock();
            try {
                if (tell) tell(event);
            } catch (const std::exception& e) {
                log::warn("Server API: the listener: %s", e.what());
            }
            lock.lock();
            telling = false;
            changed.notify_all();
        }
    }

    json call(const std::string& name, json arguments, const CancellationToken& token) {
        uint64_t id = 0, made = 0;
        std::shared_ptr<Connection> sending;
        {
            std::lock_guard lock(mutex);
            if (!connected) throw std::runtime_error("Server API: not connected to the Server");
            id = ++lastCall;
            made = generation;
            sending = connection;
            pending[id];
        }
        try {
            sending->send(json{{"id", id}, {"call", name}, {"args", std::move(arguments)}}.dump(-1, ' ', false,
                                                                                                json::error_handler_t::replace));
        } catch (const std::exception&) {
            std::lock_guard lock(mutex);
            pending.erase(id);
            throw std::runtime_error("Server API: the connection to the Server was lost");
        }
        bool cancelSent = false;
        std::unique_lock lock(mutex);
        for (;;) {
            auto& reply = pending[id];
            if (reply.done) {
                const Reply done = std::move(reply);
                pending.erase(id);
                lock.unlock();
                if (!done.error.is_null()) api::rethrow(done.error);
                return done.result;
            }
            if (!connected || generation != made) {
                pending.erase(id);
                throw std::runtime_error("Server API: the connection to the Server was lost");
            }
            if (token.cancelled() && !cancelSent) {
                cancelSent = true;
                lock.unlock();
                try {
                    sending->send(json{{"cancel", id}}.dump());
                } catch (const std::exception&) {
                }
                lock.lock();
                continue;
            }
            changed.wait_for(lock, std::chrono::milliseconds(50));
        }
    }

    std::filesystem::path cacheFolder() const {
        return options.cache.empty() ? std::filesystem::temp_directory_path() / L"camelplayer-remote" : options.cache;
    }

    HttpReply http(const std::string& method, const std::string& target, const std::string& body = {}) const {
        const SOCKET s = connect();
        HttpReply reply;
        std::string rest;
        const bool sent = sendAll(s, method + " " + target + " HTTP/1.1\r\nHost: " + address().first +
                                         "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + authorization() +
                                         "Connection: close\r\n\r\n" + body);
        const auto head = sent ? readHead(s, rest, kMaxHead, kTransferTimeout) : std::nullopt;
        if (head) {
            reply.head = websocket::parseHead(*head);
            const auto& status = reply.head.second;
            std::from_chars(status.data(), status.data() + status.size(), reply.status);
            const auto length = reply.head.header("content-length");
            if (auto read = readBody(s, rest, length.empty() ? 0 : size_t(std::stoull(length)), kTransferTimeout))
                reply.body = std::move(*read);
            else
                reply.status = 0;
        }
        closesocket(s);
        if (reply.status == 0) throw std::runtime_error("Server API: the Server did not answer " + target);
        if (reply.status == 401) throw NotPaired();
        if (reply.status >= 400 && reply.status != 404) {
            json error;
            try {
                error = json::parse(reply.body).at("error");
            } catch (const json::exception&) {
                throw std::runtime_error("Server API: " + target + " failed with " + std::to_string(reply.status));
            }
            api::rethrow(error);
        }
        return reply;
    }
};

RemoteServer::RemoteServer(Options options) : impl_(std::make_shared<Impl>(std::move(options))) {
    if (impl_->own) impl_->following = impl_->own->followedZone;
    impl_->startDiscord();
    impl_->open();
    impl_->reader = std::thread([impl = impl_.get()] { impl->run(); });
    impl_->dispatcher = std::thread([impl = impl_.get()] { impl->dispatch(); });
    std::unique_lock lock(impl_->mutex);
    const bool answered = impl_->changed.wait_for(lock, impl_->options.timeout,
                                                  [this] { return impl_->current || !impl_->connected || impl_->serverOlder; });
    if (const auto older = impl_->serverOlder) {
        const auto version = impl_->hello.contains("server") ? impl_->hello["server"].value("version", std::string()) : "";
        lock.unlock();
        shutdown();
        throw VersionMismatch(*older, version);
    }
    if (!answered || !impl_->current) {
        lock.unlock();
        shutdown();
        throw std::runtime_error("Server API: the Server sent no state");
    }
}

RemoteServer::~RemoteServer() {
    shutdown();
}

bool RemoteServer::connected() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->connected;
}

IServer::Link RemoteServer::link() const {
    std::lock_guard lock(impl_->mutex);
    if (impl_->serverOlder) return *impl_->serverOlder ? Link::ServerTooOld : Link::ThisTooOld;
    return impl_->connected ? Link::Connected : impl_->refused ? Link::Refused : Link::Lost;
}

std::string RemoteServer::serverVersion() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->hello.contains("server") ? impl_->hello["server"].value("version", std::string()) : std::string();
}

std::string RemoteServer::serverName() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->hello.contains("server") ? impl_->hello["server"].value("name", std::string()) : std::string();
}

json RemoteServer::serverUpdate() {
    return impl_->call("serverUpdate", json::array(), {});
}

std::string RemoteServer::updateServer() {
    return impl_->call("updateServer", json::array(), {}).at("version").get<std::string>();
}

json RemoteServer::hello() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->hello;
}

std::string RemoteServer::device() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->hello.value("device", std::string());
}

RemoteServer::Introduction RemoteServer::introduction(const std::filesystem::path& data, const std::string& server) {
    if (data.empty()) return {};
    return {serverIdIn(data), Pairings(Pairings::fileIn(data)).proofFor(server).value_or(std::string())};
}

RemoteServer::Paired RemoteServer::pair(const std::string& host, uint16_t port, const std::string& name, const std::string& code,
                                        const Introduction& from, std::chrono::milliseconds timeout) {
    Options options;
    options.host = host;
    options.port = port;
    options.timeout = timeout;
    const Impl impl(options);
    json asked{{"name", name}};
    if (!code.empty()) asked["code"] = code;
    if (!from.server.empty()) asked["server"] = from.server;
    if (!from.proof.empty()) asked["proof"] = from.proof;
    const SOCKET s = impl.connect();
    const auto body = asked.dump();
    std::string rest;
    const bool sent = sendAll(s, "POST /pair HTTP/1.1\r\nHost: " + host + "\r\nContent-Length: " + std::to_string(body.size()) +
                                     "\r\nConnection: close\r\n\r\n" + body);
    const auto head = sent ? readHead(s, rest, kMaxHead, timeout) : std::nullopt;
    std::optional<std::string> answer;
    websocket::HttpHead parsed;
    if (head) {
        parsed = websocket::parseHead(*head);
        const auto length = parsed.header("content-length");
        answer = readBody(s, rest, length.empty() ? 0 : size_t(std::stoull(length)), timeout);
    }
    closesocket(s);
    if (!answer) throw std::runtime_error("Server API: the Server did not answer the pairing");
    const auto reply = json::parse(*answer);
    if (parsed.second == "200") return {reply.at("token").get<std::string>(), reply.at("device").get<std::string>()};
    const auto& error = reply.at("error");
    throw PairingRefused(error.value("code", std::string("refused")), error.value("suggestion", std::string()),
                         error.value("until", int64_t{0}));
}

std::string RemoteServer::pairingCode() {
    return impl_->call("pairingCode", json::array(), {}).at("code").get<std::string>();
}

json RemoteServer::pairedDevices() {
    return impl_->call("pairedDevices", json::array(), {});
}

bool RemoteServer::revokeDevice(const std::string& device) {
    return as<bool>(impl_->call("revokeDevice", args(device), {}));
}

json RemoteServer::renameDevice(const std::string& name) {
    return impl_->call("renameDevice", args(name), {});
}

void RemoteServer::resetOwner() {
    impl_->call("resetOwner", json::array(), {});
}

void RemoteServer::setNoticeListener(std::function<void(const json&)> listener) {
    std::lock_guard lock(impl_->mutex);
    impl_->noticeListener = std::move(listener);
}

void RemoteServer::shutdown() {
    std::shared_ptr<Connection> closing;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopping = true;
        closing = impl_->connection;
    }
    impl_->changed.notify_all();
    if (closing) closing->shut();
    if (impl_->reader.joinable()) impl_->reader.join();
    if (impl_->dispatcher.joinable()) impl_->dispatcher.join();
}

CommandStamp RemoteServer::submit(Command command) {
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->own) {
            if (command.kind == CommandKind::ApplySettings && command.settings)
                impl_->toServer(command);
            else if (actsOnZone(command.kind) && command.zone.empty())
                command.zone = impl_->following;
        }
    }
    const auto stamp = commandStampFromJson(impl_->call("submit", args(toJson(command)), {}));
    std::lock_guard lock(impl_->mutex);
    impl_->lastSubmitted = std::max(impl_->lastSubmitted, stamp.id);
    return stamp;
}

Snapshot RemoteServer::snapshot() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->current ? *impl_->current : Snapshot{};
}

void RemoteServer::setListener(std::function<void(const Event&)> listener) {
    std::lock_guard lock(impl_->mutex);
    impl_->listener = std::move(listener);
}

// Done on the Server, shown here and told to the listener: the Server's answer, the revision its work reached, can come
// before the event that shows it. An older Server answers nothing, and the last command submitted stands for it.
void RemoteServer::waitIdle() {
    const json reached = impl_->call("waitIdle", json::array(), {});
    const bool fromListener = std::this_thread::get_id() == impl_->dispatcher.get_id();
    std::unique_lock lock(impl_->mutex);
    impl_->changed.wait_for(lock, std::chrono::seconds(10), [&] {
        if (!impl_->connected) return true;
        const auto& now = impl_->current;
        const bool shown = now && (reached.is_number_unsigned() ? now->revision >= reached.get<uint64_t>()
                                                                : now->command.id >= impl_->lastSubmitted);
        return shown && (fromListener || (impl_->events.empty() && !impl_->telling));
    });
}

LibraryTrackPage RemoteServer::browseTracks(int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                            const CancellationToken& token) const {
    return impl_->sawLibrary(as<LibraryTrackPage>(impl_->call("browseTracks", args(offset, limit, revision), token)));
}

LibraryTrackPage RemoteServer::searchTracks(std::string query, int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                            const CancellationToken& token) const {
    return impl_->sawLibrary(as<LibraryTrackPage>(impl_->call("searchTracks", args(query, offset, limit, revision), token)));
}

LibrarySearchResults RemoteServer::searchLibrary(std::string query, int64_t offset, int64_t limit,
                                                 std::optional<uint64_t> revision, const CancellationToken& token,
                                                 bool totals) const {
    return impl_->sawLibrary(
        as<LibrarySearchResults>(impl_->call("searchLibrary", args(query, offset, limit, revision, totals), token)));
}

PlayStatistics RemoteServer::playStatistics(const PlayStatisticsQuery& query, const CancellationToken& token) const {
    return as<PlayStatistics>(impl_->call("playStatistics", args(query), token));
}

void RemoteServer::rereadLibrary() {
    impl_->call("rereadLibrary", json::array(), {});
}

LibraryScanStart RemoteServer::scanLibraryNow(std::optional<int64_t> folder) {
    return as<LibraryScanStart>(impl_->call("scanLibraryNow", args(folder), {}));
}

void RemoteServer::clearArtworkCache() {
    impl_->call("clearArtworkCache", json::array(), {});
    // The covers this Control point kept go too, as they are made again from the Server's.
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(impl_->cacheFolder(), error))
        if (entry.path().extension() == L".jpg") std::filesystem::remove(entry.path(), error);
}

LibraryAlbumPage RemoteServer::recentlyPlayedAlbums(int64_t offset, int64_t limit, const CancellationToken& token) const {
    return as<LibraryAlbumPage>(impl_->call("recentlyPlayedAlbums", args(offset, limit), token));
}

LibraryAlbumPage RemoteServer::browseAlbums(AlbumSort sort, const std::optional<LibraryGroupFilter>& filter, int64_t offset,
                                            int64_t limit, std::optional<uint64_t> revision,
                                            const CancellationToken& token) const {
    return impl_->sawLibrary(
        as<LibraryAlbumPage>(impl_->call("browseAlbums", args(sort, filter, offset, limit, revision), token)));
}

LibraryGroupPage RemoteServer::browseGroups(LibraryGroupField field, bool favoritesOnly, int64_t offset, int64_t limit,
                                            std::optional<uint64_t> revision, const CancellationToken& token) const {
    return impl_->sawLibrary(
        as<LibraryGroupPage>(impl_->call("browseGroups", args(field, favoritesOnly, offset, limit, revision), token)));
}

std::vector<LibraryInitial> RemoteServer::groupInitials(LibraryGroupField field, const CancellationToken& token) const {
    return as<std::vector<LibraryInitial>>(impl_->call("groupInitials", args(field), token));
}

std::vector<LibraryInitial> RemoteServer::albumInitials(AlbumSort sort, const CancellationToken& token) const {
    return as<std::vector<LibraryInitial>>(impl_->call("albumInitials", args(sort), token));
}

std::vector<std::vector<LibraryNamePart>> RemoteServer::creditParts(const std::vector<std::string>& credits,
                                                                    const CancellationToken& token) const {
    return as<std::vector<std::vector<LibraryNamePart>>>(impl_->call("creditParts", args(credits), token));
}

LibraryTrackRowPage RemoteServer::browseTrackRows(TrackSort sort, bool descending,
                                                  const std::optional<LibraryGroupFilter>& filter, int64_t offset, int64_t limit,
                                                  std::optional<uint64_t> revision, const CancellationToken& token) const {
    return impl_->sawLibrary(
        as<LibraryTrackRowPage>(impl_->call("browseTrackRows", args(sort, descending, filter, offset, limit, revision), token)));
}

std::optional<int64_t> RemoteServer::albumPosition(AlbumSort sort, const std::optional<LibraryGroupFilter>& filter,
                                                   bool favorites, int64_t album, const CancellationToken& token) const {
    return as<std::optional<int64_t>>(impl_->call("albumPosition", args(sort, filter, favorites, album), token));
}

std::optional<int64_t> RemoteServer::trackRowPosition(TrackSort sort, bool descending,
                                                      const std::optional<LibraryGroupFilter>& filter, int64_t file,
                                                      const CancellationToken& token) const {
    return as<std::optional<int64_t>>(impl_->call("trackRowPosition", args(sort, descending, filter, file), token));
}

LibraryArtistPage RemoteServer::artistPage(const LibraryGroupFilter& artist, const CancellationToken& token) const {
    return as<LibraryArtistPage>(impl_->call("artistPage", args(artist), token));
}

LibraryTrackRowPage RemoteServer::smartTracks(SmartList list, int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                              const CancellationToken& token) const {
    return impl_->sawLibrary(as<LibraryTrackRowPage>(impl_->call("smartTracks", args(list, offset, limit, revision), token)));
}

LibraryAlbumPage RemoteServer::favoriteAlbums(int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                              const CancellationToken& token) const {
    return impl_->sawLibrary(as<LibraryAlbumPage>(impl_->call("favoriteAlbums", args(offset, limit, revision), token)));
}

LibraryGroupPage RemoteServer::favoriteArtists(int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                               const CancellationToken& token) const {
    return impl_->sawLibrary(as<LibraryGroupPage>(impl_->call("favoriteArtists", args(offset, limit, revision), token)));
}

// The cover comes over HTTP and is kept by its key in the cache folder, where the window reads it as its own.
std::optional<ArtworkReference> RemoteServer::artwork(const ArtworkRequest& request, const CancellationToken& token) const {
    if (token.cancelled()) return std::nullopt;
    const auto target = "/artwork?kind=" + json(request.kind).get<std::string>() + "&id=" + std::to_string(request.id) +
                        "&path=" + websocket::percentEncode(request.path) + "&name=" + websocket::percentEncode(request.name) +
                        "&pixels=" + std::to_string(request.pixels);
    const bool keep = request.kind == ArtworkKind::Album || request.kind == ArtworkKind::Folder;
    const uint64_t library = impl_->library;
    if (keep) {
        std::string kept;
        {
            std::scoped_lock lock(impl_->coversMutex);
            if (const auto found = impl_->covers.find(target); found != impl_->covers.end() && found->second.library == library)
                kept = found->second.key;
        }
        std::error_code ec;
        if (const auto file = impl_->cacheFolder() / (toWide(kept) + L".jpg"); !kept.empty() && std::filesystem::exists(file, ec))
            return ArtworkReference{kept, file};
    }
    const auto reply = impl_->http("GET", target);
    if (reply.status != 200) return std::nullopt;
    const auto key = reply.head.header("x-artwork-key");
    if (key.empty() || key.find_first_of("/\\:.") != std::string::npos) return std::nullopt;
    if (keep) {
        std::scoped_lock lock(impl_->coversMutex);
        impl_->covers[target] = {key, library};
    }
    const auto folder = impl_->cacheFolder();
    std::filesystem::create_directories(folder);
    const auto file = folder / (toWide(key) + L".jpg");
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        // Two asking at once for one cover, as an album row and the player bar, wrote one partial file: the first's
        // rename was refused while the other held it, and it named a cover not yet in place.
        const auto partial = folder / (toWide(key) + L"-" + std::to_wstring(++impl_->coversWritten) + L".part");
        std::ofstream(partial, std::ios::binary).write(reply.body.data(), std::streamsize(reply.body.size()));
        std::filesystem::rename(partial, file, ec);
        if (ec) std::filesystem::remove(partial, ec); // the other's is in place, or being read
    }
    return ArtworkReference{key, file};
}

ArtistPicture RemoteServer::artistPicture(const std::string& name, bool lookUp, const CancellationToken& token) {
    return as<ArtistPicture>(impl_->call("artistPicture", args(name, lookUp), token));
}

ArtistPicture RemoteServer::setArtistPicture(const std::string& name, const std::vector<uint8_t>& image) {
    const auto reply =
        impl_->http("POST", "/artist-picture?name=" + websocket::percentEncode(name), std::string(image.begin(), image.end()));
    return as<ArtistPicture>(json::parse(reply.body));
}

ArtistPicture RemoteServer::removeArtistPicture(const std::string& name) {
    return as<ArtistPicture>(impl_->call("removeArtistPicture", args(name), {}));
}

ArtistPicture RemoteServer::chooseArtistCandidate(const std::string& name, const std::string& mbid,
                                                  const CancellationToken& token) {
    return as<ArtistPicture>(impl_->call("chooseArtistCandidate", args(name, mbid), token));
}

LibraryAlbumDetail RemoteServer::albumDetail(int64_t album, const CancellationToken& token) const {
    return as<LibraryAlbumDetail>(impl_->call("albumDetail", args(album), token));
}

std::vector<std::string> RemoteServer::albumTrackPaths(const std::vector<int64_t>& albums, const CancellationToken& token) const {
    return as<std::vector<std::string>>(impl_->call("albumTrackPaths", args(albums), token));
}

LibraryFolderListing RemoteServer::browseFolder(std::optional<int64_t> folder, const CancellationToken& token) const {
    return as<LibraryFolderListing>(impl_->call("browseFolder", args(folder), token));
}

std::vector<LibraryTrackRow> RemoteServer::folderTracks(int64_t folder, const CancellationToken& token) const {
    return as<std::vector<LibraryTrackRow>>(impl_->call("folderTracks", args(folder), token));
}

std::vector<LibraryTrackRow> RemoteServer::folderTracksFrom(int64_t track, const CancellationToken& token) const {
    return as<std::vector<LibraryTrackRow>>(impl_->call("folderTracksFrom", args(track), token));
}

std::vector<int64_t> RemoteServer::fileFolders(int64_t file) const {
    return as<std::vector<int64_t>>(impl_->call("fileFolders", args(file), {}));
}

std::vector<LibraryRootInfo> RemoteServer::libraryRoots() const {
    return as<std::vector<LibraryRootInfo>>(impl_->call("libraryRoots", json::array(), {}));
}

LibraryRootContents RemoteServer::libraryRootContents(int64_t root) const {
    return as<LibraryRootContents>(impl_->call("libraryRootContents", args(root), {}));
}

LibraryScanProgress RemoteServer::libraryScanProgress() const {
    return as<LibraryScanProgress>(impl_->call("libraryScanProgress", json::array(), {}));
}

LoudnessProgress RemoteServer::loudnessProgress() const {
    return as<LoudnessProgress>(impl_->call("loudnessProgress", json::array(), {}));
}

std::vector<PlaylistSummary> RemoteServer::playlists() const {
    return as<std::vector<PlaylistSummary>>(impl_->call("playlists", json::array(), {}));
}

PlaylistPage RemoteServer::playlistItems(int64_t playlist, int64_t offset, int64_t limit) const {
    return as<PlaylistPage>(impl_->call("playlistItems", args(playlist, offset, limit), {}));
}

SelectionPreview RemoteServer::previewSelection(const std::vector<PlaylistSource>& sources,
                                                const CancellationToken& token) const {
    return as<SelectionPreview>(impl_->call("previewSelection", args(sources), token));
}

PlaylistChange RemoteServer::createPlaylist(std::string name, const std::vector<PlaylistSource>& sources,
                                            const CancellationToken& token) {
    return as<PlaylistChange>(impl_->call("createPlaylist", args(name, sources), token));
}

PlaylistChange RemoteServer::addToPlaylist(int64_t playlist, std::optional<int64_t> revision,
                                           const std::vector<PlaylistSource>& sources, const CancellationToken& token) {
    return as<PlaylistChange>(impl_->call("addToPlaylist", args(playlist, revision, sources), token));
}

PlaylistChange RemoteServer::saveQueueAsPlaylist(std::string name, const std::string& zone) {
    return as<PlaylistChange>(impl_->call("saveQueueAsPlaylist", args(name, zone), {}));
}

PlaylistChange RemoteServer::editPlaylist(const PlaylistEdit& edit) {
    return as<PlaylistChange>(impl_->call("editPlaylist", args(edit), {}));
}

bool RemoteServer::undoPlaylistChange(const PlaylistUndo& undo) {
    return as<bool>(impl_->call("undoPlaylistChange", args(undo), {}));
}

PlaylistChange RemoteServer::importPlaylist(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + toUtf8(file.wstring()));
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const auto reply = impl_->http("POST", "/playlist?name=" + websocket::percentEncode(toUtf8(file.filename().wstring())), text);
    return as<PlaylistChange>(json::parse(reply.body));
}

std::vector<ServerFolder> RemoteServer::serverFolders(const std::string& pathUtf8) const {
    return as<std::vector<ServerFolder>>(impl_->call("serverFolders", args(pathUtf8), {}));
}

std::string RemoteServer::keepFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + toUtf8(file.wstring()));
    const std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const auto reply = impl_->http("POST", "/upload?name=" + websocket::percentEncode(toUtf8(file.filename().wstring())), bytes);
    return json::parse(reply.body).at("path").get<std::string>();
}

// Kept under a name of its own for each path, so an impulse response read once is read from here after.
std::filesystem::path RemoteServer::readableFile(const std::string& pathUtf8) const {
    const auto folder = impl_->cacheFolder() / L"files";
    std::filesystem::create_directories(folder);
    const auto name = std::filesystem::path(toWide(pathUtf8)).filename().wstring();
    const auto file = folder / (std::to_wstring(std::hash<std::string>{}(pathUtf8)) + L"-" + name);
    std::error_code error;
    if (std::filesystem::exists(file, error)) return file;
    const auto reply = impl_->http("GET", "/upload?path=" + websocket::percentEncode(pathUtf8));
    if (reply.status != 200) throw std::runtime_error("Server API: the Server does not send " + pathUtf8);
    const auto partial = std::filesystem::path(file).concat(L".part");
    std::ofstream(partial, std::ios::binary).write(reply.body.data(), std::streamsize(reply.body.size()));
    std::filesystem::rename(partial, file, error);
    return file;
}

bool RemoteServer::playerFirewallRule() const {
    return as<bool>(impl_->call("playerFirewallRule", json::array(), {}));
}

void RemoteServer::exportPlaylist(int64_t playlist, const std::filesystem::path& file) const {
    const auto reply = impl_->http("GET", "/playlist?id=" + std::to_string(playlist));
    if (reply.status != 200) throw std::runtime_error("Server API: no such playlist");
    std::ofstream out(file, std::ios::binary);
    out.write(reply.body.data(), std::streamsize(reply.body.size()));
    if (!out) throw std::runtime_error("cannot write " + toUtf8(file.wstring()));
}

void RemoteServer::writeDiagnosticBundle(const std::filesystem::path& file, bool includePaths,
                                         const std::string& appVersion) const {
    const auto reply = impl_->http("GET", std::string("/diagnostic?paths=") + (includePaths ? "1" : "0") +
                                              "&version=" + websocket::percentEncode(appVersion));
    if (reply.status != 200) throw std::runtime_error("Server API: the Server made no diagnostic bundle");
    std::ofstream out(file, std::ios::binary);
    out.write(reply.body.data(), std::streamsize(reply.body.size()));
    if (!out) throw std::runtime_error("cannot write " + toUtf8(file.wstring()));
}

DataRecovery RemoteServer::dataRecovery() const {
    return as<DataRecovery>(impl_->call("dataRecovery", json::array(), {}));
}

DiscordStatus RemoteServer::discordStatus() const {
    if (impl_->discord) return impl_->discord->status();
    return as<DiscordStatus>(impl_->call("discordStatus", json::array(), {}));
}

Lyrics RemoteServer::lyrics(const std::string& pathUtf8) const {
    return as<Lyrics>(impl_->call("lyrics", args(pathUtf8), {}));
}

FileDetails RemoteServer::fileDetails(const std::string& pathUtf8) const {
    return as<FileDetails>(impl_->call("fileDetails", args(pathUtf8), {}));
}

std::optional<std::vector<uint8_t>> RemoteServer::waveform(const std::string& pathUtf8) const {
    return as<std::optional<std::vector<uint8_t>>>(impl_->call("waveform", args(pathUtf8), {}));
}

void RemoteServer::setDiscordTexts(const DiscordTexts& texts) {
    if (impl_->discord)
        impl_->discord->setTexts(texts);
    else
        impl_->call("setDiscordTexts", args(texts), {});
}

} // namespace camel::server
