// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include <gtest/gtest.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <taglib/flacfile.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include "Server/Api/ApiCalls.h"
#include "Server/Api/ApiJson.h"
#include "Server/Api/ApiTypesJson.h"
#include "Server/Library/LibraryScanner.h"
#include "Server/Network/SocketWait.h"
#include "Server/Remote/ApiServer.h"
#include "Server/Remote/ApiSession.h"
#include "Server/Remote/Connection.h"
#include "Server/Remote/RemoteServer.h"
#include "TestUtil.h"

using namespace camel::server;
using nlohmann::json;

namespace {

class RemoteServerTest : public ::testing::Test {
protected:
    std::filesystem::path dir;
    std::unique_ptr<LocalServer> local;
    std::unique_ptr<ApiServer> api;
    std::unique_ptr<RemoteServer> remote;

    void SetUp() override {
        dir = testutil::tempDir() / "remote-server" /
              (std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()) + "-" +
               std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        std::filesystem::create_directories(dir / "music");
        std::filesystem::create_directories(dir / "data");
        for (const auto& [artist, album] : {std::pair{"Amy", "First"}, {"Amy", "Second"}, {"Bea", "Third"}}) {
            const auto file = dir / "music" / (std::string(artist) + "-" + album + ".flac");
            std::filesystem::copy_file(testutil::testFile("16-44.1k"), file);
            TagLib::FLAC::File tags(file.c_str());
            tags.tag()->setArtist(artist);
            tags.tag()->setAlbum(album);
            tags.tag()->setTitle(std::string(album) + " Song");
            ASSERT_TRUE(tags.save());
        }
        {
            LibraryStore store(dir / "data");
            LibraryScanner scanner(store);
            ASSERT_TRUE(scanner.scan(scanner.addRoot(dir / "music")).complete);
        }
        local = std::make_unique<LocalServer>(camel::Settings{}, dir / "data" / "settings.json");
        ApiServer::Options options;
        options.port = 0;
        options.loopbackOnly = true;
        options.serverId = "test-server";
        options.serverName = "Test PC";
        options.uploads = dir / "data" / "uploads";
        api = std::make_unique<ApiServer>(*local, options);
        remote = connect();
    }

    void TearDown() override {
        remote.reset();
        api.reset();
        if (local) local->shutdown();
        local.reset();
    }

    std::string token; // this test's Control point's, which claimed the Server

    std::unique_ptr<RemoteServer> connect(std::string with = {}) {
        if (with.empty()) {
            if (token.empty()) token = RemoteServer::pair("127.0.0.1", api->port(), "Test Control", "").token;
            with = token;
        }
        RemoteServer::Options options;
        options.host = "127.0.0.1";
        options.port = api->port();
        options.cache = dir / "cache";
        options.token = with;
        return std::make_unique<RemoteServer>(options);
    }

    // Waits until `remote` shows the local Server's latest state.
    void waitForSameState() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            if (toJson(remote->snapshot()) == toJson(local->snapshot())) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        FAIL() << "the remote snapshot never matched";
    }
};

// The client side of a WebSocket to `port`, past the handshake, to send what RemoteServer never would.
std::unique_ptr<remote::Connection> rawConnection(uint16_t port, const std::string& token) {
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    EXPECT_EQ(connectWithin(s, reinterpret_cast<sockaddr*>(&address), sizeof(address), std::chrono::seconds(2)),
              Reach::Connected);
    u_long blocking = 0;
    ioctlsocket(s, FIONBIO, &blocking);
    remote::sendAll(s, std::string("GET /api HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                   "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n") +
                           "Authorization: Bearer " + token + "\r\n\r\n");
    std::string rest;
    const auto head = remote::readHead(s, rest, 16384, std::chrono::seconds(2));
    EXPECT_TRUE(head && head->starts_with("HTTP/1.1 101"));
    return std::make_unique<remote::Connection>(s, true, rest);
}

} // namespace

