// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <future>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>

#include "ControlPoint/CommandSubmit.h"
#include "Server/Playback/LocalServer.h"
#include "Server/Remote/ApiServer.h"
#include "Server/Remote/RemoteServer.h"
#include "Server/Library/LibraryScanner.h"
#include "OutputBridge/CaptureOutput.h"
#include "Engine/SamplePacker.h"
#include "Common/util/Text.h"
#include "Engine/decode/IDecoder.h"
#include "LameFixture.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::audio;
using namespace camel::server;
using namespace std::chrono_literals;

namespace {

// Queues end where a test put their end; the autoplay tests turn it on.
Settings withoutAutoplay() {
    Settings s;
    s.autoplay = false;
    return s;
}

struct Probe {
    std::atomic<int> opens = 0, closes = 0;
    std::function<void()> onOpen;
    std::optional<Backend> failBackend;
    std::mutex renderMutex;
    IRenderSource* source = nullptr;
    DeviceFormat format;
};

class ManualOutput final : public IOutput {
public:
    explicit ManualOutput(Probe& probe) : probe_(probe) {}
    bool supports(const DeviceFormat&) override { return true; }
    void open(const DeviceFormat& format, IRenderSource* source) override {
        ++probe_.opens;
        if (probe_.onOpen) probe_.onOpen();
        std::lock_guard lock(probe_.renderMutex);
        probe_.format = format;
        probe_.source = source;
    }
    void start() override {}
    void pause() override {}
    void close() override {
        std::lock_guard lock(probe_.renderMutex);
        ++probe_.closes;
        probe_.source = nullptr;
    }
    OutputStatus status() const override { return {"Manual", 256, "Server test"}; }

private:
    Probe& probe_;
};

Command select(const char* name = "24-96k", CommandKind kind = CommandKind::OpenAndPlay,
               std::optional<DecoderKind> decoder = std::nullopt) {
    Command c;
    c.kind = kind;
    c.source.emplace(toUtf8(testutil::testFile(name).wstring()), FrameRange{}, decoder);
    return c;
}

Command command(CommandKind kind) {
    Command c;
    c.kind = kind;
    return c;
}

// The running test's name as a folder's, the parameter's "/Local" or "/Remote" not making a folder of its own.
std::string folderName() {
    std::string name = ::testing::UnitTest::GetInstance()->current_test_info()->name();
    std::ranges::replace(name, '/', '-');
    return name;
}

// The Server a test drives and, when asked for, the same Server as another computer's Control point sees it: its API on
// loopback and a paired RemoteServer (docs/Plan.md §17.8). The remote view goes before the Server does.
class ServerUnderTest {
public:
    ServerUnderTest() = default;
    ServerUnderTest(const ServerUnderTest&) = delete;
    ServerUnderTest& operator=(const ServerUnderTest&) = delete;
    ~ServerUnderTest() { reset(); }

    ServerUnderTest& operator=(std::unique_ptr<LocalServer> server) {
        dropRemote();
        local_ = std::move(server);
        return *this;
    }
    void reset() {
        dropRemote();
        local_.reset();
    }
    LocalServer* operator->() const { return local_.get(); }
    LocalServer& operator*() const { return *local_; }
    LocalServer* get() const { return local_.get(); }
    explicit operator bool() const { return local_ != nullptr; }

