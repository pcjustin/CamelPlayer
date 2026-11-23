// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Remote/ApiServer.h"

#include <ws2tcpip.h>

#include <atomic>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "Common/util/Log.h"
#include "Common/util/Text.h"
#include "Server/Api/ApiCalls.h"
#include "Server/Api/ApiTypesJson.h"
#include "Server/Network/SocketWait.h"
#include "Server/Remote/ApiSession.h"
#include "Server/Remote/Connection.h"
#include "Server/Remote/Pairings.h"

namespace camel::server {

using nlohmann::json;
using namespace remote;

namespace {

constexpr SOCKET kNoSocket = INVALID_SOCKET;
constexpr size_t kMaxHead = size_t{16} * 1024;
constexpr size_t kMaxMessage = size_t{4} * 1024 * 1024;
constexpr size_t kMaxUpload = size_t{64} * 1024 * 1024;
constexpr auto kHeadTimeout = std::chrono::seconds(10);
constexpr auto kBodyTimeout = std::chrono::seconds(60);

void respond(SOCKET s, int status, const char* reason, const std::string& type, const std::string& body,
             const std::string& extra = {}) {
    sendAll(s, "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\nContent-Type: " + type +
                   "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + extra + "Connection: close\r\n\r\n" + body);
}

void respondError(SOCKET s, int status, const char* reason, const std::exception_ptr& error) {
    respond(s, status, reason, "application/json", json{{"error", api::errorOf(error)}}.dump());
}

std::string readFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

int64_t number(const std::map<std::string, std::string>& query, const char* key) {
    const auto found = query.find(key);
    if (found == query.end()) throw std::invalid_argument(std::string("API: no ") + key);
    size_t used = 0;
    const auto value = std::stoll(found->second, &used);
    if (used != found->second.size()) throw std::invalid_argument(std::string("API: ") + key + " is not a number");
    return value;
}

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// The token of `Authorization: Bearer <token>`.
std::string bearer(const websocket::HttpHead& request) {
    const auto value = request.header("authorization");
    return value.starts_with("Bearer ") ? value.substr(7) : std::string();
}

json errorBody(const char* code, const std::string& message) {
    return json{{"error", {{"code", code}, {"message", message}}}};
}

std::string textOf(const std::map<std::string, std::string>& query, const char* key) {
    const auto found = query.find(key);
    return found == query.end() ? std::string() : found->second;
}

} // namespace

struct ApiServer::Impl {
    struct Client {
        SOCKET socket = INVALID_SOCKET; // until its thread closes it
        std::shared_ptr<ApiSession> session;
        std::shared_ptr<Connection> connection;
        std::string device; // the paired device it belongs to
        std::thread thread;
        bool done = false;
    };

    Impl(LocalServer& s, Options o) : server(s), options(std::move(o)), pairings(options.pairings) {}

    LocalServer& server;
    Options options;
    Pairings pairings;
    WinsockScope winsock;
    SOCKET listener = INVALID_SOCKET;
    uint16_t port = 0;
    mutable std::mutex mutex;
    std::map<uint64_t, Client> clients;
    uint64_t lastClient = 0;
    bool stopping = false;
    std::atomic<size_t> sessions{0};
    uint64_t observer = 0;
    std::thread acceptor;

