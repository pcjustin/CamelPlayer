// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Remote/RemoteServer.h"

#include <ws2tcpip.h>

#include <charconv>
#include <condition_variable>
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
#include "Server/Network/SocketWait.h"
#include "Server/Remote/Connection.h"

namespace camel::server {

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

    explicit Impl(Options o) : options(std::move(o)) {}

    Options options;
    WinsockScope winsock;
    std::unique_ptr<Connection> connection;
    std::thread reader;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::map<uint64_t, Reply> pending;
    uint64_t lastCall = 0;
    bool connected = false;
    std::optional<Snapshot> current;
    std::function<void(const Event&)> listener;
    json hello;
    api::QueueDecoder decoder; // the reader's alone

    SOCKET connect() const {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* found = nullptr;
        if (getaddrinfo(options.host.c_str(), std::to_string(options.port).c_str(), &hints, &found) != 0 || !found)
            throw std::runtime_error("Server API: " + options.host + " is not found");
        const SOCKET s = socket(found->ai_family, found->ai_socktype, found->ai_protocol);
        const auto reach =
            s == kNoSocket ? Reach::Failed : connectWithin(s, found->ai_addr, int(found->ai_addrlen), options.timeout);
        freeaddrinfo(found);
        if (reach != Reach::Connected) {
            if (s != kNoSocket) closesocket(s);
            throw std::runtime_error("Server API: " + options.host + " does not answer");
        }
        u_long blocking = 0;
        ioctlsocket(s, FIONBIO, &blocking);
        return s;
    }

    void open() {
        const SOCKET s = connect();
        std::array<uint8_t, 16> nonce{};
        std::mt19937 generator{std::random_device{}()};
        for (auto& byte : nonce) byte = uint8_t(generator());
        const auto key = websocket::base64(nonce);
        std::string rest;
        const bool sent = sendAll(s, "GET /api HTTP/1.1\r\nHost: " + options.host + ":" + std::to_string(options.port) +
                                         "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
                                         "\r\nSec-WebSocket-Version: 13\r\n\r\n");
        const auto head = sent ? readHead(s, rest, kMaxHead, options.timeout) : std::nullopt;
        if (!head) {
            closesocket(s);
            throw std::runtime_error("Server API: the Server did not answer the connection");
        }
        const auto answer = websocket::parseHead(*head);
        if (answer.second != "101" || answer.header("sec-websocket-accept") != websocket::acceptKey(key)) {
            closesocket(s);
            throw std::runtime_error("Server API: the Server refused the connection (" + answer.second + ")");
        }
        connection = std::make_unique<Connection>(s, true, std::move(rest));
        connected = true;
    }

    void readAll() {
        try {
            while (const auto message = connection->read(kMaxMessage)) handle(*message);
        } catch (const std::exception& e) {
            log::warn("Server API: the connection ended: %s", e.what());
        }
        {
            std::lock_guard lock(mutex);
            connected = false;
        }
        changed.notify_all();
    }