    // The Server itself, or through the network when `remote`; from any thread.
    IServer& api(bool remote) {
        if (!remote) return *local_;
        std::lock_guard lock(connecting_);
        if (!remote_) connect();
        return *remote_;
    }

private:
    void connect() {
        dir_ = testutil::tempDir() / "server-remote" /
               (folderName() + "-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        std::filesystem::create_directories(dir_);
        ApiServer::Options served;
        served.port = 0;
        served.portsToTry = 1;
        served.loopbackOnly = true;
        served.serverId = "test-server";
        served.serverName = "Test PC";
        served.pairings = dir_ / "pairings.json";
        served.uploads = dir_ / "uploads";
        api_ = std::make_unique<ApiServer>(*local_, served);
        RemoteServer::Options options;
        options.host = "127.0.0.1";
        options.port = api_->port();
        options.cache = dir_ / "cache";
        options.token = RemoteServer::pair("127.0.0.1", api_->port(), "Test Control", "").token;
        remote_ = std::make_unique<RemoteServer>(options);
    }
    void dropRemote() {
        remote_.reset();
        api_.reset();
    }

    std::unique_ptr<LocalServer> local_;
    std::unique_ptr<ApiServer> api_;
    std::unique_ptr<RemoteServer> remote_;
    std::filesystem::path dir_;
    std::mutex connecting_;
};

// Each Server test runs on the Server itself and again through the remote IServer, as a Control point on another computer
// drives it (TC-R01).
std::string throughName(const ::testing::TestParamInfo<bool>& info) {
    return info.param ? "Remote" : "Local";
}

class ServerTest : public ::testing::TestWithParam<bool> {
protected:
    Probe probe;
    ServerUnderTest server;
    IServer& api() { return server.api(GetParam()); }
    void SetUp() override {
        server = std::make_unique<LocalServer>(withoutAutoplay(), std::filesystem::path{}, [&](const Settings& settings) {
            if (probe.failBackend == settings.backend) throw std::runtime_error("injected output activation failure");
            return std::make_unique<ManualOutput>(probe);
        });
    }
    Snapshot send(Command c) {
        api().submit(std::move(c));
        api().waitIdle();
        return api().snapshot();
    }
};

TEST_P(ServerTest, LibraryWorkGoesOnWhilePlaybackIsAhead) {
    const auto dir = testutil::tempDir() / "server-library-playback";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "music");
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    scanner.addRoot(dir / "music");
    server = std::make_unique<LocalServer>(withoutAutoplay(), dir / "db" / "settings.json",
                                           [&](const Settings&) { return std::make_unique<ManualOutput>(probe); });
    const auto waitFor = [](const std::function<bool()>& check) {
        const auto end = std::chrono::steady_clock::now() + 8s;
        while (std::chrono::steady_clock::now() < end) {
            if (check()) return true;
            std::this_thread::sleep_for(20ms);
        }
        return false;
    };
    ASSERT_TRUE(waitFor(
        [&] { return store.openReader().scalar("SELECT COUNT(*) FROM roots WHERE last_scan IS NOT NULL").value() == 1; }));
    // The engine holds its ring full while the device takes nothing, so work on the same disk goes on, one file at a time.
    send(select());
    std::ofstream(dir / "music" / "lyrics.lrc") << "lyrics";
    ASSERT_TRUE(waitFor(
        [&] { return store.openReader().scalar("SELECT COUNT(*) FROM files WHERE content_hash IS NOT NULL").value() == 1; }));
    EXPECT_EQ(api().snapshot().playback.state, PlaybackState::Playing);
    server.reset();
}

TEST(TrackSourceTest, ValidatesPathRangeAndDecoderWithoutMutableAccess) {
    EXPECT_THROW(TrackSource(""), std::invalid_argument);
    EXPECT_THROW(TrackSource(std::string("a\0b", 3)), std::invalid_argument);
    EXPECT_THROW(TrackSource(std::string("\xff", 1)), std::invalid_argument);
    EXPECT_THROW(TrackSource("a", (FrameRange{10, 10})), std::invalid_argument);
    EXPECT_THROW(TrackSource("a", {}, DecoderKind(255)), std::invalid_argument);
    std::string path = "a.flac";
    TrackSource source(path, {10, 20}, DecoderKind::Sndfile);
    path = "b.flac";
    EXPECT_EQ(source.pathUtf8(), "a.flac");
    EXPECT_EQ(source.range().endExclusive, 20u);
    EXPECT_EQ(source.decoder(), DecoderKind::Sndfile);
}

TEST(SnapshotCursorTest, RejectsStaleRevisionCommandGenerationQueueAndVersion) {
    SnapshotCursor cursor;
    Snapshot s;
    EXPECT_TRUE(cursor.accept(s));
    EXPECT_FALSE(cursor.accept(s));
    cursor.expect({4, 2, 2});
    cursor.expect({1, 1, 1});
    s.revision = 10;
    s.command = {3, 2, 2};
    EXPECT_FALSE(cursor.accept(s));
    s.command = {4, 1, 2};
    EXPECT_FALSE(cursor.accept(s));
    s.command = {4, 2, 1};
    EXPECT_FALSE(cursor.accept(s));
    s.command = {4, 2, 2};
    s.apiVersion = kApiVersion + 1;
    EXPECT_FALSE(cursor.accept(s));
    s.apiVersion = kApiVersion;
    EXPECT_TRUE(cursor.accept(s));
    s.revision = 9;
    EXPECT_FALSE(cursor.accept(s));
    s.revision = 11;
    s.command.id = 3;
    EXPECT_FALSE(cursor.accept(s));
    EXPECT_EQ(cursor.current().value_or(Snapshot{}).revision, 10u);
}

TEST_P(ServerTest, RejectsInvalidCommandsBeforeAllocatingIds) {
    Command c;
    c.apiVersion = kApiVersion + 1;
    EXPECT_THROW(api().submit(c), std::invalid_argument);
    c = command(CommandKind::Load);
    EXPECT_THROW(api().submit(c), std::invalid_argument);
    c = select();
    c.kind = CommandKind::Play;
    EXPECT_THROW(api().submit(c), std::invalid_argument);
    c = command(CommandKind(255));
    EXPECT_THROW(api().submit(c), std::invalid_argument);
    c = command(CommandKind::Stop);
    c.frame = 10;
    EXPECT_THROW(api().submit(c), std::invalid_argument);
    c = select();
    c.source.emplace("a", FrameRange{1, 20});
    EXPECT_THROW(api().submit(c), std::invalid_argument);
    // Through the network the settings are read as a settings file is, each value put in range on the way, so none of
    // these reaches the Server to be refused.
    if (!GetParam()) {
        c = command(CommandKind::ApplySettings);
        c.settings = SettingsDto{};
        c.settings->ks.packetCount = 0;
        EXPECT_THROW(api().submit(c), std::invalid_argument);
        c.settings->ks.packetCount = 4;
        c.settings->backend = Backend(255);
        EXPECT_THROW(api().submit(c), std::invalid_argument);
    }
    EXPECT_EQ(api().submit(Command{}).id, 1u);
    api().waitIdle();
}

TEST_P(ServerTest, AssignsOrderedScopesAndDistinctQueueEntries) {
    auto s = send(select("16-44.1k", CommandKind::Load));
    EXPECT_EQ(s.command, (CommandStamp{1, 1, 1}));
    EXPECT_EQ(s.playback.state, PlaybackState::Stopped);
    ASSERT_TRUE(s.playback.source);
    EXPECT_EQ(s.playback.source.value_or(SourceInfo{}).bitsPerSample, 16u);
    EXPECT_EQ(probe.opens.load(), 0);
    const auto entry = s.queueEntryId;
    s = send(command(CommandKind::Play));
    EXPECT_EQ(s.command, (CommandStamp{2, 1, 1}));
    EXPECT_EQ(s.playback.state, PlaybackState::Playing);
    EXPECT_TRUE(s.playback.bitPerfect());
    EXPECT_EQ(s.playback.outputMode, "Manual");
    s = send(select("16-44.1k", CommandKind::Load));
    EXPECT_EQ(s.command, (CommandStamp{3, 2, 2}));
    EXPECT_NE(s.queueEntryId, entry);
    EXPECT_EQ(send(Command{}).command, (CommandStamp{4, 3, 2}));
}

TEST_P(ServerTest, SerializesPlaybackControlsAndSeek) {
    send(select());
    api().submit(command(CommandKind::Pause));
    Command seek = command(CommandKind::Seek);
    seek.frame = 12000;
    api().submit(seek);
    api().waitIdle();
    auto s = api().snapshot();
    EXPECT_EQ(s.playback.state, PlaybackState::Paused);
    EXPECT_EQ(s.playback.position, 12000u);
    EXPECT_FALSE(s.playback.error);
    EXPECT_EQ(send(command(CommandKind::TogglePause)).playback.state, PlaybackState::Playing);
    EXPECT_EQ(send(Command{}).playback.state, PlaybackState::Stopped);
    EXPECT_FALSE(send(command(CommandKind::ShowControlPanel)).controlPanelOpened.value_or(true));
}

TEST_P(ServerTest, ConcurrentProducersReceiveUniqueOrderedIds) {
    std::mutex mutex;
    std::vector<uint64_t> ids;
    auto producer = [&] {
        for (int i = 0; i < 25; ++i) {
            auto id = api().submit(command(CommandKind::Pause)).id;
            std::lock_guard lock(mutex);
            ids.push_back(id);
        }
    };
    std::thread a(producer), b(producer), c(producer), d(producer);
    a.join();
    b.join();
    c.join();
    d.join();
    api().waitIdle();
    std::sort(ids.begin(), ids.end());
    ASSERT_EQ(ids.size(), 100u);
    for (size_t i = 0; i < ids.size(); ++i) EXPECT_EQ(ids[i], i + 1);
    EXPECT_EQ(api().snapshot().command.id, 100u);
}

TEST_P(ServerTest, SuppressesBlockedOldGenerationAfterNewSelection) {
    std::promise<void> opened;
    std::atomic<bool> released = false;
    probe.onOpen = [&] {
        opened.set_value();
        released.wait(false);
    };
    std::mutex mutex;
    std::vector<Snapshot> events;
    struct DrainServer {
        ServerUnderTest& server;
        ~DrainServer() { server.reset(); }
    } drain{server};
    api().setListener([&](const Event& e) {
        std::lock_guard lock(mutex);
        events.push_back(e.snapshot);
    });
    struct Release {
        std::atomic<bool>& released;
        ~Release() {
            released = true;
            released.notify_all();
        }
    };
    {
        Release unblock{released};
        api().submit(select());
        ASSERT_EQ(opened.get_future().wait_for(10s), std::future_status::ready);
        const auto latest = api().submit(select("16-44.1k", CommandKind::Load));
        EXPECT_EQ(latest.generation, 2u);
    }
    api().waitIdle();
    api().setListener({});
    ASSERT_FALSE(events.empty());
    for (const auto& e : events) EXPECT_EQ(e.command.generation, 2u);
    EXPECT_EQ(api().snapshot().playback.pathUtf8, toUtf8(testutil::testFile("16-44.1k").wstring()));
}

TEST_P(ServerTest, ListenerMayReadSnapshotAndSubmitButExceptionsAreIsolated) {
    std::atomic<int> callbacks = 0;
    api().setListener([&](const Event& e) {
        EXPECT_GE(api().snapshot().revision, e.snapshot.revision);
        if (++callbacks == 1) api().submit(command(CommandKind::Pause));
        throw std::runtime_error("injected listener failure");
    });
    api().submit(select());
    api().waitIdle();
    api().waitIdle();
    EXPECT_EQ(api().snapshot().playback.state, PlaybackState::Paused);
    api().setListener({});
    EXPECT_GE(callbacks.load(), 2);
    EXPECT_EQ(send(Command{}).playback.state, PlaybackState::Stopped);
}

TEST_P(ServerTest, FileErrorsRemainVisibleAndNextSelectionRecovers) {
    Command c = command(CommandKind::OpenAndPlay);
    c.source.emplace(toUtf8((testutil::tempDir() / "server-missing.flac").wstring()));
    auto s = send(c);
    ASSERT_TRUE(s.playback.error);
    EXPECT_EQ(s.playback.error, ErrorCode::FileOpen);
    EXPECT_EQ(s.playback.state, PlaybackState::Stopped);
    s = send(select());
    EXPECT_FALSE(s.playback.error);
    EXPECT_EQ(s.playback.state, PlaybackState::Playing);
}

TEST_P(ServerTest, TrackDecoderOverrideSurvivesSettingsAndDoesNotChangeGlobalPreference) {
    auto s = send(select("24-96k", CommandKind::OpenAndPlay, DecoderKind::Sndfile));
    EXPECT_EQ(s.playback.decoder, DecoderKind::Sndfile);
    EXPECT_EQ(decoderFor(s.settings.decoderByFormat, AudioFormat::Flac), DecoderKind::Sndfile);
    send(command(CommandKind::Pause));
    Command c = command(CommandKind::ApplySettings);
    c.settings = s.settings;
    c.settings->decoderByFormat[AudioFormat::Flac] = DecoderKind::DrFlac;
    s = send(c);
    EXPECT_EQ(s.playback.state, PlaybackState::Paused);
    EXPECT_EQ(s.playback.decoder, DecoderKind::Sndfile);
    EXPECT_EQ(decoderFor(s.settings.decoderByFormat, AudioFormat::Flac), DecoderKind::DrFlac);
    s = send(select());
    EXPECT_EQ(s.playback.decoder, DecoderKind::DrFlac);
    EXPECT_FALSE(s.playback.error);
}

TEST_P(ServerTest, UnrelatedSettingsDoNotRestartPlaybackAndRequestedStateIsSeparate) {
    auto s = send(select());
    ASSERT_EQ(s.playback.state, PlaybackState::Playing);
    EXPECT_EQ(s.settings.backend, Backend::Wasapi);
    EXPECT_EQ(s.playback.activeBackend, Backend::Wasapi);
    EXPECT_EQ(s.playback.activeDecoder, DecoderKind::Sndfile);
    const int opens = probe.opens.load();

    auto requested = s.settings;
    requested.language = "en-US";
    requested.asio.bufferSize = 512;
    requested.ks.packetCount = 8;
    Command c = command(CommandKind::ApplySettings);
    c.settings = requested;
    s = send(c);

    EXPECT_EQ(probe.opens.load(), opens);
    EXPECT_EQ(s.settings.language, "en-US");
    EXPECT_EQ(s.settings.asio.bufferSize, 512);
    EXPECT_EQ(s.settings.ks.packetCount, 8);
    EXPECT_EQ(s.playback.state, PlaybackState::Playing);
    EXPECT_EQ(s.playback.activeBackend, Backend::Wasapi);
    EXPECT_EQ(s.playback.activeDecoder, DecoderKind::Sndfile);
}

TEST_P(ServerTest, RequestedBackendRemainsVisibleWhenActivationFails) {
    auto s = send(select());
    ASSERT_EQ(s.playback.activeBackend, Backend::Wasapi);
    probe.failBackend = Backend::Ks;

    auto requested = s.settings;
    requested.backend = Backend::Ks;
    Command c = command(CommandKind::ApplySettings);
    c.settings = requested;
    send(c);
    s = send(select()); // in the zone selected now, Kernel Streaming's

    EXPECT_EQ(s.settings.backend, Backend::Ks);
    EXPECT_EQ(s.playback.backend, Backend::Ks);
    EXPECT_FALSE(s.playback.activeBackend);
    EXPECT_FALSE(s.playback.activeDecoder);
    EXPECT_FALSE(s.playback.output);
    EXPECT_EQ(s.playback.state, PlaybackState::Stopped);
    ASSERT_TRUE(s.playback.error);
}

TEST_P(ServerTest, DestructorClosesActiveOutputAndDrainsCommands) {
    send(select());
    api().submit(command(CommandKind::Pause));
    server.reset();
    EXPECT_GT(probe.closes.load(), 0);
}

// Settings that do not read back as they are are refused, the refusal naming the part that does not; the window gets a
// refusal as no command rather than as an exception, which ended it in a XAML event handler.
TEST(ServerRefusalTest, RefusedSettingsNameTheirPartAndDoNotReachTheWindowAsAnException) {
    LocalServer server(Settings{});
    Command c;
    c.kind = CommandKind::ApplySettings;
    auto settings = server.snapshot().settings;
    settings.volume.db = 5.0; // above 0 dB, read back as 0
    c.settings = settings;
    try {
        server.submit(c);
        ADD_FAILURE() << "accepted";
    } catch (const std::invalid_argument& e) {
        EXPECT_NE(std::string(e.what()).find("volume"), std::string::npos) << e.what();
    }
    EXPECT_NO_THROW(EXPECT_FALSE(ui::submitChecked(server, c)));
}

TEST(ServerPersistenceTest, SavesSettingsAndProtectsServerOwnedLastFile) {
    const auto path = testutil::tempDir() / "server-settings.json";
    std::filesystem::remove(path);
    LocalServer server(Settings{}, path);
    auto stale = server.snapshot().settings;
    const auto load = select("16-44.1k", CommandKind::Load);
    const auto loadPath = toUtf8(testutil::testFile("16-44.1k").wstring());
    server.submit(load);
    Command c = command(CommandKind::ApplySettings);
    c.settings = stale;
    c.settings->language = "en-US";
    server.submit(c);
    server.waitIdle();
    auto s = server.snapshot();
    EXPECT_FALSE(s.commandError);
    EXPECT_EQ(s.settings.lastFileUtf8, loadPath);
    auto saved = loadSettings(path);
    EXPECT_EQ(saved.language, "en-US");
    EXPECT_EQ(toUtf8(saved.lastFile), loadPath);
    std::filesystem::remove(path);
}

// A receiver's level is saved once its slider stops moving, and closing saves one not yet saved.
TEST(ServerPersistenceTest, AReceiverLevelIsSavedOnceItsSliderStops) {
    const auto path = testutil::tempDir() / "server-receiver-level.json";
    std::filesystem::remove(path);
    Settings settings = withoutAutoplay();
    settings.backend = Backend::AirPlay;
    settings.airplay.deviceId = "800A805D016B";
    settings.volume.mode = VolumeMode::Player;
    const auto saved = [&] {
        std::error_code ec;
        return std::filesystem::exists(path, ec) ? loadSettings(path).airplayLevels["800A805D016B"].level : 0u;
    };
    const auto step = [](LocalServer& server, uint32_t level) {
        Command c = command(CommandKind::SetPlayerVolume);
        c.playerVolume = level;
        server.submit(c);
    };
    {
        LocalServer server(settings, path);
        AirPlayFinderOptions none; // the network's receivers stay out of it
        none.queryHost = "127.0.0.1";
        none.queryPort = 9;
        none.groupPort = 0;
        server.setAirPlayFinderOptions(none);
        for (uint32_t level = 1; level <= 40; ++level) step(server, level);
        server.waitIdle();
        const auto end = std::chrono::steady_clock::now() + 5s;
        while (saved() != 40 && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(20ms);
        EXPECT_EQ(saved(), 40u);
        step(server, 41);
        server.waitIdle();
        server.shutdown();
    }
    EXPECT_EQ(saved(), 41u);
    std::filesystem::remove(path);
}

TEST(ServerPersistenceTest, ReportsSaveFailureWhileApplyingSettings) {
    LocalServer server(Settings{}, testutil::tempDir() / "missing-server-directory" / "settings.json");
    Command c = command(CommandKind::ApplySettings);
    c.settings = server.snapshot().settings;
    c.settings->language = "zh-TW";
    server.submit(c);
    server.waitIdle();
    ASSERT_TRUE(server.snapshot().commandError);
    EXPECT_EQ(server.snapshot().settings.language, "zh-TW");
}

TEST(ServerPersistenceTest, RestoresExistingSelectionWithoutAutoplayAndIgnoresMissingFile) {
    Settings settings;
    settings.lastFile = testutil::testFile("16-44.1k").wstring();
    {
        LocalServer server(settings);
        server.waitIdle();
        EXPECT_EQ(server.snapshot().playback.pathUtf8, toUtf8(settings.lastFile));
        EXPECT_EQ(server.snapshot().playback.state, PlaybackState::Stopped);
        EXPECT_FALSE(server.snapshot().playback.output);
    }
    settings.lastFile += L".missing";
    LocalServer server(settings);
    server.waitIdle();
    EXPECT_TRUE(server.snapshot().playback.pathUtf8.empty());
    EXPECT_FALSE(server.snapshot().playback.error);
}

TrackSource source(const char* name) {
    return TrackSource(toUtf8(testutil::testFile(name).wstring()));
}

TrackSource missingSource(const char* name) {
    return TrackSource(toUtf8((testutil::tempDir() / name).wstring()));
}

Command queueReplace(std::vector<TrackSource> sources, uint64_t index = 0) {
    Command c = command(CommandKind::QueueReplace);
    c.sources = std::move(sources);
    c.index = index;
    return c;
}

std::vector<uint64_t> entryIds(const Snapshot& s) {
    std::vector<uint64_t> ids;
    ids.reserve(s.queue.entries.size());
    for (const auto& e : s.queue.entries) ids.push_back(e.id);
    return ids;
}

TEST_P(ServerTest, RejectsInvalidQueuePayloads) {
    EXPECT_THROW(api().submit(queueReplace({})), std::invalid_argument);
    EXPECT_THROW(api().submit(queueReplace({source("24-96k")}, 1)), std::invalid_argument);
    EXPECT_THROW(api().submit(command(CommandKind::QueueReorder)), std::invalid_argument);
    EXPECT_THROW(api().submit(command(CommandKind::QueueSelect)), std::invalid_argument);
    EXPECT_THROW(api().submit(command(CommandKind::SetShuffle)), std::invalid_argument);
    Command c = command(CommandKind::SetRepeat);
    c.repeat = RepeatMode(7);
    EXPECT_THROW(api().submit(c), std::invalid_argument);
    c = command(CommandKind::Next);
    c.entryId = 1;
    EXPECT_THROW(api().submit(c), std::invalid_argument);
    c = queueReplace({TrackSource("a.flac", FrameRange{1, 2})});
    EXPECT_THROW(api().submit(c), std::invalid_argument);
    c = command(CommandKind::RemoveLibraryRoot);
    EXPECT_THROW(api().submit(c), std::invalid_argument);
    c.rootId = -1;
    EXPECT_THROW(api().submit(c), std::invalid_argument);
    c.kind = CommandKind::Stop;
    c.rootId = 1;
    EXPECT_THROW(api().submit(c), std::invalid_argument);
    EXPECT_EQ(api().snapshot().command.id, 0u);
}

TEST_P(ServerTest, PlayNextListsTracksRightAfterTheCurrentOne) {
    auto s = send(queueReplace({source("16-44.1k"), source("24-96k"), source("no-md5")}));
    const auto first = entryIds(s);
    Command next = command(CommandKind::QueuePlayNext);
    next.sources = {source("24-96k"), source("16-44.1k")};
    s = send(next);
    const auto ids = entryIds(s);
    ASSERT_EQ(ids.size(), 5u);
    EXPECT_EQ(ids[0], first[0]);
    EXPECT_EQ(ids[3], first[1]);
    EXPECT_EQ(ids[4], first[2]);
    EXPECT_EQ(s.queue.upcomingEntryId, ids[1]);
    EXPECT_EQ(send(command(CommandKind::Next)).queueEntryId, ids[1]);
    EXPECT_EQ(send(command(CommandKind::Next)).queueEntryId, ids[2]);
    EXPECT_EQ(send(command(CommandKind::Next)).queueEntryId, ids[3]);
    Command add = command(CommandKind::QueueAdd);
    add.sources = {source("no-md5")};
    const auto all = entryIds(send(add));
    ASSERT_EQ(all.size(), 6u);
    EXPECT_EQ(std::vector<uint64_t>(all.begin(), all.end() - 1), ids); // Add to queue still goes to the end
}

TEST_P(ServerTest, QueueNavigationReorderAndUpcomingEntry) {
    auto s = send(queueReplace({source("16-44.1k"), source("24-96k"), source("no-md5")}));
    const auto ids = entryIds(s);
    ASSERT_EQ(ids.size(), 3u);
    EXPECT_EQ(s.queueEntryId, ids[0]);
    EXPECT_EQ(s.queue.upcomingEntryId, ids[1]);
    EXPECT_EQ(s.playback.state, PlaybackState::Playing);
    EXPECT_EQ(s.track, std::optional<TrackSource>(s.queue.entries[0].source));

    EXPECT_EQ(send(command(CommandKind::Next)).queueEntryId, ids[1]);
    EXPECT_EQ(send(command(CommandKind::Previous)).queueEntryId, ids[0]);
    Command select = command(CommandKind::QueueSelect);
    select.entryId = ids[2];
    s = send(select);
    EXPECT_EQ(s.queueEntryId, ids[2]);
    EXPECT_FALSE(s.queue.upcomingEntryId);

    const auto revision = s.queue.revision;
    Command reorder = command(CommandKind::QueueReorder);
    reorder.entryIds = {ids[2], ids[0], ids[1]};
    s = send(reorder);
    EXPECT_GT(s.queue.revision, revision);
    EXPECT_EQ(entryIds(s), reorder.entryIds);
    EXPECT_EQ(s.queueEntryId, ids[2]);
    EXPECT_EQ(s.queue.upcomingEntryId, ids[0]);

    select.entryId = ids[1];
    send(select);
    s = send(command(CommandKind::Next));
    EXPECT_EQ(s.playback.state, PlaybackState::Stopped);
    EXPECT_EQ(s.queueEntryId, ids[1]);

    select.entryId = 999;
    s = send(select);
    ASSERT_TRUE(s.commandError);
    EXPECT_EQ(s.queueEntryId, ids[1]);
}

TEST_P(ServerTest, ShuffleAndRepeatAreReportedInQueueSnapshot) {
    auto s = send(queueReplace({source("16-44.1k"), source("24-96k")}));
    Command shuffle = command(CommandKind::SetShuffle);
    shuffle.shuffle = true;
    const auto revision = s.queue.revision;
    s = send(shuffle);
    EXPECT_TRUE(s.queue.shuffle);
    EXPECT_GT(s.queue.revision, revision);
    Command repeat = command(CommandKind::SetRepeat);
    repeat.repeat = RepeatMode::One;
    s = send(repeat);
    EXPECT_EQ(s.queue.repeat, RepeatMode::One);
    EXPECT_EQ(s.queue.upcomingEntryId, s.queueEntryId);
}

TEST_P(ServerTest, RepeatAllStopsWhenEveryEntryFails) {
    Command repeat = command(CommandKind::SetRepeat);
    repeat.repeat = RepeatMode::All;
    send(repeat);
    send(queueReplace({missingSource("queue-missing-1.flac"), missingSource("queue-missing-2.flac")}));
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    Snapshot s = api().snapshot();
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(100ms);
        const auto next = api().snapshot();
        if (next.playback.revision == s.playback.revision && next.queueEntryId == s.queueEntryId) break;
        s = next;
    }
    std::this_thread::sleep_for(300ms);
    const auto settled = api().snapshot();
    EXPECT_EQ(settled.playback.revision, s.playback.revision);
    EXPECT_EQ(settled.playback.state, PlaybackState::Stopped);
    EXPECT_EQ(settled.playback.error, ErrorCode::FileOpen);
    EXPECT_FALSE(settled.queue.upcomingEntryId);
}

TEST(ServerQueueCaptureTest, NaturalEndAdvancesAndSkipsTrackLocalFailures) {
    CaptureOptions options;
    options.file = testutil::tempDir() / "server-queue.raw";
    options.accepts = [](const DeviceFormat& f) { return f.type == SampleType::Int32; };
    {
        LocalServer server(withoutAutoplay(), {}, [&](const Settings&) { return createCaptureOutput(options); });
        Command c = queueReplace({source("16-44.1k"), missingSource("queue-missing.flac"), source("24-96k")});
        server.submit(c);
        server.waitIdle();
        const auto ids = entryIds(server.snapshot());
        ASSERT_EQ(ids.size(), 3u);
        const auto deadline = std::chrono::steady_clock::now() + 20s;
        Snapshot s = server.snapshot();
        while ((s.queueEntryId != ids[2] || s.playback.state != PlaybackState::Stopped) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(10ms);
            s = server.snapshot();
        }
        EXPECT_EQ(s.queueEntryId, ids[2]);
        EXPECT_EQ(s.playback.state, PlaybackState::Stopped);
        EXPECT_FALSE(s.playback.error) << s.playback.errorDetail;
        EXPECT_FALSE(s.queue.upcomingEntryId);
    }
    std::filesystem::remove(options.file);
}

// Counts device opens of a wrapped output, which a gapless sequence must not repeat.
class CountingOutput final : public IOutput {
public:
    CountingOutput(std::unique_ptr<IOutput> inner, std::atomic<int>& opens) : inner_(std::move(inner)), opens_(opens) {}
    bool supports(const DeviceFormat& f) override { return inner_->supports(f); }
    void open(const DeviceFormat& f, IRenderSource* source) override {
        ++opens_;
        inner_->open(f, source);
    }
    void start() override { inner_->start(); }
    void pause() override { inner_->pause(); }
    void close() override { inner_->close(); }
    OutputStatus status() const override { return inner_->status(); }

private:
    std::unique_ptr<IOutput> inner_;
    std::atomic<int>& opens_;
};

// Plays `sources` as one queue through a 32-bit capture device until the last entry stops.
Snapshot playQueueToEnd(std::vector<TrackSource> sources, const std::filesystem::path& capture, std::atomic<int>& opens) {
    CaptureOptions options;
    options.file = capture;
    options.accepts = [](const DeviceFormat& f) { return f.type == SampleType::Int32; };
    options.realtime = true; // queuing the following file takes polls; real playback leaves seconds for that
    LocalServer server(withoutAutoplay(), {},
                       [&](const Settings&) { return std::make_unique<CountingOutput>(createCaptureOutput(options), opens); });
    server.submit(queueReplace(std::move(sources)));
    server.waitIdle();
    const auto ids = entryIds(server.snapshot());
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    Snapshot s = server.snapshot();
    while ((s.queueEntryId != ids.back() || s.playback.state != PlaybackState::Stopped) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
        s = server.snapshot();
    }
    return s;
}

TEST(ServerGaplessTest, MatchingTracksConcatenateBitExactlyThroughOneDeviceOpen) {
    const std::vector<std::filesystem::path> files = {testutil::testFile("24-96k"),
                                                      testutil::generatedFile({"gapless-short-24-96k", 96000, 24, 2, 0.3}),
                                                      testutil::generatedFile({"gapless-tail-24-96k", 96000, 24, 2, 0.7})};
    std::vector<int32_t> expected;
    std::vector<TrackSource> sources;
    for (const auto& file : files) {
        const auto reference = testaudio::referenceDecode(file);
        ASSERT_TRUE(reference.error.empty()) << reference.error;
        expected.insert(expected.end(), reference.samples.begin(), reference.samples.end());
        sources.emplace_back(toUtf8(file.wstring()));
    }
    const auto capture = testutil::tempDir() / "server-gapless.raw";
    std::atomic<int> opens = 0;
    const auto s = playQueueToEnd(sources, capture, opens);
    EXPECT_EQ(s.playback.state, PlaybackState::Stopped);
    EXPECT_FALSE(s.playback.error) << s.playback.errorDetail;
    EXPECT_EQ(s.queueEntryId, s.queue.entries.back().id);
    EXPECT_EQ(opens.load(), 1);

    std::ifstream in(capture, std::ios::binary | std::ios::ate);
    ASSERT_TRUE(in);
    ASSERT_EQ(in.tellg(), std::streamoff(expected.size() * 4));
    in.seekg(0);
    std::vector<uint8_t> bytes(expected.size() * 4);
    ASSERT_TRUE(in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size())));
    in.close();
    std::vector<int32_t> samples(expected.size());
    unpackSamples(bytes.data(), samples.size(), 24, SampleType::Int32, samples.data());
    EXPECT_TRUE(samples == expected);
    std::filesystem::remove(capture);
}