    void listen() {
        for (int i = 0; i < std::max(1, options.portsToTry); ++i) {
            const auto wanted = uint16_t(options.port == 0 ? 0 : options.port + i);
            const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == kNoSocket) break;
            BOOL exclusive = TRUE; // another program on the port is seen, not shared
            setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(wanted);
            address.sin_addr.s_addr = htonl(options.loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
            if (bind(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && ::listen(s, SOMAXCONN) == 0) {
                int length = sizeof(address);
                getsockname(s, reinterpret_cast<sockaddr*>(&address), &length);
                listener = s;
                port = ntohs(address.sin_port);
                return;
            }
            closesocket(s);
            if (options.port == 0) break;
        }
        throw std::runtime_error("Server API: no free port from " + std::to_string(options.port));
    }

    void accept() {
        for (;;) {
            const SOCKET s = ::accept(listener, nullptr, nullptr);
            std::lock_guard lock(mutex);
            if (stopping) {
                if (s != kNoSocket) closesocket(s);
                return;
            }
            if (s == kNoSocket) continue;
            for (auto it = clients.begin(); it != clients.end();) {
                if (!it->second.done) {
                    ++it;
                    continue;
                }
                it->second.thread.join();
                it = clients.erase(it);
            }
            const auto id = ++lastClient;
            auto& client = clients[id];
            client.socket = s;
            client.thread = std::thread([this, id, s] { serve(id, s); });
        }
    }

    // The client's socket is closed only after it is marked done, so the destructor never shuts another's.
    void finished(uint64_t id) {
        std::lock_guard lock(mutex);
        clients[id].done = true;
        clients[id].socket = INVALID_SOCKET;
        clients[id].session.reset();
    }

    void serve(uint64_t id, SOCKET s) {
        std::string rest;
        try {
            const auto head = readHead(s, rest, kMaxHead, kHeadTimeout);
            if (head) {
                const auto request = websocket::parseHead(*head);
                const std::string path = request.second.substr(0, request.second.find('?'));
                // Only pairing goes without a paired device's token.
                if (path == "/pair" && request.first == "POST") {
                    servePair(s, request, rest);
                } else if (const auto device = pairings.deviceOf(bearer(request)); !device) {
                    respond(s, 401, "Unauthorized", "application/json",
                            errorBody("notPaired", "this device is not paired with the Server").dump());
                } else if (path == "/api" && request.first == "GET") {
                    serveApi(id, s, request, std::move(rest), device->id);
                    return;
                } else {
                    serveHttp(s, request, path, rest);
                }
            }
        } catch (const std::invalid_argument&) {
            respondError(s, 400, "Bad Request", std::current_exception());
        } catch (const std::exception& e) {
            log::warn("Server API: %s", e.what());
        }
        finished(id);
        closesocket(s);
    }

    void servePair(SOCKET s, const websocket::HttpHead& request, std::string& rest) {
        const auto lengthText = request.header("content-length");
        const size_t length = lengthText.empty() ? 0 : size_t(std::stoull(lengthText));
        if (length > kMaxHead) throw std::invalid_argument("API: a pairing request too long");
        const auto body = readBody(s, rest, length, kHeadTimeout);
        if (!body) return;
        const auto asked = json::parse(*body);
        const auto code = asked.contains("code") && asked["code"].is_string() ? asked["code"].get<std::string>() : std::string();
        const auto answer = pairings.pair(asked.at("name").get<std::string>(), code, nowMs());
        if (answer.stoppedNow) {
            std::lock_guard lock(mutex);
            for (auto& [client, c] : clients)
                if (c.session) c.session->notify({{"kind", "pairingStopped"}, {"until", answer.stoppedUntil}});
        }
        if (!answer.token.empty()) {
            respond(s, 200, "OK", "application/json", json{{"token", answer.token}, {"device", answer.device}}.dump());
            return;
        }
        auto error = errorBody(answer.error.c_str(), "pairing refused: " + answer.error);
        if (!answer.suggestion.empty()) error["error"]["suggestion"] = answer.suggestion;
        if (answer.stoppedUntil) error["error"]["until"] = answer.stoppedUntil;
        const bool stopped = answer.error == "stopped" || answer.stoppedNow;
        const bool taken = answer.error == "nameTaken";
        respond(s,
                stopped ? 429
                : taken ? 409
                        : 403,
                stopped ? "Too Many Requests"
                : taken ? "Conflict"
                        : "Forbidden",
                "application/json", error.dump());
    }

    // The calls of pairing, which every paired device may make (docs/Plan.md 17.8).
    std::optional<ApiSession::Answer> pairingCall(const std::string& call, const json& args) {
        if (call == "pairingCode")
            return ApiSession::Answer{json{{"code", pairings.newCode(nowMs())}, {"validMs", Pairings::kCodeLifetimeMs}}, {}};
        if (call == "pairedDevices") return ApiSession::Answer{devicesJson(), {}};
        if (call == "revokeDevice") {
            const auto device = args.at(0).get<std::string>();
            return ApiSession::Answer{pairings.revoke(device), [this, device] { close(device); }};
        }
        if (call == "resetOwner") {
            pairings.resetOwner();
            return ApiSession::Answer{nullptr, [this] { close({}); }};
        }
        return std::nullopt;
    }

    json devicesJson() const {
        json out = json::array();
        std::lock_guard lock(mutex);
        for (const auto& device : pairings.devices()) {
            const bool connected =
                std::ranges::any_of(clients, [&](const auto& c) { return c.second.session && c.second.device == device.id; });
            out.push_back({{"id", device.id}, {"name", device.name}, {"paired", device.paired}, {"connected", connected}});
        }
        return out;
    }

    // Closes the connections of `device`, or of every device when empty.
    void close(const std::string& device) {
        std::lock_guard lock(mutex);
        for (auto& [id, client] : clients)
            if (client.connection && (device.empty() || client.device == device)) client.connection->shut(1008);
    }

    void serveApi(uint64_t id, SOCKET s, const websocket::HttpHead& request, std::string rest, const std::string& device) {
        const auto key = request.header("sec-websocket-key");
        if (key.empty() || request.header("sec-websocket-version") != "13") {
            respond(s, 426, "Upgrade Required", "text/plain", "a WebSocket is needed here", "Sec-WebSocket-Version: 13\r\n");
            finished(id);
            closesocket(s);
            return;
        }
        sendAll(s, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                       websocket::acceptKey(key) + "\r\n\r\n");
        auto connection = std::make_shared<Connection>(s, false, std::move(rest));
        auto session = std::make_shared<ApiSession>(
            server, [connection](const std::string& message) { connection->send(message); },
            json{{"api", {{"min", 2}, {"max", 2}}}, {"server", {{"id", options.serverId}, {"name", options.serverName}}}},
            [this](const std::string& call, const json& args) { return pairingCall(call, args); });
        {
            std::lock_guard lock(mutex);
            clients[id].session = session;
            clients[id].connection = connection;
            clients[id].device = device;
        }
        ++sessions;
        try {
            while (const auto message = connection->read(kMaxMessage)) session->receive(*message);
        } catch (const std::invalid_argument& e) {
            log::warn("Server API: a connection closed for %s", e.what());
            connection->shut(1008);
        }
        --sessions;
        {
            std::lock_guard lock(mutex);
            clients[id].session.reset();
            clients[id].connection.reset();
            clients[id].done = true;
            clients[id].socket = INVALID_SOCKET;
        }
        session.reset(); // waits for its calls, which send through the connection
        connection.reset();
    }

    void serveHttp(SOCKET s, const websocket::HttpHead& request, const std::string& path, std::string& rest) {
        try {
            const auto query = websocket::queryOf(request.second);
            const auto lengthText = request.header("content-length");
            const size_t length = lengthText.empty() ? 0 : size_t(std::stoull(lengthText));
            if (length > kMaxUpload) {
                respond(s, 413, "Content Too Large", "text/plain", "the upload is too large");
                return;
            }
            const auto body = readBody(s, rest, length, kBodyTimeout);
            if (!body) return;
            const auto bytes = [&] { return std::vector<uint8_t>(body->begin(), body->end()); };
            if (request.first == "GET" && path == "/artwork") {
                ArtworkRequest r;
                from_json(json(textOf(query, "kind")), r.kind);
                r.id = query.contains("id") ? number(query, "id") : 0;
                r.path = textOf(query, "path");
                r.name = textOf(query, "name");
                if (query.contains("pixels")) r.pixels = unsigned(number(query, "pixels"));
                const auto found = server.artwork(r);
                if (!found) {
                    respond(s, 404, "Not Found", "text/plain", "no artwork");
                    return;
                }
                respond(s, 200, "OK", "image/jpeg", readFile(found->path), "X-Artwork-Key: " + found->key + "\r\n");
            } else if (request.first == "POST" && path == "/artist-picture") {
                respond(s, 200, "OK", "application/json", json(server.setArtistPicture(textOf(query, "name"), bytes())).dump());
            } else if (request.first == "POST" && path == "/playlist") {
                const auto file = uploadPath(std::filesystem::path(toWide(textOf(query, "name"))).filename());
                std::ofstream(file, std::ios::binary).write(body->data(), std::streamsize(body->size()));
                respond(s, 200, "OK", "application/json", json(server.importPlaylist(file)).dump());
            } else if (request.first == "GET" && path == "/playlist") {
                const auto file = uploadPath(L"export.m3u8");
                server.exportPlaylist(number(query, "id"), file);
                const auto text = readFile(file);
                std::filesystem::remove(file);
                respond(s, 200, "OK", "audio/x-mpegurl", text);
            } else if (request.first == "GET" && path == "/diagnostic") {
                const auto file = uploadPath(L"diagnostic.zip");
                server.writeDiagnosticBundle(file, textOf(query, "paths") == "1", textOf(query, "version"));
                const auto zip = readFile(file);
                std::filesystem::remove(file);
                respond(s, 200, "OK", "application/zip", zip);
            } else {
                respond(s, 404, "Not Found", "text/plain", "no such resource");
            }
        } catch (const std::invalid_argument&) {
            respondError(s, 400, "Bad Request", std::current_exception());
        } catch (const json::exception&) {
            respondError(s, 400, "Bad Request", std::current_exception());
        } catch (const std::exception&) {
            respondError(s, 500, "Internal Server Error", std::current_exception());
        }
    }

    // A file of its own in the uploads folder for `name`.
    std::filesystem::path uploadPath(const std::filesystem::path& name) {
        auto folder = options.uploads.empty() ? std::filesystem::temp_directory_path() / L"camelplayer-api" : options.uploads;
        std::filesystem::create_directories(folder);
        static std::atomic<uint64_t> next{0};
        const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
        return folder / (std::to_wstring(stamp) + L"-" + std::to_wstring(++next) + L"-" +
                         (name.empty() ? std::wstring(L"upload") : name.wstring()));
    }
};

ApiServer::ApiServer(LocalServer& server, Options options) : impl_(std::make_shared<Impl>(server, std::move(options))) {
    impl_->listen();
    // An event already on its way when the server goes still finds what it needs.
    impl_->observer = server.addObserver([weak = std::weak_ptr<Impl>(impl_)](const Event& event) {
        const auto impl = weak.lock();
        if (!impl) return;
        std::lock_guard lock(impl->mutex);
        for (auto& [id, client] : impl->clients)
            if (client.session) client.session->push(event);
    });
    impl_->acceptor = std::thread([impl = impl_.get()] { impl->accept(); });
}

ApiServer::~ApiServer() {
    impl_->server.removeObserver(impl_->observer);
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopping = true;
        closesocket(impl_->listener);
        for (auto& [id, client] : impl_->clients)
            if (client.socket != kNoSocket) shutdown(client.socket, SD_BOTH);
    }
    impl_->acceptor.join();
    for (auto& [id, client] : impl_->clients) client.thread.join();
}

uint16_t ApiServer::port() const {
    return impl_->port;
}

size_t ApiServer::connections() const {
    return impl_->sessions.load();
}

std::string ApiServer::pairingCode() {
    return impl_->pairings.newCode(nowMs());
}

nlohmann::json ApiServer::pairedDevices() const {
    return impl_->devicesJson();
}

bool ApiServer::revoke(const std::string& device) {
    const bool done = impl_->pairings.revoke(device);
    impl_->close(device);
    return done;
}

void ApiServer::resetOwner() {
    impl_->pairings.resetOwner();
    impl_->close({});
}

} // namespace camel::server