// TC-R01: the window's calls answer the same through the Server API as from the Server itself.
TEST_F(RemoteServerTest, EveryCallAnswersAsTheServersOwn) {
    const IServer& server = *local;
    const auto page = server.browseTracks(0, 10, std::nullopt);
    ASSERT_EQ(page.total, 3);
    EXPECT_EQ(json(remote->browseTracks(0, 10, std::nullopt)), json(page));
    EXPECT_EQ(json(remote->searchTracks("song", 0, 10, page.revision)), json(server.searchTracks("song", 0, 10, page.revision)));
    EXPECT_EQ(json(remote->searchLibrary("Amy", 0, 10, std::nullopt)), json(server.searchLibrary("Amy", 0, 10, std::nullopt)));
    EXPECT_EQ(json(remote->browseAlbums(AlbumSort::Title, std::nullopt, 0, 10, std::nullopt)),
              json(server.browseAlbums(AlbumSort::Title, std::nullopt, 0, 10, std::nullopt)));
    const LibraryGroupFilter amy{LibraryGroupField::Artist, "Amy"};
    EXPECT_EQ(json(remote->browseAlbums(AlbumSort::Year, amy, 0, 10, std::nullopt)),
              json(server.browseAlbums(AlbumSort::Year, amy, 0, 10, std::nullopt)));
    EXPECT_EQ(json(remote->browseGroups(LibraryGroupField::Artist, false, 0, 10, std::nullopt)),
              json(server.browseGroups(LibraryGroupField::Artist, false, 0, 10, std::nullopt)));
    EXPECT_EQ(json(remote->groupInitials(LibraryGroupField::Artist)), json(server.groupInitials(LibraryGroupField::Artist)));
    EXPECT_EQ(json(remote->albumInitials(AlbumSort::Title)), json(server.albumInitials(AlbumSort::Title)));
    EXPECT_EQ(json(remote->creditParts({"Amy", "Bea"})), json(server.creditParts({"Amy", "Bea"})));
    EXPECT_EQ(json(remote->browseTrackRows(TrackSort::Title, true, amy, 0, 10, std::nullopt)),
              json(server.browseTrackRows(TrackSort::Title, true, amy, 0, 10, std::nullopt)));
    const auto album = server.browseAlbums(AlbumSort::Title, std::nullopt, 0, 1, std::nullopt).albums.at(0).id;
    EXPECT_EQ(remote->albumPosition(AlbumSort::Title, std::nullopt, false, album),
              server.albumPosition(AlbumSort::Title, std::nullopt, false, album));
    EXPECT_EQ(json(remote->artistPage(amy)), json(server.artistPage(amy)));
    EXPECT_EQ(json(remote->albumDetail(album)), json(server.albumDetail(album)));
    EXPECT_EQ(remote->albumTrackPaths({album}), server.albumTrackPaths({album}));
    EXPECT_EQ(json(remote->smartTracks(SmartList::RecentlyAdded, 0, 10, std::nullopt)),
              json(server.smartTracks(SmartList::RecentlyAdded, 0, 10, std::nullopt)));
    EXPECT_EQ(json(remote->browseFolder(std::nullopt)), json(server.browseFolder(std::nullopt)));
    EXPECT_EQ(json(remote->libraryRoots()), json(server.libraryRoots()));
    EXPECT_EQ(json(remote->libraryScanProgress()), json(server.libraryScanProgress()));
    EXPECT_EQ(json(remote->playStatistics({})), json(server.playStatistics({})));
    EXPECT_EQ(json(remote->fileDetails(page.tracks.at(0).path)), json(server.fileDetails(page.tracks.at(0).path)));
    EXPECT_EQ(json(remote->lyrics(page.tracks.at(0).path)), json(server.lyrics(page.tracks.at(0).path)));
    EXPECT_EQ(json(remote->dataRecovery()), json(server.dataRecovery()));

    // A change made through the API is the Server's own.
    PlaylistSource track;
    track.kind = PlaylistSource::Kind::Track;
    track.track = LibraryTrackKey{page.tracks.at(0).file};
    const auto created = remote->createPlaylist("Remote", {track});
    EXPECT_EQ(created.added, 1);
    EXPECT_EQ(json(remote->playlists()), json(server.playlists()));
    EXPECT_EQ(json(remote->playlistItems(created.playlist, 0, 10)), json(server.playlistItems(created.playlist, 0, 10)));
}