// Library files played in real time through a 32-bit capture device, for play-statistics checks (docs/Plan.md §7.8).
class LibraryPlaybackTest : public ::testing::TestWithParam<bool> {
protected:
    std::filesystem::path dir;
    std::unique_ptr<LibraryStore> store;
    std::unique_ptr<LibraryScanner> scanner;
    ServerUnderTest server;
    IServer& api() { return server.api(GetParam()); }
    int64_t root = 0;
    CaptureOptions options;

    std::filesystem::path setUp(const std::vector<std::filesystem::path>& files) {
        dir = testutil::tempDir() / "server-play" /
              (folderName() + "-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        std::filesystem::create_directories(dir / "music");
        for (const auto& file : files) std::filesystem::copy_file(file, dir / "music" / file.filename());
        store = std::make_unique<LibraryStore>(dir / "db");
        scanner = std::make_unique<LibraryScanner>(*store);
        root = scanner->addRoot(dir / "music");
        EXPECT_TRUE(scanner->scan(root).complete);
        options.file = dir / "capture.raw";
        options.accepts = [](const DeviceFormat& f) { return f.type == SampleType::Int32; };
        options.realtime = true;
        server = std::make_unique<LocalServer>(withoutAutoplay(), dir / "db" / "settings.json",
                                               [&](const Settings&) { return createCaptureOutput(options); });
        return dir / "music";
    }
    void TearDown() override {
        server.reset();
        scanner.reset();
        store.reset();
        std::error_code ignored;
        std::filesystem::remove_all(dir, ignored);
    }

    Snapshot send(Command c) {
        api().submit(std::move(c));
        api().waitIdle();
        return api().snapshot();
    }
    bool waitFor(const std::function<bool()>& check, std::chrono::milliseconds timeout = 15s) {
        const auto end = std::chrono::steady_clock::now() + timeout;
        while (!check()) {
            if (std::chrono::steady_clock::now() >= end) return false;
            std::this_thread::sleep_for(10ms);
        }
        return true;
    }
    bool stopped() { return api().snapshot().playback.state == PlaybackState::Stopped; }
    // Plays are persisted through the Server's own writer, so tests wait for them rather than for a barrier.
    PlayStatistics plays(int64_t expected) {
        PlayStatistics result;
        EXPECT_TRUE(waitFor([&] { return (result = api().playStatistics({})).historyTotal >= expected; }));
        return result;
    }
    int64_t file(const std::filesystem::path& path) {
        auto row = store->openReader().prepare("SELECT id FROM files WHERE path LIKE ? AND available=1");
        return row.bind(1, "%" + toUtf8(path.filename().wstring())).step() ? row.integer(0) : 0;
    }
    TrackSource track(const std::filesystem::path& path) {
        return TrackSource(toUtf8((dir / "music" / path.filename()).wstring()));
    }
};

// The window's background reads go on while the Server closes; the parts they read are never freed under them.
// A use after free shows under ASan.
TEST_P(LibraryPlaybackTest, ReadsWhileTheServerClosesFindItsPartsOrNothing) {
    const auto music = setUp({testutil::generatedFile({"closing-reads", 44100, 16, 2, 0.5})});
    const std::string path = toUtf8((music / "closing-reads.flac").wstring());
    std::atomic<bool> closed = false;
    std::atomic<int> reads = 0;
    constexpr int kReaders = 4;
    std::vector<std::thread> readers;
    readers.reserve(kReaders);
    for (int r = 0; r < kReaders; ++r)
        readers.emplace_back([&] {
            while (!closed) {
                try {
                    (void)api().loudnessProgress();
                    (void)api().discordStatus();
                    api().setDiscordTexts({});
                    (void)api().waveform(path);
                    api().rereadLibrary();
                } catch (const std::exception&) {
                    // a closed library may refuse a read; it must not crash
                }
                ++reads;
            }
        });
    std::this_thread::sleep_for(200ms);
    server->shutdown(); // the Server's own, whichever way the reads come
    std::this_thread::sleep_for(50ms);
    closed = true;
    for (auto& reader : readers) reader.join();
    EXPECT_GT(reads.load(), 0);
}

TEST_P(LibraryPlaybackTest, AnAddedRootIsIndexedPromptlyAndBrowsable) {
    setUp({});
    const auto second = dir / "second";
    std::filesystem::create_directories(second / "album");
    std::filesystem::copy_file(testutil::generatedFile({"added-root", 48000, 16, 2, 0.2}), second / "album" / "a.flac");
    Command add = command(CommandKind::AddLibraryRoot);
    EXPECT_THROW(api().submit(add), std::invalid_argument);
    add.path = toUtf8(second.wstring());
    ASSERT_FALSE(send(add).commandError);
    EXPECT_EQ(api().libraryRoots().size(), 2u);
    // The monitor rereads roots on its next pass rather than after its 60-second availability interval.
    // Tracks and their albums appear with each written batch.
    ASSERT_TRUE(waitFor([&] { return api().browseAlbums(AlbumSort::Title, std::nullopt, 0, 10, std::nullopt).total == 1; }, 20s));
    EXPECT_EQ(api().browseTrackRows(TrackSort::Title, false, std::nullopt, 0, 10, std::nullopt).total, 1);
    const auto albums = api().browseAlbums(AlbumSort::Title, std::nullopt, 0, 10, std::nullopt);
    EXPECT_EQ(api().albumDetail(albums.albums[0].id).tracks.size(), 1u);
    const auto roots = api().browseFolder(std::nullopt);
    EXPECT_EQ(roots.folders.size(), 2u);
    Command missing = command(CommandKind::AddLibraryRoot);
    missing.path = toUtf8((dir / "does-not-exist").wstring());
    const auto refused = send(missing); // added beside the command thread, so it says how in `finished`
    EXPECT_FALSE(refused.commandError);
    EXPECT_EQ(refused.finished.command, refused.command.id);
    EXPECT_TRUE(refused.finished.failure);
}

TEST_P(LibraryPlaybackTest, RemovingARootTakesItsTracksOutOfTheQueue) {
    const auto a = testutil::generatedFile({"remove-root", 48000, 16, 2, 0.3});
    setUp({a});
    const auto outside = toUtf8(testutil::testFile("16-44.1k").wstring());
    send(queueReplace({track(a), TrackSource(outside)}));
    send(command(CommandKind::Stop));
    EXPECT_EQ(api().libraryRootContents(root).tracks, 1);
    Command remove = command(CommandKind::RemoveLibraryRoot);
    remove.rootId = root;
    ASSERT_FALSE(send(remove).commandError);
    EXPECT_TRUE(api().libraryRoots().empty());
    ASSERT_TRUE(waitFor([&] { return api().snapshot().queue.entries.size() == 1; }));
    EXPECT_EQ(api().snapshot().queue.entries[0].source.pathUtf8(), outside);
}

TEST_P(LibraryPlaybackTest, ALibraryTrackPlaysUnderItsOnDiskSpelling) {
    const auto music = setUp({});
    std::filesystem::copy_file(testutil::generatedFile({"spelling", 48000, 16, 2, 0.3}), music / "Mixed Case.flac");
    ASSERT_TRUE(scanner->scan(root).complete);
    const auto rows = api().browseTrackRows(TrackSort::Title, false, std::nullopt, 0, 10, std::nullopt);
    ASSERT_EQ(rows.tracks.size(), 1u);
    ASSERT_NE(rows.tracks[0].path.find("mixed case.flac"), std::string::npos); // the lowercase index key
    send(queueReplace({TrackSource(rows.tracks[0].path)}));
    const auto path = api().snapshot().playback.pathUtf8;
    EXPECT_EQ(std::filesystem::path(toWide(path)).filename(), L"Mixed Case.flac");
    EXPECT_FALSE(path.starts_with(R"(\\?\)"));
    EXPECT_EQ(api().snapshot().queue.entries[0].source.pathUtf8(), path);
}

TEST_P(LibraryPlaybackTest, SavingTheQueueKeepsDuplicatesAndEntriesOutsideTheLibrary) {
    const auto a = testutil::generatedFile({"queue-save-a", 48000, 16, 2, 0.3});
    setUp({a});
    const auto outside = testutil::testFile("16-44.1k");
    send(queueReplace({track(a), TrackSource(toUtf8(outside.wstring())), track(a)}));
    send(command(CommandKind::Stop));
    const auto saved = api().saveQueueAsPlaylist("Queue");
    EXPECT_EQ(saved.added, 3);
    const auto page = api().playlistItems(saved.playlist, 0, 10);
    ASSERT_EQ(page.items.size(), 3u);
    EXPECT_EQ(page.items[0].track, (LibraryTrackKey{file(a)}));
    EXPECT_FALSE(page.items[1].track);
    EXPECT_EQ(page.items[1].source, toUtf8(outside.wstring()));
    EXPECT_EQ(page.items[2].track, page.items[0].track);
    EXPECT_TRUE(api().undoPlaylistChange(saved.undo));
    EXPECT_TRUE(api().playlists().empty());
}

// The queue is saved with the library file each entry carries, without asking the storage: saved while the storage is
// away, its tracks stay the library's.
TEST_P(LibraryPlaybackTest, SavingTheQueueWhileItsStorageIsAwayKeepsItsTracksInTheLibrary) {
    const auto a = testutil::generatedFile({"queue-save-away", 48000, 16, 2, 0.3});
    const auto music = setUp({a});
    send(queueReplace({track(a)}));
    send(command(CommandKind::Stop));
    const auto id = file(a); // before it goes: the folder watch may then mark it unavailable
    ASSERT_TRUE(waitFor([&] { return api().snapshot().queue.entries[0].file == id; }));
    std::filesystem::rename(music / a.filename(), dir / a.filename());
    const auto saved = api().saveQueueAsPlaylist("Away");
    const auto page = api().playlistItems(saved.playlist, 0, 10);
    ASSERT_EQ(page.items.size(), 1u);
    EXPECT_EQ(page.items[0].track, (LibraryTrackKey{id}));
}

// A library command waits for the library writer beside the command thread: while another holds the library, the
// Server answers the commands that follow.
TEST_P(LibraryPlaybackTest, ALibraryCommandHoldsUpNoOtherCommandWhileTheLibraryIsBusy) {
    const auto a = testutil::generatedFile({"busy-library", 48000, 16, 2, 0.3});
    setUp({a});
    std::promise<void> release;
    const auto released = release.get_future().share();
    auto holding = store->write([released](sqlite::Database&) { released.wait(); });
    std::this_thread::sleep_for(100ms);
    Command favorite = command(CommandKind::SetFavorite);
    favorite.tracks = {LibraryTrackKey{file(a)}};
    favorite.favorite = true;
    api().submit(std::move(favorite));
    Command shuffle = command(CommandKind::SetShuffle);
    shuffle.shuffle = true;
    const auto stamp = api().submit(std::move(shuffle));
    const bool answered = waitFor([&] { return api().snapshot().command.id >= stamp.id; }, 1000ms);
    release.set_value();
    holding.get();
    api().waitIdle();
    EXPECT_TRUE(answered);
}

TEST_P(LibraryPlaybackTest, TheQueueRestoresPausedAtItsPositionWithoutOpeningTheFileUntilPlay) {
    const auto a = testutil::generatedFile({"restore-a", 48000, 16, 2, 2.0});
    const auto b = testutil::generatedFile({"restore-b", 48000, 16, 2, 2.0});
    const auto c = testutil::generatedFile({"restore-c", 48000, 16, 2, 2.0});
    const auto music = setUp({a, b, c});
    send(queueReplace({track(a), track(b), track(c)}, 1));
    send(command(CommandKind::Pause));
    Command seek = command(CommandKind::Seek);
    seek.frame = 48000;
    send(seek);
    Command repeat = command(CommandKind::SetRepeat);
    repeat.repeat = RepeatMode::All;
    const auto before = send(repeat);
    // Saved while the Server runs, so a crash would keep it too.
    ASSERT_TRUE(waitFor([&] {
        auto row = store->openReader().prepare("SELECT value FROM app_state WHERE key='session:wasapi:'");
        return row.step() && row.text(0).find("\"position\":48000") != std::string::npos;
    }));
    server.reset();
    // The restore reads only the library database: the current file need not even be reachable.
    std::filesystem::rename(music / b.filename(), music / "away.flac");
    server = std::make_unique<LocalServer>(withoutAutoplay(), dir / "db" / "settings.json",
                                           [&](const Settings&) { return createCaptureOutput(options); });
    api().waitIdle();
    auto s = api().snapshot();
    ASSERT_EQ(s.queue.entries.size(), 3u);
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(s.queue.entries[i].source, before.queue.entries[i].source);
        EXPECT_EQ(s.queue.entries[i].title, before.queue.entries[i].title);
    }
    EXPECT_EQ(s.queueEntryId, s.queue.entries[1].id);
    EXPECT_EQ(s.queue.repeat, RepeatMode::All);
    EXPECT_EQ(s.playback.state, PlaybackState::Paused);
    EXPECT_EQ(s.playback.pathUtf8, before.playback.pathUtf8);
    EXPECT_EQ(s.playback.position, 48000u);
    EXPECT_EQ(s.playback.total, before.playback.total);
    ASSERT_TRUE(s.playback.source);
    EXPECT_EQ(s.playback.source->sampleRate, 48000u);
    EXPECT_FALSE(s.playback.output);
    EXPECT_FALSE(s.playback.error);
    // The startup scan of the restarted Server may still be reading the renamed file.
    ASSERT_TRUE(waitFor([&] {
        std::error_code error;
        std::filesystem::rename(music / "away.flac", music / b.filename(), error);
        return !error;
    }));
    seek.frame = 24000;
    EXPECT_EQ(send(seek).playback.position, 24000u);
    send(command(CommandKind::Play));
    ASSERT_TRUE(waitFor([&] { return api().snapshot().playback.state == PlaybackState::Playing; }));
    s = api().snapshot();
    EXPECT_EQ(s.queueEntryId, s.queue.entries[1].id);
    EXPECT_GE(s.playback.position, 24000u);
    EXPECT_LT(s.playback.position, 72000u);
    EXPECT_TRUE(s.playback.output);
    send(command(CommandKind::Stop));
}