    void handle(const std::string& text) {
        json j = json::parse(text);
        if (j.contains("hello")) {
            std::lock_guard lock(mutex);
            hello = j["hello"];
            return;
        }
        if (j.contains("event")) {
            decoder.decode(j.at("snapshot"));
            auto snapshot = snapshotFromJson(j["snapshot"]);
            const auto kind = j["event"] == "commandCompleted" ? EventKind::CommandCompleted : EventKind::StateChanged;
            std::function<void(const Event&)> tell;
            {
                std::lock_guard lock(mutex);
                current = snapshot;
                tell = listener;
            }
            changed.notify_all();
            if (tell) tell(Event{kind, std::move(snapshot)});
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

    json call(const std::string& name, json arguments, const CancellationToken& token) {
        uint64_t id = 0;
        {
            std::lock_guard lock(mutex);
            if (!connected) throw std::runtime_error("Server API: not connected to the Server");
            id = ++lastCall;
            pending[id];
        }
        try {
            connection->send(json{{"id", id}, {"call", name}, {"args", std::move(arguments)}}.dump(
                -1, ' ', false, json::error_handler_t::replace));
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
            if (!connected) {
                pending.erase(id);
                throw std::runtime_error("Server API: the connection to the Server was lost");
            }
            if (token.cancelled() && !cancelSent) {
                cancelSent = true;
                lock.unlock();
                try {
                    connection->send(json{{"cancel", id}}.dump());
                } catch (const std::exception&) {
                }
                lock.lock();
                continue;
            }
            changed.wait_for(lock, std::chrono::milliseconds(50));
        }
    }

    HttpReply http(const std::string& method, const std::string& target, const std::string& body = {}) const {
        const SOCKET s = connect();
        HttpReply reply;
        std::string rest;
        const bool sent = sendAll(s, method + " " + target + " HTTP/1.1\r\nHost: " + options.host + "\r\nContent-Length: " +
                                         std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
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
    impl_->open();
    impl_->reader = std::thread([impl = impl_.get()] { impl->readAll(); });
    std::unique_lock lock(impl_->mutex);
    if (!impl_->changed.wait_for(lock, impl_->options.timeout, [this] { return impl_->current || !impl_->connected; }) ||
        !impl_->current) {
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

json RemoteServer::hello() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->hello;
}

void RemoteServer::shutdown() {
    if (impl_->connection) impl_->connection->shut();
    if (impl_->reader.joinable()) impl_->reader.join();
}

CommandStamp RemoteServer::submit(Command command) {
    return commandStampFromJson(impl_->call("submit", args(toJson(command)), {}));
}

Snapshot RemoteServer::snapshot() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->current ? *impl_->current : Snapshot{};
}

void RemoteServer::setListener(std::function<void(const Event&)> listener) {
    std::lock_guard lock(impl_->mutex);
    impl_->listener = std::move(listener);
}

void RemoteServer::waitIdle() {
    impl_->call("waitIdle", json::array(), {});
}

LibraryTrackPage RemoteServer::browseTracks(int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                            const CancellationToken& token) const {
    return as<LibraryTrackPage>(impl_->call("browseTracks", args(offset, limit, revision), token));
}

LibraryTrackPage RemoteServer::searchTracks(std::string query, int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                            const CancellationToken& token) const {
    return as<LibraryTrackPage>(impl_->call("searchTracks", args(query, offset, limit, revision), token));
}

LibrarySearchResults RemoteServer::searchLibrary(std::string query, int64_t offset, int64_t limit,
                                                 std::optional<uint64_t> revision, const CancellationToken& token,
                                                 bool totals) const {
    return as<LibrarySearchResults>(impl_->call("searchLibrary", args(query, offset, limit, revision, totals), token));
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
}

LibraryAlbumPage RemoteServer::recentlyPlayedAlbums(int64_t offset, int64_t limit, const CancellationToken& token) const {
    return as<LibraryAlbumPage>(impl_->call("recentlyPlayedAlbums", args(offset, limit), token));
}

LibraryAlbumPage RemoteServer::browseAlbums(AlbumSort sort, const std::optional<LibraryGroupFilter>& filter, int64_t offset,
                                            int64_t limit, std::optional<uint64_t> revision,
                                            const CancellationToken& token) const {
    return as<LibraryAlbumPage>(impl_->call("browseAlbums", args(sort, filter, offset, limit, revision), token));
}

LibraryGroupPage RemoteServer::browseGroups(LibraryGroupField field, bool favoritesOnly, int64_t offset, int64_t limit,
                                            std::optional<uint64_t> revision, const CancellationToken& token) const {
    return as<LibraryGroupPage>(impl_->call("browseGroups", args(field, favoritesOnly, offset, limit, revision), token));
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
    return as<LibraryTrackRowPage>(
        impl_->call("browseTrackRows", args(sort, descending, filter, offset, limit, revision), token));
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
    return as<LibraryTrackRowPage>(impl_->call("smartTracks", args(list, offset, limit, revision), token));
}

LibraryAlbumPage RemoteServer::favoriteAlbums(int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                              const CancellationToken& token) const {
    return as<LibraryAlbumPage>(impl_->call("favoriteAlbums", args(offset, limit, revision), token));
}

LibraryGroupPage RemoteServer::favoriteArtists(int64_t offset, int64_t limit, std::optional<uint64_t> revision,
                                               const CancellationToken& token) const {
    return as<LibraryGroupPage>(impl_->call("favoriteArtists", args(offset, limit, revision), token));
}

// The cover comes over HTTP and is kept by its key in the cache folder, where the window reads it as its own.
std::optional<ArtworkReference> RemoteServer::artwork(const ArtworkRequest& request, const CancellationToken& token) const {
    if (token.cancelled()) return std::nullopt;
    const auto reply =
        impl_->http("GET", "/artwork?kind=" + json(request.kind).get<std::string>() + "&id=" + std::to_string(request.id) +
                               "&path=" + websocket::percentEncode(request.path) +
                               "&name=" + websocket::percentEncode(request.name) + "&pixels=" + std::to_string(request.pixels));
    if (reply.status != 200) return std::nullopt;
    const auto key = reply.head.header("x-artwork-key");
    if (key.empty() || key.find_first_of("/\\:.") != std::string::npos) return std::nullopt;
    const auto folder =
        impl_->options.cache.empty() ? std::filesystem::temp_directory_path() / L"camelplayer-remote" : impl_->options.cache;
    std::filesystem::create_directories(folder);
    const auto file = folder / (toWide(key) + L".jpg");
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        const auto partial = folder / (toWide(key) + L".part");
        std::ofstream(partial, std::ios::binary).write(reply.body.data(), std::streamsize(reply.body.size()));
        std::filesystem::rename(partial, file, ec);
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
    impl_->call("setDiscordTexts", args(texts), {});
}

} // namespace camel::server