TEST_F(RemoteServerTest, ErrorsComeBackAsTheSameExceptions) {
    const auto page = remote->browseTracks(0, 10, std::nullopt);
    EXPECT_THROW(local->browseTracks(0, 10, page.revision + 7), LibraryRevisionChanged);
    EXPECT_THROW(remote->browseTracks(0, 10, page.revision + 7), LibraryRevisionChanged);
    EXPECT_THROW(remote->setArtistPicture("Amy", {1, 2, 3}), std::invalid_argument);
    CancellationToken cancelled;
    cancelled.cancel();
    EXPECT_THROW(remote->browseTracks(0, 10, std::nullopt, cancelled), std::runtime_error);
    EXPECT_TRUE(remote->connected()); // a call that failed leaves the connection open
}

TEST_F(RemoteServerTest, CommandsGoThroughAndEventsComeBack) {
    std::mutex mutex;
    std::condition_variable told;
    std::vector<Event> events;
    remote->setListener([&](const Event& event) {
        std::lock_guard lock(mutex);
        events.push_back(event);
        told.notify_all();
    });
    Command shuffle;
    shuffle.kind = CommandKind::SetShuffle;
    shuffle.shuffle = true;
    const auto stamp = remote->submit(shuffle);
    EXPECT_GT(stamp.id, 0u);
    std::unique_lock lock(mutex);
    EXPECT_TRUE(told.wait_for(lock, std::chrono::seconds(5), [&] {
        return std::ranges::any_of(events, [](const Event& e) { return e.snapshot.queue.shuffle; });
    }));
    lock.unlock();
    remote->waitIdle();
    waitForSameState();
    EXPECT_TRUE(local->snapshot().queue.shuffle);
    EXPECT_EQ(remote->hello()["server"]["name"], "Test PC");
}

// A queue's changes go out as what changed: a long queue arrives whole once, then as the entries that differ.
TEST_F(RemoteServerTest, AQueueArrivesWholeOnceAndThenAsWhatChanged) {
    const auto tracks = local->browseTracks(0, 10, std::nullopt).tracks;
    Command add;
    add.kind = CommandKind::QueueAdd;
    for (int i = 0; i < 2000; ++i) add.sources.emplace_back(tracks.at(size_t(i) % tracks.size()).path);
    local->submit(add);
    local->waitIdle();
    waitForSameState();
    EXPECT_EQ(remote->snapshot().queue.entries.size(), 2000u);
    Command one;
    one.kind = CommandKind::QueueAdd;
    one.sources.emplace_back(tracks.at(0).path);
    local->submit(one);
    local->waitIdle();
    waitForSameState();
    EXPECT_EQ(remote->snapshot().queue.entries.size(), 2001u);

    api::QueueEncoder encoder;
    api::QueueDecoder decoder;
    const auto full = toJson(local->snapshot());
    auto first = full;
    encoder.encode(first);
    auto same = full;
    encoder.encode(same);
    EXPECT_FALSE(same["zones"][0]["queue"].contains("entries"));
    EXPECT_TRUE(same["zones"][0]["queue"].contains("same"));
    decoder.decode(first);
    decoder.decode(same);
    EXPECT_EQ(same, full);
    auto shorter = full;
    shorter["zones"][0]["queue"]["entries"].erase(5);
    if (shorter.contains("queue")) shorter["queue"]["entries"].erase(5);
    const auto expected = shorter;
    encoder.encode(shorter);
    EXPECT_EQ(shorter["zones"][0]["queue"]["entries"].size(), 0u);
    EXPECT_EQ(shorter["zones"][0]["queue"]["keep"], json::array({5, 1995}));
    decoder.decode(shorter);
    EXPECT_EQ(shorter, expected);
    auto broken = json{{"zones", json::array({{{"id", "x"}, {"queue", {{"keep", {1, 1}}, {"entries", json::array()}}}}})}};
    EXPECT_THROW(api::QueueDecoder().decode(broken), std::invalid_argument);
}