// A save of the queue that fails, as on a full disk, is not taken for saved: the next save writes the entries again.
TEST_P(LibraryPlaybackTest, AQueueSaveThatFailedIsWrittenAgain) {
    std::vector<std::filesystem::path> files;
    files.reserve(5);
    for (const char* name : {"save-a", "save-b", "save-c", "save-d", "save-e"})
        files.push_back(testutil::generatedFile({name, 48000, 16, 2, 0.5}));
    setUp(files);
    const auto rows = [&] {
        std::vector<std::string> paths;
        auto row = store->openReader().prepare("SELECT path FROM queue ORDER BY position");
        while (row.step()) paths.push_back(std::filesystem::path(toWide(row.text(0))).filename().string());
        return paths;
    };
    send(queueReplace({track(files[0]), track(files[1])}));
    send(command(CommandKind::Stop));
    ASSERT_TRUE(waitFor([&] { return rows().size() == 2; }));
    store
        ->write([](sqlite::Database& db) {
            db.exec("CREATE TRIGGER fail_queue BEFORE INSERT ON queue BEGIN SELECT RAISE(ABORT, 'disk full'); END");
        })
        .get();
    send(queueReplace({track(files[2]), track(files[3]), track(files[4])}));
    send(command(CommandKind::Stop));
    std::this_thread::sleep_for(1s); // the save is made and fails
    ASSERT_EQ(rows().size(), 2u);
    store->write([](sqlite::Database& db) { db.exec("DROP TRIGGER fail_queue"); }).get();
    Command repeat = command(CommandKind::SetRepeat);
    repeat.repeat = RepeatMode::All;
    send(repeat);
    server.reset();
    EXPECT_EQ(rows(), (std::vector<std::string>{"save-c.flac", "save-d.flac", "save-e.flac"}));
}

TEST_P(LibraryPlaybackTest, AnEntryWhoseFileIsGoneIsKeptAndPlaySkipsIt) {
    const auto a = testutil::generatedFile({"restore-gone-a", 48000, 16, 2, 0.5});
    const auto b = testutil::generatedFile({"restore-gone-b", 48000, 16, 2, 0.5});
    const auto music = setUp({a, b});
    store->write([](sqlite::Database& db) { db.exec("UPDATE tracks SET title='Kept title',artist='Kept artist'"); }).get();
    send(queueReplace({track(a), track(b)}));
    send(command(CommandKind::Stop));
    server.reset();
    std::filesystem::remove(music / a.filename());
    ASSERT_TRUE(scanner->scan(root).complete);
    store
        ->write([](sqlite::Database& db) {
            db.exec("DELETE FROM tracks WHERE file_id IN (SELECT id FROM files WHERE path LIKE '%restore-gone-a%')");
        })
        .get();
    server = std::make_unique<LocalServer>(withoutAutoplay(), dir / "db" / "settings.json",
                                           [&](const Settings&) { return createCaptureOutput(options); });
    api().waitIdle();
    auto s = api().snapshot();
    ASSERT_EQ(s.queue.entries.size(), 2u);
    EXPECT_EQ(s.queueEntryId, s.queue.entries[0].id);
    EXPECT_EQ(s.queue.entries[0].title, "Kept title · Kept artist"); // what it showed, although the file is gone
    EXPECT_EQ(s.queue.entries[1].title, "Kept title");
    send(command(CommandKind::Play));
    s = api().snapshot();
    EXPECT_EQ(s.queueEntryId, s.queue.entries[1].id);
    send(command(CommandKind::Stop));
}

TEST_P(LibraryPlaybackTest, AnUntitledTrackKeepsItsFileNameTitleAfterARestart) {
    const auto a = testutil::generatedFile({"untitled-gone", 48000, 16, 2, 0.5});
    const auto b = testutil::generatedFile({"untitled-kept", 48000, 16, 2, 0.5});
    const auto music = setUp({a, b});
    store->write([](sqlite::Database& db) { db.exec("UPDATE tracks SET title='',artist='Artist'"); }).get();
    send(queueReplace({track(a), track(b)}));
    send(command(CommandKind::Stop));
    server.reset();
    std::filesystem::remove(music / a.filename());
    ASSERT_TRUE(scanner->scan(root).complete);
    store
        ->write([](sqlite::Database& db) {
            db.exec("DELETE FROM tracks WHERE file_id IN (SELECT id FROM files WHERE path LIKE '%untitled-gone%')");
        })
        .get();
    server = std::make_unique<LocalServer>(withoutAutoplay(), dir / "db" / "settings.json",
                                           [&](const Settings&) { return createCaptureOutput(options); });
    api().waitIdle();
    const auto s = api().snapshot();
    ASSERT_EQ(s.queue.entries.size(), 2u);
    EXPECT_EQ(s.queue.entries[0].title, toUtf8(a.stem().wstring()) + " · Artist"); // the file is gone: what it showed
    EXPECT_EQ(s.queue.entries[1].title, "");                                       // the control point shows the file name
    EXPECT_EQ(s.queue.entries[1].artist, "Artist");
}

TEST_P(LibraryPlaybackTest, AutoplayFollowsTheLastEntryWithAnUnplayedLibraryTrack) {
    const auto a = testutil::generatedFile({"auto-a", 48000, 16, 2, 0.6});
    const auto b = testutil::generatedFile({"auto-b", 48000, 16, 2, 0.6});
    const auto c = testutil::generatedFile({"auto-c", 48000, 16, 2, 0.6});
    setUp({a, b, c});
    server = std::make_unique<LocalServer>(Settings{}, dir / "db" / "settings.json",
                                           [&](const Settings&) { return createCaptureOutput(options); });
    store
        ->write([&](sqlite::Database& db) {
            db.prepare("INSERT INTO play_events(file_id,started,utc_offset,heard_ms,counted) VALUES(?,?,0,1000,1)")
                .bind(1, file(b))
                .bind(2, int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count()))
                .run();
        })
        .get();
    send(queueReplace({track(a)}));
    ASSERT_TRUE(waitFor([&] { return api().snapshot().queue.entries.size() == 2; }));
    auto s = api().snapshot();
    EXPECT_FALSE(s.queue.entries[0].automatic);
    EXPECT_TRUE(s.queue.entries[1].automatic);
    EXPECT_NE(s.queue.entries[1].source.pathUtf8().find("auto-c"), std::string::npos); // b was played this week
    EXPECT_EQ(s.queue.upcomingEntryId, s.queue.entries[1].id);
    ASSERT_TRUE(waitFor([&] { return api().snapshot().queueEntryId == s.queue.entries[1].id; }));
    // Every track was played or queued: the one played longest ago follows.
    ASSERT_TRUE(waitFor([&] { return api().snapshot().queue.entries.size() == 3; }));
    EXPECT_NE(api().snapshot().queue.entries[2].source.pathUtf8().find("auto-b"), std::string::npos);
    send(command(CommandKind::Stop));
}

// Autoplay picks one library track after another; when none of them plays, as with conversion off and a device that
// takes none of the library's formats, it gives up after three failures in a row, with the last one's error shown.
TEST_P(LibraryPlaybackTest, AutoplayStopsAfterThreeTracksInARowFail) {
    std::vector<std::filesystem::path> files;
    files.reserve(8);
    for (int i = 0; i < 8; ++i)
        files.push_back(testutil::generatedFile({"autoplay-fails-" + std::to_string(i), 48000, 16, 2, 0.5}));
    setUp(files);
    Settings settings;
    settings.convertWhenUnsupported = false;
    options.accepts = [](const DeviceFormat&) { return false; };
    server = std::make_unique<LocalServer>(settings, dir / "db" / "settings.json",
                                           [&](const Settings&) { return createCaptureOutput(options); });
    send(queueReplace({track(files[0])}));
    size_t entries = 0;
    for (int still = 0; still < 20; std::this_thread::sleep_for(100ms)) {
        const auto now = api().snapshot().queue.entries.size();
        still = now == entries ? still + 1 : 0;
        entries = now;
    }
    const auto s = api().snapshot();
    EXPECT_EQ(s.queue.entries.size(), 3u);
    EXPECT_EQ(s.playback.state, PlaybackState::Stopped);
    EXPECT_TRUE(s.playback.error);
}