TEST_F(RemoteServerTest, FilesGoOverHttp) {
    EXPECT_FALSE(remote->artwork({ArtworkKind::Artist, 0, {}, "Nobody"}));
    // A 2x2 24-bit BMP.
    std::vector<uint8_t> image = {'B', 'M', 70, 0, 0, 0, 0, 0,  0, 0, 54, 0, 0, 0, 40, 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 1,
                                  0,   24,  0,  0, 0, 0, 0, 16, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    image.resize(70, 0x80);
    EXPECT_TRUE(remote->setArtistPicture("Amy", image).chosen);
    const auto picture = remote->artwork({ArtworkKind::Artist, 0, {}, "Amy"});
    ASSERT_TRUE(picture);
    EXPECT_TRUE(std::filesystem::file_size(picture->path) > 0);
    EXPECT_EQ(picture->path.parent_path(), dir / "cache");

    const auto tracks = local->browseTracks(0, 10, std::nullopt).tracks;
    const auto playlist = dir / "list.m3u8";
    std::ofstream(playlist) << "#EXTM3U\n" << tracks.at(1).path << "\n";
    const auto imported = remote->importPlaylist(playlist);
    EXPECT_EQ(imported.added, 1);
    const auto exported = dir / "out.m3u8";
    remote->exportPlaylist(imported.playlist, exported);
    std::ifstream in(exported);
    std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    auto name = std::filesystem::path(tracks.at(1).path).filename().string();
    for (auto* s : {&text, &name}) std::ranges::transform(*s, s->begin(), [](unsigned char c) { return char(std::tolower(c)); });
    EXPECT_NE(text.find(name), std::string::npos) << text;

    const auto bundle = dir / "bundle.zip";
    remote->writeDiagnosticBundle(bundle, false, "9.9.9");
    EXPECT_GT(std::filesystem::file_size(bundle), 100u);
}

TEST_F(RemoteServerTest, ALostConnectionFailsTheCallsUnderWay) {
    std::atomic<bool> failed = false;
    std::thread calling([&] {
        try {
            for (;;) remote->browseTracks(0, 10, std::nullopt);
        } catch (const std::runtime_error&) {
            failed = true;
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    api.reset();
    calling.join();
    EXPECT_TRUE(failed);
    EXPECT_FALSE(remote->connected());
    EXPECT_THROW(remote->libraryRoots(), std::runtime_error);
}

TEST_F(RemoteServerTest, APortAnotherProgramHoldsMovesToTheNext) {
    const SOCKET held = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    ASSERT_EQ(bind(held, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(listen(held, 1), 0);
    int length = sizeof(address);
    getsockname(held, reinterpret_cast<sockaddr*>(&address), &length);
    ApiServer::Options options;
    options.port = ntohs(address.sin_port);
    options.portsToTry = 5;
    ApiServer next(*local, options);
    EXPECT_GT(next.port(), options.port);
    EXPECT_LE(next.port(), options.port + 4);
    options.portsToTry = 1;
    EXPECT_THROW(ApiServer(*local, options), std::runtime_error);
    closesocket(held);
}

// Messages that are not the API's close their own connection and harm nothing else.
TEST_F(RemoteServerTest, MalformedMessagesAreRefusedWithoutHarm) {
    for (const auto* message : {"not json", "[]", R"({"id":"x","call":"playlists"})", R"({"cancel":"x"})", R"({"call":"x"})"}) {
        const auto raw = rawConnection(api->port(), token);
        while (const auto pushed = raw->read(1 << 26))
            if (pushed->find("\"event\"") != std::string::npos) break;
        raw->send(message);
        std::optional<std::string> after;
        while ((after = raw->read(1 << 26))) {
        }
        EXPECT_FALSE(after) << message;
    }
    {
        // An unknown call and arguments that do not fit are answered as errors; the connection stays.
        const auto raw = rawConnection(api->port(), token);
        raw->send(R"({"id":1,"call":"noSuchCall","args":[]})");
        raw->send(R"({"id":2,"call":"browseTracks","args":["a"]})");
        int errors = 0;
        while (errors < 2) {
            const auto reply = raw->read(1 << 26);
            ASSERT_TRUE(reply);
            if (reply->find("invalidArgument") != std::string::npos) ++errors;
        }
    }
    {
        ApiSession session(*local, [](const std::string&) {}, json::object());
        EXPECT_THROW(session.receive("{"), std::invalid_argument);
        EXPECT_THROW(session.receive(R"({"id":1})"), std::invalid_argument);
        EXPECT_NO_THROW(session.receive(R"({"cancel":99})"));
    }
    // The other connection went on unharmed.
    EXPECT_EQ(remote->libraryRoots().size(), 1u);
    EXPECT_TRUE(remote->connected());
}

// TC-R02: without a paired device's token nothing is served, covers and uploads included; only pairing goes without.
TEST_F(RemoteServerTest, NothingIsServedBeforePairing) {
    EXPECT_THROW(connect("0123456789abcdef"), NotPaired);
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(api->port());
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(connectWithin(s, reinterpret_cast<sockaddr*>(&address), sizeof(address), std::chrono::seconds(2)),
              Reach::Connected);
    u_long blocking = 0;
    ioctlsocket(s, FIONBIO, &blocking);
    remote::sendAll(s, "GET /artwork?kind=artist&name=Amy HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    std::string rest;
    const auto head = remote::readHead(s, rest, 16384, std::chrono::seconds(2));
    closesocket(s);
    ASSERT_TRUE(head);
    EXPECT_TRUE(head->starts_with("HTTP/1.1 401")) << *head;
    // A later device needs a code, which the paired one is shown.
    try {
        RemoteServer::pair("127.0.0.1", api->port(), "Laptop", "");
        FAIL() << "paired without a code";
    } catch (const PairingRefused& e) {
        EXPECT_EQ(e.code, "codeNeeded");
    }
    try {
        RemoteServer::pair("127.0.0.1", api->port(), "Test Control", remote->pairingCode());
        FAIL() << "paired under a name taken";
    } catch (const PairingRefused& e) {
        EXPECT_EQ(e.code, "nameTaken");
        EXPECT_EQ(e.suggestion, "Test Control (2)");
    }
    const auto laptop = RemoteServer::pair("127.0.0.1", api->port(), "Laptop", remote->pairingCode());
    EXPECT_TRUE(connect(laptop.token)->connected());
    EXPECT_EQ(remote->pairedDevices().size(), 2u);
}

// A revoked device's live connection closes at once and its token stops working; the Server's own window and any
// paired device may revoke.
TEST_F(RemoteServerTest, ARevokedDeviceIsCutOffAtOnce) {
    const auto laptop = RemoteServer::pair("127.0.0.1", api->port(), "Laptop", api->pairingCode());
    const auto other = connect(laptop.token);
    ASSERT_TRUE(other->connected());
    EXPECT_TRUE(remote->revokeDevice(laptop.device));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (other->connected() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(other->connected());
    EXPECT_THROW(connect(laptop.token), NotPaired);
    EXPECT_TRUE(remote->connected());
    // The Server's own window revokes too.
    const auto tablet = RemoteServer::pair("127.0.0.1", api->port(), "Tablet", api->pairingCode());
    EXPECT_TRUE(api->revoke(tablet.device));
    EXPECT_THROW(connect(tablet.token), NotPaired);
}

// Reset owner from a Control point cuts every device off, itself among them, and the next to ask claims the Server.
TEST_F(RemoteServerTest, ResetOwnerFromAControlPointCutsItOffToo) {
    remote->resetOwner();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (remote->connected() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(remote->connected());
    EXPECT_THROW(connect(token), NotPaired);
    const auto claim = RemoteServer::pair("127.0.0.1", api->port(), "Laptop", "");
    EXPECT_FALSE(claim.token.empty());
}

// Five wrong codes stop pairing for five minutes, and the paired devices are told.
TEST_F(RemoteServerTest, FiveWrongCodesStopPairingAndThePairedDevicesAreTold) {
    std::mutex mutex;
    std::condition_variable told;
    std::optional<json> notice;
    remote->setNoticeListener([&](const json& n) {
        std::lock_guard lock(mutex);
        notice = n;
        told.notify_all();
    });
    const auto code = remote->pairingCode();
    for (int i = 0; i < 5; ++i) EXPECT_THROW(RemoteServer::pair("127.0.0.1", api->port(), "Laptop", "000000"), PairingRefused);
    try {
        RemoteServer::pair("127.0.0.1", api->port(), "Laptop", code);
        FAIL() << "paired while pairing is stopped";
    } catch (const PairingRefused& e) {
        EXPECT_EQ(e.code, "stopped");
        EXPECT_GT(e.until, 0);
    }
    std::unique_lock lock(mutex);
    ASSERT_TRUE(told.wait_for(lock, std::chrono::seconds(5), [&] { return notice.has_value(); }));
    EXPECT_EQ((*notice)["kind"], "pairingStopped");
}