TEST_P(LibraryPlaybackTest, AutoplayYieldsToAddedTracksRepeatAndTheSwitch) {
    const auto a = testutil::generatedFile({"yield-a", 48000, 16, 2, 3.0});
    const auto b = testutil::generatedFile({"yield-b", 48000, 16, 2, 3.0});
    const auto c = testutil::generatedFile({"yield-c", 48000, 16, 2, 3.0});
    setUp({a, b, c});
    server = std::make_unique<LocalServer>(Settings{}, dir / "db" / "settings.json",
                                           [&](const Settings&) { return createCaptureOutput(options); });
    send(queueReplace({track(a)}));
    ASSERT_TRUE(waitFor([&] { return api().snapshot().queue.entries.size() == 2; }));
    Command add = command(CommandKind::QueueAdd);
    add.sources = {track(b)};
    auto s = send(add);
    ASSERT_EQ(s.queue.entries.size(), 2u); // the automatic entry made way for the user's track
    EXPECT_FALSE(s.queue.entries[1].automatic);
    EXPECT_EQ(s.queue.entries[1].source, track(b));

    Command repeat = command(CommandKind::SetRepeat);
    repeat.repeat = RepeatMode::All;
    send(repeat);
    send(command(CommandKind::Next));
    ASSERT_TRUE(waitFor([&] { return api().snapshot().queueEntryId == api().snapshot().queue.entries[1].id; }));
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(api().snapshot().queue.entries.size(), 2u); // repeat all never ends the queue

    Command off = command(CommandKind::ApplySettings);
    auto settings = api().snapshot().settings;
    settings.autoplay = false;
    off.settings = settings;
    send(off);
    repeat.repeat = RepeatMode::Off;
    send(repeat);
    send(command(CommandKind::Next));
    EXPECT_EQ(api().snapshot().queue.entries.size(), 2u);
    EXPECT_EQ(api().snapshot().playback.state, PlaybackState::Stopped);
}

TEST_P(LibraryPlaybackTest, PlayOnAnEmptyQueueStartsAutoplayAndItsEntriesSurviveARestart) {
    const auto a = testutil::generatedFile({"empty-a", 48000, 16, 2, 2.0});
    setUp({a});
    server = std::make_unique<LocalServer>(Settings{}, dir / "db" / "settings.json",
                                           [&](const Settings&) { return createCaptureOutput(options); });
    auto s = send(command(CommandKind::Play));
    ASSERT_EQ(s.queue.entries.size(), 1u);
    EXPECT_TRUE(s.queue.entries[0].automatic);
    EXPECT_EQ(s.queueEntryId, s.queue.entries[0].id);
    send(command(CommandKind::Pause));
    server.reset();
    server = std::make_unique<LocalServer>(Settings{}, dir / "db" / "settings.json",
                                           [&](const Settings&) { return createCaptureOutput(options); });
    api().waitIdle();
    s = api().snapshot();
    ASSERT_EQ(s.queue.entries.size(), 1u);
    EXPECT_TRUE(s.queue.entries[0].automatic);
}

TEST_P(LibraryPlaybackTest, ThePositionIsSavedWhilePlayingSoACrashLosesAtMostFiveSeconds) {
    const auto a = testutil::generatedFile({"restore-playing", 48000, 16, 2, 9.0});
    setUp({a});
    send(queueReplace({track(a)}));
    const auto saved = [&] {
        auto row = store->openReader().prepare("SELECT value FROM app_state WHERE key='session:wasapi:'");
        if (!row.step()) return uint64_t(0);
        const auto text = row.text(0);
        const auto at = text.find("\"position\":");
        return at == std::string::npos ? uint64_t(0) : std::stoull(text.substr(at + 11));
    };
    ASSERT_TRUE(waitFor([&] { return saved() >= uint64_t(48000) * 3; }, 8s));
    EXPECT_EQ(api().snapshot().playback.state, PlaybackState::Playing);
    send(command(CommandKind::Stop));
}

TEST_P(LibraryPlaybackTest, ShutdownSavesTheQueueAndLeavesTheServerReadable) {
    const auto a = testutil::generatedFile({"restore-shutdown", 48000, 16, 2, 2.0});
    setUp({a});
    send(queueReplace({track(a)}));
    send(command(CommandKind::Pause));
    Command seek = command(CommandKind::Seek);
    seek.frame = 12000;
    send(seek);
    // The Server's own: a Control point's shutdown() closes only its own connection.
    server->shutdown();
    server->shutdown();
    EXPECT_THROW(api().submit(command(CommandKind::Play)), std::runtime_error);
    EXPECT_EQ(api().browseTrackRows(TrackSort::Title, false, std::nullopt, 0, 10, std::nullopt).total, 1);
    auto row = store->openReader().prepare("SELECT value FROM app_state WHERE key='session:wasapi:'");
    ASSERT_TRUE(row.step());
    EXPECT_NE(row.text(0).find("\"position\":12000"), std::string::npos);
}

TEST_P(LibraryPlaybackTest, AClearedQueueStaysEmptyInsteadOfReopeningTheLastFile) {
    const auto a = testutil::generatedFile({"restore-cleared", 48000, 16, 2, 0.3});
    setUp({a});
    send(queueReplace({track(a)}));
    send(command(CommandKind::Stop));
    Command clear = command(CommandKind::QueueRemove);
    clear.entryIds = entryIds(api().snapshot());
    send(clear);
    server.reset();
    server = std::make_unique<LocalServer>(withoutAutoplay(), dir / "db" / "settings.json",
                                           [&](const Settings&) { return createCaptureOutput(options); });
    api().waitIdle();
    EXPECT_TRUE(api().snapshot().queue.entries.empty());
    EXPECT_TRUE(api().snapshot().playback.pathUtf8.empty());
}

// An entry saved with a decoder since removed, such as dr_mp3 (6) or minimp3 (8), restores with the setting's decoder.
TEST_P(LibraryPlaybackTest, AQueueSavedWithARemovedDecoderRestoresWithTheSetting) {
    const auto a = testutil::generatedFile({"restore-removed-a", 48000, 16, 2, 0.3});
    const auto b = testutil::generatedFile({"restore-removed-b", 48000, 16, 2, 0.3});
    setUp({a, b});
    send(queueReplace({TrackSource(track(a).pathUtf8(), {}, DecoderKind::DrFlac), track(b)}));
    send(command(CommandKind::Stop));
    server.reset();
    store->write([](sqlite::Database& db) { db.exec("UPDATE queue SET decoder=6+2*(position>0)"); }).get();
    server = std::make_unique<LocalServer>(withoutAutoplay(), dir / "db" / "settings.json",
                                           [&](const Settings&) { return createCaptureOutput(options); });
    api().waitIdle();
    const auto s = api().snapshot();
    ASSERT_EQ(s.queue.entries.size(), 2u);
    for (const auto& entry : s.queue.entries) EXPECT_FALSE(entry.source.decoder());
}

TEST_P(LibraryPlaybackTest, GaplessTransitionsRecordEachTrackOnceWithItsFullLength) {
    const auto a = testutil::generatedFile({"play-gapless-a", 48000, 16, 2, 0.6});
    const auto b = testutil::generatedFile({"play-gapless-b", 48000, 16, 2, 0.4});
    setUp({a, b});
    send(queueReplace({track(a), track(b)}));
    ASSERT_TRUE(waitFor([&] { return stopped() && api().snapshot().queueEntryId == api().snapshot().queue.entries.back().id; }));
    const auto statistics = plays(2);
    ASSERT_EQ(statistics.history.size(), 2u);
    EXPECT_EQ(statistics.history[0].track.file, file(b)); // newest first
    EXPECT_EQ(statistics.history[1].track.file, file(a));
    EXPECT_NEAR(double(statistics.history[0].heardMs), 400.0, 60.0);
    EXPECT_NEAR(double(statistics.history[1].heardMs), 600.0, 60.0);
    EXPECT_TRUE(statistics.history[0].counted && statistics.history[1].counted);
    EXPECT_EQ(statistics.overview.plays, 2);
}

TEST_P(LibraryPlaybackTest, EachRepeatOneLoopIsANewPlay) {
    const auto a = testutil::generatedFile({"play-repeat", 48000, 16, 2, 0.4});
    setUp({a});
    Command repeat = command(CommandKind::SetRepeat);
    repeat.repeat = RepeatMode::One;
    send(repeat);
    send(queueReplace({track(a)}));
    ASSERT_TRUE(waitFor([&] { return api().playStatistics({}).overview.plays >= 3; }));
    send(command(CommandKind::Stop));
    const auto statistics = api().playStatistics({});
    EXPECT_EQ(statistics.tracks.size(), 1u);
    EXPECT_GE(statistics.tracks[0].plays, 3);
    EXPECT_GE(statistics.historyTotal, 3);
}

TEST_P(LibraryPlaybackTest, ASeekForwardCreditsOnlyTheAudioActuallyHeard) {
    const auto a = testutil::generatedFile({"play-seek", 48000, 16, 2, 3.0});
    setUp({a});
    send(queueReplace({track(a)}));
    ASSERT_TRUE(waitFor([&] { return api().snapshot().playback.position >= 9600; })); // 0.2 s
    Command seek = command(CommandKind::Seek);
    seek.frame = 48000 * 27 / 10;
    send(seek);
    ASSERT_TRUE(waitFor([&] { return stopped(); }));
    const auto statistics = plays(1);
    ASSERT_EQ(statistics.history.size(), 1u);
    EXPECT_GT(statistics.history[0].heardMs, 300); // about 0.2 s before and 0.3 s after the seek
    EXPECT_LT(statistics.history[0].heardMs, 1000);
    EXPECT_FALSE(statistics.history[0].counted); // well below half of three seconds
}

TEST_P(LibraryPlaybackTest, ADecodeFailureKeepsOnlyTheAudioHeardBeforeIt) {
    // The engine stops as soon as decoding fails, discarding predecoded audio, so little of the file is ever heard.
    const auto truncated = testutil::tempDir() / "play-truncated.flac";
    testaudio::writeTruncated(testutil::generatedFile({"play-truncated-source", 48000, 16, 2, 6.0}), truncated, 0.9);
    setUp({truncated});
    send(queueReplace({track(truncated)}));
    ASSERT_TRUE(waitFor([&] { return stopped(); }));
    EXPECT_EQ(api().snapshot().playback.error, ErrorCode::DecodeFailed);
    waitFor([&] { return api().playStatistics({}).historyTotal > 0; }, 2s); // nothing heard records nothing
    const auto statistics = api().playStatistics({});
    ASSERT_LE(statistics.history.size(), 1u);
    for (const auto& play : statistics.history) {
        EXPECT_LT(play.heardMs, 1000); // the rest of the six seconds is not credited
        EXPECT_FALSE(play.counted);
    }
}

TEST_P(LibraryPlaybackTest, TheCountIsSavedWhilePlayingSoACrashCannotLoseIt) {
    const auto a = testutil::generatedFile({"play-durable", 48000, 16, 2, 6.0});
    setUp({a});
    send(queueReplace({track(a)}));
    const auto sql = "SELECT COUNT(*) FROM play_events WHERE counted=1 AND file_id=" + std::to_string(file(a));
    ASSERT_TRUE(waitFor([&] { return store->openReader().scalar(sql) == 1; }, 10s));
    EXPECT_EQ(api().snapshot().playback.state, PlaybackState::Playing); // persisted at 3 s, before the play ended
    send(command(CommandKind::Stop));
}

TEST_P(LibraryPlaybackTest, PlaysGoWithARemovedRootAndTheRootAddedAgainStartsAfresh) {
    const auto a = testutil::generatedFile({"play-root", 48000, 16, 2, 0.4});
    setUp({a});
    send(queueReplace({track(a)}));
    ASSERT_TRUE(waitFor([&] { return stopped(); }));
    ASSERT_EQ(plays(1).history.size(), 1u);
    Command remove = command(CommandKind::RemoveLibraryRoot);
    remove.rootId = root;
    ASSERT_FALSE(send(remove).commandError);
    ASSERT_TRUE(waitFor([&] { return api().playStatistics({}).history.empty(); }));
    EXPECT_EQ(api().playStatistics({}).overview.plays, 0);
    ASSERT_TRUE(scanner->scan(scanner->addRoot(dir / "music")).complete);
    EXPECT_NE(file(a), 0);
    EXPECT_TRUE(api().playStatistics({}).history.empty());
    EXPECT_EQ(store->openReader().scalar("SELECT COUNT(*) FROM stats"), 0);
}

INSTANTIATE_TEST_SUITE_P(Through, LibraryPlaybackTest, ::testing::Values(false, true), throughName);

std::vector<int32_t> concatenatedReferences(const std::vector<std::filesystem::path>& files) {
    std::vector<int32_t> samples;
    for (const auto& file : files) {
        const auto reference = testaudio::referenceDecode(file);
        EXPECT_TRUE(reference.error.empty()) << reference.error;
        samples.insert(samples.end(), reference.samples.begin(), reference.samples.end());
    }
    return samples;
}

std::vector<int32_t> capturedSamples(const std::filesystem::path& capture) {
    std::ifstream in(capture, std::ios::binary | std::ios::ate);
    if (!in) return {};
    std::vector<uint8_t> bytes(size_t(in.tellg()));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    std::vector<int32_t> samples(bytes.size() / 4);
    unpackSamples(bytes.data(), samples.size(), 24, SampleType::Int32, samples.data());
    return samples;
}

// A queue of over 2000 entries reads availability only when files or roots change, which removing a root does.
TEST_P(ServerTest, RemovingTheRootOfALargeQueueEmptiesIt) {
    const auto dir = testutil::tempDir() / "server-removed-root-large";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "music");
    const auto a = dir / "music" / "a.flac";
    std::filesystem::copy_file(testutil::generatedFile({"removed-root-large", 48000, 16, 2, 0.3}), a);
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(dir / "music");
    ASSERT_TRUE(scanner.scan(root).complete);
    server = std::make_unique<LocalServer>(withoutAutoplay(), dir / "db" / "settings.json",
                                           [&](const Settings&) { return std::make_unique<ManualOutput>(probe); });
    Command add = command(CommandKind::QueueAdd);
    add.sources = std::vector(2500, TrackSource(toUtf8(a.wstring())));
    ASSERT_EQ(send(add).queue.entries.size(), 2500u);
    Command remove = command(CommandKind::RemoveLibraryRoot);
    remove.rootId = root;
    ASSERT_FALSE(send(remove).commandError);
    const auto emptied = std::chrono::steady_clock::now() + 3s;
    while (!api().snapshot().queue.entries.empty() && std::chrono::steady_clock::now() < emptied)
        std::this_thread::sleep_for(10ms);
    EXPECT_TRUE(api().snapshot().queue.entries.empty());
    server.reset();
}

TEST_P(ServerTest, RemovingThePlayingTracksRootFinishesTheTrackAndEmptiesTheQueue) {
    const auto dir = testutil::tempDir() / "server-removed-root";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "music");
    const auto a = dir / "music" / "a.flac", b = dir / "music" / "b.flac";
    std::filesystem::copy_file(testutil::testFile("24-96k"), a);
    std::filesystem::copy_file(testutil::generatedFile({"removed-root-tail", 96000, 24, 2, 0.3}), b);
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(dir / "music");
    ASSERT_TRUE(scanner.scan(root).complete);
    server = std::make_unique<LocalServer>(withoutAutoplay(), dir / "db" / "settings.json",
                                           [&](const Settings&) { return std::make_unique<ManualOutput>(probe); });
    auto playing = send(queueReplace({TrackSource(toUtf8(a.wstring())), TrackSource(toUtf8(b.wstring()))}));
    const auto ids = entryIds(playing);
    std::vector<int32_t> heard;
    const auto pull = [&] {
        std::lock_guard lock(probe.renderMutex);
        if (!probe.source) return;
        std::vector<uint8_t> bytes(size_t(4096) * probe.format.bytesPerFrame());
        const auto got = probe.source->render(bytes.data(), 4096);
        std::vector<int32_t> samples(got * probe.format.channels);
        unpackSamples(bytes.data(), samples.size(), 24, probe.format.type, samples.data());
        heard.insert(heard.end(), samples.begin(), samples.end());
    };
    pull();
    std::this_thread::sleep_for(500ms);
    Command remove = command(CommandKind::RemoveLibraryRoot);
    remove.rootId = root;
    const auto removed = send(remove);
    ASSERT_FALSE(removed.commandError);
    EXPECT_EQ(removed.queueEntryId, ids[0]);
    EXPECT_EQ(removed.playback.state, PlaybackState::Playing);
    EXPECT_FALSE(removed.queue.upcomingEntryId);
    const auto end = std::chrono::steady_clock::now() + 5s;
    while (api().snapshot().playback.state == PlaybackState::Playing && std::chrono::steady_clock::now() < end) {
        pull();
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(api().snapshot().playback.state, PlaybackState::Stopped);
    EXPECT_TRUE(heard == concatenatedReferences({a}));
    const auto emptied = std::chrono::steady_clock::now() + 3s;
    while (!api().snapshot().queue.entries.empty() && std::chrono::steady_clock::now() < emptied)
        std::this_thread::sleep_for(10ms);
    EXPECT_TRUE(api().snapshot().queue.entries.empty());
    EXPECT_EQ(store.openReader().scalar("SELECT COUNT(*) FROM files").value(), 0);
    ASSERT_TRUE(scanner.scan(scanner.addRoot(dir / "music")).complete);
    EXPECT_TRUE(api().snapshot().queue.entries.empty());
    server.reset();
}

TEST_P(ServerTest, PlaybackReplacesAnEstimatedMp3LengthWithTheExactOne) {
    const auto dir = testutil::tempDir() / "server-estimated-mp3";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "music");
    const auto path = dir / "music" / "plain.mp3";
    testutil::writeMp3(path, testaudio::makeSignal({"short", 44100, 16, 2, 2.0}), 44100, 16, 2, false);
    const auto exact = [&] {
        auto decoder = openDecoder(DecoderKind::Mpg123, path);
        std::vector<int32_t> samples(size_t(4096) * 2);
        while (decoder->read(samples.data(), 4096)) {
        }
        return decoder->info().totalFrames;
    }();
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    ASSERT_TRUE(scanner.scan(scanner.addRoot(dir / "music")).complete);
    store.write([](sqlite::Database& db) { db.exec("UPDATE tracks SET total_frames=1 WHERE length_estimated=1"); }).get();
    server = std::make_unique<LocalServer>(withoutAutoplay(), dir / "db" / "settings.json",
                                           [&](const Settings&) { return std::make_unique<ManualOutput>(probe); });
    ASSERT_EQ(send(queueReplace({TrackSource(toUtf8(path.wstring()))})).playback.state, PlaybackState::Playing);
    const auto end = std::chrono::steady_clock::now() + 5s;
    const auto corrected = [&] {
        return store.openReader().scalar("SELECT COUNT(*) FROM tracks WHERE length_estimated=0 AND total_frames=" +
                                         std::to_string(exact)) == 1;
    };
    // mpg123 learns the length while the file plays, so the device keeps taking audio.
    while (!corrected() && std::chrono::steady_clock::now() < end) {
        {
            std::lock_guard lock(probe.renderMutex);
            if (probe.source) {
                std::vector<uint8_t> bytes(size_t(480) * probe.format.bytesPerFrame());
                probe.source->render(bytes.data(), 480);
            }
        }
        std::this_thread::sleep_for(20ms);
    }
    EXPECT_TRUE(corrected());
    send(command(CommandKind::Stop));
    server.reset();
}

TEST_P(ServerTest, PlayingALibraryTrackRecordsOneCountedSessionThatResetClears) {
    const auto dir = testutil::tempDir() / "server-play-statistics";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "music");
    const auto path = dir / "music" / "a.flac";
    std::filesystem::copy_file(testutil::generatedFile({"play-statistics", 48000, 16, 2, 0.5}), path);
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    ASSERT_TRUE(scanner.scan(scanner.addRoot(dir / "music")).complete);
    const auto file = store.openReader().scalar("SELECT id FROM files WHERE kind='audio'").value();
    server = std::make_unique<LocalServer>(withoutAutoplay(), dir / "db" / "settings.json",
                                           [&](const Settings&) { return std::make_unique<ManualOutput>(probe); });
    ASSERT_EQ(send(queueReplace({TrackSource(toUtf8(path.wstring()))})).playback.state, PlaybackState::Playing);
    const auto end = std::chrono::steady_clock::now() + 10s;
    while (api().snapshot().playback.state == PlaybackState::Playing && std::chrono::steady_clock::now() < end) {
        {
            std::lock_guard lock(probe.renderMutex);
            if (probe.source) {
                std::vector<uint8_t> bytes(size_t(480) * probe.format.bytesPerFrame());
                probe.source->render(bytes.data(), 480);
            }
        }
        std::this_thread::sleep_for(10ms); // about real time, so the Server observes continuous playback
    }
    ASSERT_EQ(api().snapshot().playback.state, PlaybackState::Stopped);
    api().waitIdle();
    const auto statistics = api().playStatistics({});
    ASSERT_EQ(statistics.history.size(), 1u);
    EXPECT_EQ(statistics.history[0].track, (LibraryTrackKey{file}));
    EXPECT_TRUE(statistics.history[0].counted);
    EXPECT_NEAR(double(statistics.history[0].heardMs), 500.0, 50.0);
    EXPECT_EQ(statistics.overview.plays, 1);
    EXPECT_EQ(store.openReader().scalar("SELECT play_count FROM stats WHERE file_id=" + std::to_string(file)), 1);

    Command reset = command(CommandKind::ResetPlayStatistics);
    EXPECT_THROW(api().submit(reset), std::invalid_argument);
    reset.tracks = {{file}};
    ASSERT_FALSE(send(reset).commandError);
    EXPECT_EQ(api().playStatistics({}).historyTotal, 0);
    EXPECT_EQ(store.openReader().scalar("SELECT play_count+listened_ms FROM stats WHERE file_id=" + std::to_string(file)), 0);
    Command all = command(CommandKind::ResetAllPlayStatistics);
    all.tracks = {{file}};
    EXPECT_THROW(api().submit(all), std::invalid_argument);
    all.tracks.clear();
    EXPECT_FALSE(send(all).commandError);

    Command favorite = command(CommandKind::SetFavorite);
    EXPECT_THROW(api().submit(favorite), std::invalid_argument);
    favorite.tracks = {{file}};
    favorite.favorite = true;
    const auto marks = api().snapshot().queue.marks;
    const auto marked = send(favorite);
    EXPECT_FALSE(marked.commandError);
    ASSERT_EQ(marked.queue.entries.size(), 1u);
    EXPECT_TRUE(marked.queue.entries.get()[0].favorite);
    EXPECT_EQ(marked.queue.entries.get()[0].file, file);
    EXPECT_GT(marked.queue.marks, marks);
    Command album = command(CommandKind::SetAlbumFavorite);
    album.favorite = true;
    EXPECT_THROW(api().submit(album), std::invalid_argument);
    album.albums = {store.openReader().scalar("SELECT album_id FROM tracks").value()};
    EXPECT_FALSE(send(album).commandError);
    EXPECT_EQ(store.openReader().scalar("SELECT album_favorite FROM stats WHERE file_id=" + std::to_string(file)), 1);
    Command artist = command(CommandKind::SetArtistFavorite);
    artist.favorite = true;
    EXPECT_THROW(api().submit(artist), std::invalid_argument);
    artist.artists = {{LibraryGroupField::Genre, "Rock"}};
    EXPECT_THROW(api().submit(artist), std::invalid_argument);
    artist.artists = {{LibraryGroupField::Composer, "Someone"}};
    EXPECT_FALSE(send(artist).commandError);
    EXPECT_EQ(store.openReader().scalar("SELECT COUNT(*) FROM favorite_names WHERE field='composer' AND name='Someone'"), 1);
    server.reset();
}

INSTANTIATE_TEST_SUITE_P(Through, ServerTest, ::testing::Values(false, true), throughName);

TEST(ServerGaplessTest, ReorderReplacesTheQueuedFileNotTheOneAlreadyDecoding) {
    const std::vector<std::filesystem::path> files = {testutil::testFile("24-96k"),
                                                      testutil::generatedFile({"gapless-b-2s-24-96k", 96000, 24, 2, 2.0}),
                                                      testutil::generatedFile({"gapless-c-24-96k", 96000, 24, 2, 0.4}),
                                                      testutil::generatedFile({"gapless-d-24-96k", 96000, 24, 2, 0.5})};
    const auto capture = testutil::tempDir() / "server-gapless-reorder.raw";
    CaptureOptions options;
    options.file = capture;
    options.accepts = [](const DeviceFormat& f) { return f.type == SampleType::Int32; };
    options.realtime = true;
    std::atomic<int> opens = 0;
    {
        // Decoding a second ahead of the device, it is in b while c waits in the slot, from a's last second to b's.
        Settings settings = withoutAutoplay();
        settings.wasapi.timing.predecodeSeconds = 1;
        LocalServer server(
            settings, {}, [&](const Settings&) { return std::make_unique<CountingOutput>(createCaptureOutput(options), opens); });
        std::vector<TrackSource> sources;
        sources.reserve(files.size());
        for (const auto& file : files) sources.emplace_back(toUtf8(file.wstring()));
        server.submit(queueReplace(sources));
        server.waitIdle();
        const auto ids = entryIds(server.snapshot());
        std::this_thread::sleep_for(1500ms); // b is already decoding behind a; c waits in the slot
        Command reorder = command(CommandKind::QueueReorder);
        reorder.entryIds = {ids[0], ids[1], ids[3], ids[2]};
        server.submit(reorder);
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        Snapshot s = server.snapshot();
        while ((s.queueEntryId != ids[2] || s.playback.state != PlaybackState::Stopped) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(10ms);
            s = server.snapshot();
        }
        EXPECT_EQ(s.queueEntryId, ids[2]);
        EXPECT_FALSE(s.playback.error) << s.playback.errorDetail;
    }
    EXPECT_EQ(opens.load(), 1);
    EXPECT_TRUE(capturedSamples(capture) == concatenatedReferences({files[0], files[1], files[3], files[2]}));
    std::filesystem::remove(capture);
}

TEST(ServerGaplessTest, ARemovedEntryAlreadyDecodingIsNotHeardAndTheNextPlays) {
    const std::vector<std::filesystem::path> files = {testutil::testFile("24-96k"),
                                                      testutil::generatedFile({"gapless-b-24-96k", 96000, 24, 2, 0.3}),
                                                      testutil::generatedFile({"gapless-c-24-96k", 96000, 24, 2, 0.4})};
    const auto capture = testutil::tempDir() / "server-gapless-remove.raw";
    CaptureOptions options;
    options.file = capture;
    options.accepts = [](const DeviceFormat& f) { return f.type == SampleType::Int32; };
    options.realtime = true;
    std::atomic<int> opens = 0;
    {
        LocalServer server(withoutAutoplay(), {}, [&](const Settings&) {
            return std::make_unique<CountingOutput>(createCaptureOutput(options), opens);
        });
        std::vector<TrackSource> sources;
        sources.reserve(files.size());
        for (const auto& file : files) sources.emplace_back(toUtf8(file.wstring()));
        server.submit(queueReplace(sources));
        server.waitIdle();
        const auto ids = entryIds(server.snapshot());
        std::this_thread::sleep_for(500ms); // b is already decoding behind a
        Command remove = command(CommandKind::QueueRemove);
        remove.entryIds = {ids[1]};
        server.submit(remove);
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        Snapshot s = server.snapshot();
        while ((s.queueEntryId != ids[2] || s.playback.state != PlaybackState::Stopped) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(10ms);
            s = server.snapshot();
        }
        EXPECT_EQ(s.queueEntryId, ids[2]);
        EXPECT_FALSE(s.playback.error) << s.playback.errorDetail;
        EXPECT_FALSE(s.commandError);
    }
    // Dropping a file the Engine already decodes restarts the stream where a is, and the capture with it: what it holds
    // is the rest of a, then c.
    const auto got = capturedSamples(capture);
    const auto a = concatenatedReferences({files[0]});
    const auto c = concatenatedReferences({files[2]});
    ASSERT_GE(got.size(), c.size());
    const auto restOfA = std::ptrdiff_t(got.size() - c.size());
    EXPECT_TRUE(std::equal(c.begin(), c.end(), got.begin() + restOfA));
    ASSERT_LE(restOfA, std::ptrdiff_t(a.size()));
    EXPECT_TRUE(std::equal(got.begin(), got.begin() + restOfA, a.end() - restOfA));
    std::filesystem::remove(capture);
}

TEST(ServerGaplessTest, ClearingTheQueueDropsTheEntryAlreadyDecoding) {
    const std::vector<std::filesystem::path> files = {testutil::testFile("24-96k"),
                                                      testutil::generatedFile({"gapless-b-24-96k", 96000, 24, 2, 0.3})};
    const auto capture = testutil::tempDir() / "server-gapless-clear.raw";
    CaptureOptions options;
    options.file = capture;
    options.accepts = [](const DeviceFormat& f) { return f.type == SampleType::Int32; };
    options.realtime = true;
    std::atomic<int> opens = 0;
    {
        LocalServer server(withoutAutoplay(), {}, [&](const Settings&) {
            return std::make_unique<CountingOutput>(createCaptureOutput(options), opens);
        });
        server.submit(queueReplace({TrackSource(toUtf8(files[0].wstring())), TrackSource(toUtf8(files[1].wstring()))}));
        server.waitIdle();
        const auto ids = entryIds(server.snapshot());
        std::this_thread::sleep_for(500ms); // b is already decoding behind a
        server.submit(command(CommandKind::QueueClear));
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        Snapshot s = server.snapshot();
        while (s.playback.state != PlaybackState::Stopped && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(10ms);
            s = server.snapshot();
        }
        EXPECT_EQ(s.queueEntryId, ids[0]);
        EXPECT_FALSE(s.playback.error) << s.playback.errorDetail;
        EXPECT_FALSE(s.commandError);
    }
    // The capture starts again where a was when the stream restarted, and holds the rest of a alone.
    const auto got = capturedSamples(capture);
    const auto a = concatenatedReferences({files[0]});
    ASSERT_LE(got.size(), a.size());
    EXPECT_TRUE(std::equal(got.begin(), got.end(), a.end() - std::ptrdiff_t(got.size())));
    std::filesystem::remove(capture);
}

TEST(ServerGaplessTest, SeekPlaysOnTheOpenDeviceAndQueuesTheNextFileAgain) {
    const std::vector<std::filesystem::path> files = {testutil::testFile("24-96k"),
                                                      testutil::generatedFile({"gapless-b-24-96k", 96000, 24, 2, 0.3})};
    const auto capture = testutil::tempDir() / "server-gapless-seek.raw";
    CaptureOptions options;
    options.file = capture;
    options.accepts = [](const DeviceFormat& f) { return f.type == SampleType::Int32; };
    options.realtime = true;
    std::atomic<int> opens = 0;
    {
        LocalServer server(withoutAutoplay(), {}, [&](const Settings&) {
            return std::make_unique<CountingOutput>(createCaptureOutput(options), opens);
        });
        server.submit(queueReplace({TrackSource(toUtf8(files[0].wstring())), TrackSource(toUtf8(files[1].wstring()))}));
        server.waitIdle();
        const auto ids = entryIds(server.snapshot());
        Command seek = command(CommandKind::Seek);
        seek.frame = 96000 * 3 / 2;
        server.submit(seek);
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        Snapshot s = server.snapshot();
        while ((s.queueEntryId != ids[1] || s.playback.state != PlaybackState::Stopped) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(10ms);
            s = server.snapshot();
        }
        EXPECT_EQ(s.queueEntryId, ids[1]);
        EXPECT_FALSE(s.playback.error) << s.playback.errorDetail;
    }
    EXPECT_EQ(opens.load(), 1); // the seek plays on the open device; the next file then follows without another open
    const auto first = testaudio::referenceDecode(files[0]);
    auto expected = std::vector<int32_t>(first.samples.begin() + ptrdiff_t(96000) * 3, first.samples.end());
    const auto second = concatenatedReferences({files[1]});
    expected.insert(expected.end(), second.begin(), second.end());
    // a from its start up to the seek, the silence the device plays while the seek starts, then a from 1.5 s and b.
    const auto got = capturedSamples(capture);
    ASSERT_GE(got.size(), expected.size());
    const auto seekAt = got.end() - ptrdiff_t(expected.size());
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(), seekAt));
    const auto heardEnd = std::find_if(std::make_reverse_iterator(seekAt), got.rend(), [](int32_t v) { return v != 0; }).base();
    ASSERT_LE(heardEnd - got.begin(), ptrdiff_t(first.samples.size()));
    EXPECT_TRUE(std::equal(got.begin(), heardEnd, first.samples.begin()));
    std::filesystem::remove(capture);
}

TEST(ServerGaplessTest, FormatChangeBetweenTracksReopensTheDevice) {
    const auto capture = testutil::tempDir() / "server-format-change.raw";
    std::atomic<int> opens = 0;
    const auto s = playQueueToEnd({source("24-96k"), source("16-44.1k")}, capture, opens);
    EXPECT_EQ(s.playback.state, PlaybackState::Stopped);
    EXPECT_FALSE(s.playback.error) << s.playback.errorDetail;
    EXPECT_EQ(s.queueEntryId, s.queue.entries.back().id);
    EXPECT_EQ(opens.load(), 2);
    std::filesystem::remove(capture);
}

class ServerCaptureTest : public ::testing::TestWithParam<DecoderKind> {};

TEST_P(ServerCaptureTest, MatchesReferencePcmFor16And24BitFiles) {
    for (const char* name : {"16-44.1k", "24-96k"}) {
        const auto file = testutil::testFile(name);
        const auto reference = testaudio::referenceDecode(file);
        ASSERT_TRUE(reference.error.empty()) << reference.error;
        const auto output = testutil::tempDir() / "server-capture.raw";
        CaptureOptions options;
        options.file = output;
        options.accepts = [](const DeviceFormat& f) { return f.type == SampleType::Int32; };
        {
            Settings settings;
            settings.decoderByFormat[AudioFormat::Flac] = GetParam();
            LocalServer server(settings, {}, [&](const Settings&) { return createCaptureOutput(options); });
            server.submit(select(name));
            server.waitIdle();
            const auto deadline = std::chrono::steady_clock::now() + 10s;
            while (server.snapshot().playback.state != PlaybackState::Stopped && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(5ms);
            const auto s = server.snapshot();
            ASSERT_EQ(s.playback.state, PlaybackState::Stopped);
            ASSERT_FALSE(s.playback.error) << s.playback.errorDetail;
        }
        std::ifstream in(output, std::ios::binary | std::ios::ate);
        ASSERT_TRUE(in);
        ASSERT_EQ(in.tellg(), std::streamoff(reference.samples.size() * 4));
        in.seekg(0);
        std::vector<uint8_t> bytes(reference.samples.size() * 4);
        ASSERT_TRUE(in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size())));
        std::vector<int32_t> samples(reference.samples.size());
        unpackSamples(bytes.data(), samples.size(), reference.info.bitsPerSample, SampleType::Int32, samples.data());
        EXPECT_EQ(samples, reference.samples);
        in.close();
        std::filesystem::remove(output);
    }
}

INSTANTIATE_TEST_SUITE_P(AllDecoders, ServerCaptureTest, ::testing::ValuesIn(decodersFor(AudioFormat::Flac)));

} // namespace
