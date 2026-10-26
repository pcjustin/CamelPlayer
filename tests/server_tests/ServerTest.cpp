// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <fstream>
#include <mutex>
#include <thread>

#include "Server/Playback/LocalServer.h"
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

class ServerTest : public ::testing::Test {
protected:
    Probe probe;
    std::unique_ptr<LocalServer> server;
    void SetUp() override {
        server = std::make_unique<LocalServer>(Settings{}, std::filesystem::path{}, [&](const Settings& settings) {
            if (probe.failBackend == settings.backend) throw std::runtime_error("injected output activation failure");
            return std::make_unique<ManualOutput>(probe);
        });
    }
    Snapshot send(Command c) {
        server->submit(std::move(c));
        server->waitIdle();
        return server->snapshot();
    }
};

TEST_F(ServerTest, LibraryFingerprintingFollowsActualPlaybackState) {
    const auto dir = testutil::tempDir() / "server-library-playback";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "music");
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    scanner.addRoot(dir / "music");
    server = std::make_unique<LocalServer>(Settings{}, dir / "db" / "settings.json", [&](const Settings&) {
        return std::make_unique<ManualOutput>(probe);
    });
    const auto waitFor = [](const std::function<bool()>& check) {
        const auto end = std::chrono::steady_clock::now() + 8s;
        while (std::chrono::steady_clock::now() < end) {
            if (check()) return true;
            std::this_thread::sleep_for(20ms);
        }
        return false;
    };
    ASSERT_TRUE(waitFor([&] { return store.openReader().scalar("SELECT COUNT(*) FROM roots WHERE last_scan IS NOT NULL").value() == 1; }));
    for (const auto release : {CommandKind::Pause, CommandKind::Stop}) {
        send(select());
        const auto file = dir / "music" / (release == CommandKind::Pause ? "pause.lrc" : "stop.lrc");
        std::ofstream(file) << (release == CommandKind::Pause ? "lyrics" : "different lyrics");
        ASSERT_TRUE(waitFor([&] { return store.openReader().scalar("SELECT COUNT(*) FROM files WHERE content_hash IS NULL").value() == 1; }));
        std::this_thread::sleep_for(1200ms);
        EXPECT_EQ(store.openReader().scalar("SELECT COUNT(*) FROM files WHERE content_hash IS NULL").value(), 1);
        send(command(release));
        ASSERT_TRUE(waitFor([&] { return store.openReader().scalar("SELECT COUNT(*) FROM files WHERE content_hash IS NULL").value() == 0; }));
    }
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
    s.apiVersion = 2;
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

TEST_F(ServerTest, RejectsInvalidCommandsBeforeAllocatingIds) {
    Command c;
    c.apiVersion = 2;
    EXPECT_THROW(server->submit(c), std::invalid_argument);
    c = command(CommandKind::Load);
    EXPECT_THROW(server->submit(c), std::invalid_argument);
    c = select();
    c.kind = CommandKind::Play;
    EXPECT_THROW(server->submit(c), std::invalid_argument);
    c = command(CommandKind(255));
    EXPECT_THROW(server->submit(c), std::invalid_argument);
    c = command(CommandKind::Stop);
    c.frame = 10;
    EXPECT_THROW(server->submit(c), std::invalid_argument);
    c = select();
    c.source.emplace("a", FrameRange{1, 20});
    EXPECT_THROW(server->submit(c), std::invalid_argument);
    c = command(CommandKind::ApplySettings);
    c.settings = SettingsDto{};
    c.settings->ks.packetCount = 0;
    EXPECT_THROW(server->submit(c), std::invalid_argument);
    c.settings->ks.packetCount = 4;
    c.settings->backend = Backend(255);
    EXPECT_THROW(server->submit(c), std::invalid_argument);
    EXPECT_EQ(server->submit(Command{}).id, 1u);
    server->waitIdle();
}

TEST_F(ServerTest, AssignsOrderedScopesAndDistinctQueueEntries) {
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

TEST_F(ServerTest, SerializesPlaybackControlsAndSeek) {
    send(select());
    server->submit(command(CommandKind::Pause));
    Command seek = command(CommandKind::Seek);
    seek.frame = 12000;
    server->submit(seek);
    server->waitIdle();
    auto s = server->snapshot();
    EXPECT_EQ(s.playback.state, PlaybackState::Paused);
    EXPECT_EQ(s.playback.position, 12000u);
    EXPECT_FALSE(s.playback.error);
    EXPECT_EQ(send(command(CommandKind::TogglePause)).playback.state, PlaybackState::Playing);
    EXPECT_EQ(send(Command{}).playback.state, PlaybackState::Stopped);
    EXPECT_FALSE(send(command(CommandKind::ShowControlPanel)).controlPanelOpened.value_or(true));
}

TEST_F(ServerTest, ConcurrentProducersReceiveUniqueOrderedIds) {
    std::mutex mutex;
    std::vector<uint64_t> ids;
    auto producer = [&] {
        for (int i = 0; i < 25; ++i) {
            auto id = server->submit(command(CommandKind::Pause)).id;
            std::lock_guard lock(mutex);
            ids.push_back(id);
        }
    };
    std::thread a(producer), b(producer), c(producer), d(producer);
    a.join();
    b.join();
    c.join();
    d.join();
    server->waitIdle();
    std::sort(ids.begin(), ids.end());
    ASSERT_EQ(ids.size(), 100u);
    for (size_t i = 0; i < ids.size(); ++i) EXPECT_EQ(ids[i], i + 1);
    EXPECT_EQ(server->snapshot().command.id, 100u);
}

TEST_F(ServerTest, SuppressesBlockedOldGenerationAfterNewSelection) {
    std::promise<void> opened;
    std::atomic<bool> released = false;
    probe.onOpen = [&] {
        opened.set_value();
        released.wait(false);
    };
    std::mutex mutex;
    std::vector<Snapshot> events;
    struct DrainServer {
        std::unique_ptr<LocalServer>& server;
        ~DrainServer() { server.reset(); }
    } drain{server};
    server->setListener([&](const Event& e) {
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
        server->submit(select());
        ASSERT_EQ(opened.get_future().wait_for(10s), std::future_status::ready);
        const auto latest = server->submit(select("16-44.1k", CommandKind::Load));
        EXPECT_EQ(latest.generation, 2u);
    }
    server->waitIdle();
    server->setListener({});
    ASSERT_FALSE(events.empty());
    for (const auto& e : events) EXPECT_EQ(e.command.generation, 2u);
    EXPECT_EQ(server->snapshot().playback.pathUtf8, toUtf8(testutil::testFile("16-44.1k").wstring()));
}

TEST_F(ServerTest, ListenerMayReadSnapshotAndSubmitButExceptionsAreIsolated) {
    std::atomic<int> callbacks = 0;
    server->setListener([&](const Event& e) {
        EXPECT_GE(server->snapshot().revision, e.snapshot.revision);
        if (++callbacks == 1) server->submit(command(CommandKind::Pause));
        throw std::runtime_error("injected listener failure");
    });
    server->submit(select());
    server->waitIdle();
    server->waitIdle();
    EXPECT_EQ(server->snapshot().playback.state, PlaybackState::Paused);
    server->setListener({});
    EXPECT_GE(callbacks.load(), 2);
    EXPECT_EQ(send(Command{}).playback.state, PlaybackState::Stopped);
}

TEST_F(ServerTest, FileErrorsRemainVisibleAndNextSelectionRecovers) {
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

TEST_F(ServerTest, TrackDecoderOverrideSurvivesSettingsAndDoesNotChangeGlobalPreference) {
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

TEST_F(ServerTest, UnrelatedSettingsDoNotRestartPlaybackAndRequestedStateIsSeparate) {
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

TEST_F(ServerTest, RequestedBackendRemainsVisibleWhenActivationFails) {
    auto s = send(select());
    ASSERT_EQ(s.playback.activeBackend, Backend::Wasapi);
    probe.failBackend = Backend::Ks;

    auto requested = s.settings;
    requested.backend = Backend::Ks;
    Command c = command(CommandKind::ApplySettings);
    c.settings = requested;
    s = send(c);

    EXPECT_EQ(s.settings.backend, Backend::Ks);
    EXPECT_EQ(s.playback.backend, Backend::Ks);
    EXPECT_FALSE(s.playback.activeBackend);
    EXPECT_FALSE(s.playback.activeDecoder);
    EXPECT_FALSE(s.playback.output);
    EXPECT_EQ(s.playback.state, PlaybackState::Stopped);
    ASSERT_TRUE(s.playback.error);
}

TEST_F(ServerTest, DestructorClosesActiveOutputAndDrainsCommands) {
    send(select());
    server->submit(command(CommandKind::Pause));
    server.reset();
    EXPECT_GT(probe.closes.load(), 0);
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

TEST_F(ServerTest, RejectsInvalidQueuePayloads) {
    EXPECT_THROW(server->submit(queueReplace({})), std::invalid_argument);
    EXPECT_THROW(server->submit(queueReplace({source("24-96k")}, 1)), std::invalid_argument);
    EXPECT_THROW(server->submit(command(CommandKind::QueueReorder)), std::invalid_argument);
    EXPECT_THROW(server->submit(command(CommandKind::QueueSelect)), std::invalid_argument);
    EXPECT_THROW(server->submit(command(CommandKind::SetShuffle)), std::invalid_argument);
    Command c = command(CommandKind::SetRepeat);
    c.repeat = RepeatMode(7);
    EXPECT_THROW(server->submit(c), std::invalid_argument);
    c = command(CommandKind::Next);
    c.entryId = 1;
    EXPECT_THROW(server->submit(c), std::invalid_argument);
    c = queueReplace({TrackSource("a.flac", FrameRange{1, 2})});
    EXPECT_THROW(server->submit(c), std::invalid_argument);
    c = command(CommandKind::RemoveLibraryRoot);
    EXPECT_THROW(server->submit(c), std::invalid_argument);
    c.rootId = -1;
    EXPECT_THROW(server->submit(c), std::invalid_argument);
    c.kind = CommandKind::Stop;
    c.rootId = 1;
    EXPECT_THROW(server->submit(c), std::invalid_argument);
    EXPECT_EQ(server->snapshot().command.id, 0u);
}

TEST_F(ServerTest, PlayNextListsTracksRightAfterTheCurrentOne) {
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

TEST_F(ServerTest, QueueNavigationReorderAndUpcomingEntry) {
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

TEST_F(ServerTest, ShuffleAndRepeatAreReportedInQueueSnapshot) {
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

TEST_F(ServerTest, RepeatAllStopsWhenEveryEntryFails) {
    Command repeat = command(CommandKind::SetRepeat);
    repeat.repeat = RepeatMode::All;
    send(repeat);
    send(queueReplace({missingSource("queue-missing-1.flac"), missingSource("queue-missing-2.flac")}));
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    Snapshot s = server->snapshot();
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(100ms);
        const auto next = server->snapshot();
        if (next.playback.revision == s.playback.revision && next.queueEntryId == s.queueEntryId) break;
        s = next;
    }
    std::this_thread::sleep_for(300ms);
    const auto settled = server->snapshot();
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
        LocalServer server(Settings{}, {}, [&](const Settings&) { return createCaptureOutput(options); });
        Command c = queueReplace({source("16-44.1k"), missingSource("queue-missing.flac"), source("24-96k")});
        server.submit(c);
        server.waitIdle();
        const auto ids = entryIds(server.snapshot());
        ASSERT_EQ(ids.size(), 3u);
        const auto deadline = std::chrono::steady_clock::now() + 20s;
        Snapshot s = server.snapshot();
        while (!(s.queueEntryId == ids[2] && s.playback.state == PlaybackState::Stopped) &&
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
    LocalServer server(Settings{}, {},
                       [&](const Settings&) { return std::make_unique<CountingOutput>(createCaptureOutput(options), opens); });
    server.submit(queueReplace(std::move(sources)));
    server.waitIdle();
    const auto ids = entryIds(server.snapshot());
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    Snapshot s = server.snapshot();
    while (!(s.queueEntryId == ids.back() && s.playback.state == PlaybackState::Stopped) &&
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
class LibraryPlaybackTest : public ::testing::Test {
protected:
    std::filesystem::path dir;
    std::unique_ptr<LibraryStore> store;
    std::unique_ptr<LibraryScanner> scanner;
    std::unique_ptr<LocalServer> server;
    int64_t root = 0;
    CaptureOptions options;

    std::filesystem::path setUp(const std::vector<std::filesystem::path>& files) {
        dir = testutil::tempDir() / "server-play" /
              (std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()) + "-" +
               std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        std::filesystem::create_directories(dir / "music");
        for (const auto& file : files) std::filesystem::copy_file(file, dir / "music" / file.filename());
        store = std::make_unique<LibraryStore>(dir / "db");
        scanner = std::make_unique<LibraryScanner>(*store);
        root = scanner->addRoot(dir / "music");
        EXPECT_TRUE(scanner->scan(root).complete);
        options.file = dir / "capture.raw";
        options.accepts = [](const DeviceFormat& f) { return f.type == SampleType::Int32; };
        options.realtime = true;
        server = std::make_unique<LocalServer>(Settings{}, dir / "db" / "settings.json",
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
        server->submit(std::move(c));
        server->waitIdle();
        return server->snapshot();
    }
    bool waitFor(const std::function<bool()>& check, std::chrono::milliseconds timeout = 15s) {
        const auto end = std::chrono::steady_clock::now() + timeout;
        while (!check()) {
            if (std::chrono::steady_clock::now() >= end) return false;
            std::this_thread::sleep_for(10ms);
        }
        return true;
    }
    bool stopped() { return server->snapshot().playback.state == PlaybackState::Stopped; }
    // Plays are persisted through the Server's own writer, so tests wait for them rather than for a barrier.
    PlayStatistics plays(int64_t expected) {
        PlayStatistics result;
        EXPECT_TRUE(waitFor([&] { return (result = server->playStatistics({})).historyTotal >= expected; }));
        return result;
    }
    int64_t file(const std::filesystem::path& path) {
        auto row = store->openReader().prepare("SELECT id FROM files WHERE path LIKE ? AND available=1");
        return row.bind(1, "%" + toUtf8(path.filename().wstring())).step() ? row.integer(0) : 0;
    }
    TrackSource track(const std::filesystem::path& path) { return TrackSource(toUtf8((dir / "music" / path.filename()).wstring())); }
};

TEST_F(LibraryPlaybackTest, AnAddedRootIsIndexedPromptlyAndBrowsable) {
    setUp({});
    const auto second = dir / "second";
    std::filesystem::create_directories(second / "album");
    std::filesystem::copy_file(testutil::generatedFile({"added-root", 48000, 16, 2, 0.2}), second / "album" / "a.flac");
    Command add = command(CommandKind::AddLibraryRoot);
    EXPECT_THROW(server->submit(add), std::invalid_argument);
    add.path = toUtf8(second.wstring());
    ASSERT_FALSE(send(add).commandError);
    EXPECT_EQ(server->libraryRoots().size(), 2u);
    // The monitor rereads roots on its next pass rather than after its 60-second availability interval.
    // Tracks and their albums appear with each written batch.
    ASSERT_TRUE(waitFor([&] { return server->browseAlbums(AlbumSort::Title, std::nullopt, 0, 10, std::nullopt).total == 1; }, 20s));
    EXPECT_EQ(server->browseTrackRows(TrackSort::Title, false, 0, 10, std::nullopt).total, 1);
    const auto albums = server->browseAlbums(AlbumSort::Title, std::nullopt, 0, 10, std::nullopt);
    EXPECT_EQ(server->albumDetail(albums.albums[0].id).tracks.size(), 1u);
    const auto roots = server->browseFolder(std::nullopt);
    EXPECT_EQ(roots.folders.size(), 2u);
    Command missing = command(CommandKind::AddLibraryRoot);
    missing.path = toUtf8((dir / "does-not-exist").wstring());
    EXPECT_TRUE(send(missing).commandError);
}

TEST_F(LibraryPlaybackTest, ALibraryTrackPlaysUnderItsOnDiskSpelling) {
    const auto music = setUp({});
    std::filesystem::copy_file(testutil::generatedFile({"spelling", 48000, 16, 2, 0.3}), music / "Mixed Case.flac");
    ASSERT_TRUE(scanner->scan(root).complete);
    const auto rows = server->browseTrackRows(TrackSort::Title, false, 0, 10, std::nullopt);
    ASSERT_EQ(rows.tracks.size(), 1u);
    ASSERT_NE(rows.tracks[0].path.find("mixed case.flac"), std::string::npos); // the lowercase index key
    send(queueReplace({TrackSource(rows.tracks[0].path)}));
    const auto path = server->snapshot().playback.pathUtf8;
    EXPECT_EQ(std::filesystem::path(toWide(path)).filename(), L"Mixed Case.flac");
    EXPECT_FALSE(path.starts_with(R"(\\?\)"));
    EXPECT_EQ(server->snapshot().queue.entries[0].source.pathUtf8(), path);
}

TEST_F(LibraryPlaybackTest, SavingTheQueueKeepsDuplicatesAndEntriesOutsideTheLibrary) {
    const auto a = testutil::generatedFile({"queue-save-a", 48000, 16, 2, 0.3});
    setUp({a});
    const auto outside = testutil::testFile("16-44.1k");
    send(queueReplace({track(a), TrackSource(toUtf8(outside.wstring())), track(a)}));
    send(command(CommandKind::Stop));
    const auto saved = server->saveQueueAsPlaylist("Queue");
    EXPECT_EQ(saved.added, 3);
    const auto page = server->playlistItems(saved.playlist, 0, 10);
    ASSERT_EQ(page.items.size(), 3u);
    EXPECT_EQ(page.items[0].track, (LibraryTrackKey{file(a), 0}));
    EXPECT_FALSE(page.items[1].track);
    EXPECT_EQ(page.items[1].source, toUtf8(outside.wstring()));
    EXPECT_EQ(page.items[2].track, page.items[0].track);
    EXPECT_TRUE(server->undoPlaylistChange(saved.undo));
    EXPECT_TRUE(server->playlists().empty());
}

TEST_F(LibraryPlaybackTest, TheQueueRestoresPausedAtItsPositionWithoutOpeningTheFileUntilPlay) {
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
        auto row = store->openReader().prepare("SELECT value FROM app_state WHERE key='session'");
        return row.step() && row.text(0).find("\"position\":48000") != std::string::npos;
    }));
    server.reset();
    // The restore reads only the library database: the current file need not even be reachable.
    std::filesystem::rename(music / b.filename(), music / "away.flac");
    server = std::make_unique<LocalServer>(Settings{}, dir / "db" / "settings.json",
                                           [&](const Settings&) { return createCaptureOutput(options); });
    server->waitIdle();
    auto s = server->snapshot();
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
    std::filesystem::rename(music / "away.flac", music / b.filename());
    seek.frame = 24000;
    EXPECT_EQ(send(seek).playback.position, 24000u);
    send(command(CommandKind::Play));
    ASSERT_TRUE(waitFor([&] { return server->snapshot().playback.state == PlaybackState::Playing; }));
    s = server->snapshot();
    EXPECT_EQ(s.queueEntryId, s.queue.entries[1].id);
    EXPECT_GE(s.playback.position, 24000u);
    EXPECT_LT(s.playback.position, 72000u);
    EXPECT_TRUE(s.playback.output);
    send(command(CommandKind::Stop));
}

TEST_F(LibraryPlaybackTest, AnEntryWhoseFileIsGoneIsKeptAndPlaySkipsIt) {
    const auto a = testutil::generatedFile({"restore-gone-a", 48000, 16, 2, 0.5});
    const auto b = testutil::generatedFile({"restore-gone-b", 48000, 16, 2, 0.5});
    const auto music = setUp({a, b});
    store->write([](sqlite::Database& db) { db.exec("UPDATE tracks SET title='Kept title',artist='Kept artist'"); }).get();
    send(queueReplace({track(a), track(b)}));
    send(command(CommandKind::Stop));
    server.reset();
    std::filesystem::remove(music / a.filename());
    ASSERT_TRUE(scanner->scan(root).complete);
    store->write([](sqlite::Database& db) {
             db.exec("DELETE FROM tracks WHERE file_id IN (SELECT id FROM files WHERE path LIKE '%restore-gone-a%')");
         })
        .get();
    server = std::make_unique<LocalServer>(Settings{}, dir / "db" / "settings.json",
                                           [&](const Settings&) { return createCaptureOutput(options); });
    server->waitIdle();
    auto s = server->snapshot();
    ASSERT_EQ(s.queue.entries.size(), 2u);
    EXPECT_EQ(s.queueEntryId, s.queue.entries[0].id);
    EXPECT_EQ(s.queue.entries[0].title, "Kept title · Kept artist"); // what it showed, although the file is gone
    EXPECT_EQ(s.queue.entries[1].title, "Kept title");
    send(command(CommandKind::Play));
    s = server->snapshot();
    EXPECT_EQ(s.queueEntryId, s.queue.entries[1].id);
    send(command(CommandKind::Stop));
}

TEST_F(LibraryPlaybackTest, ThePositionIsSavedWhilePlayingSoACrashLosesAtMostFiveSeconds) {
    const auto a = testutil::generatedFile({"restore-playing", 48000, 16, 2, 9.0});
    setUp({a});
    send(queueReplace({track(a)}));
    const auto saved = [&] {
        auto row = store->openReader().prepare("SELECT value FROM app_state WHERE key='session'");
        if (!row.step()) return uint64_t(0);
        const auto text = row.text(0);
        const auto at = text.find("\"position\":");
        return at == std::string::npos ? uint64_t(0) : std::stoull(text.substr(at + 11));
    };
    ASSERT_TRUE(waitFor([&] { return saved() >= 48000u * 3; }, 8s));
    EXPECT_EQ(server->snapshot().playback.state, PlaybackState::Playing);
    send(command(CommandKind::Stop));
}

TEST_F(LibraryPlaybackTest, ShutdownSavesTheQueueAndLeavesTheServerReadable) {
    const auto a = testutil::generatedFile({"restore-shutdown", 48000, 16, 2, 2.0});
    setUp({a});
    send(queueReplace({track(a)}));
    send(command(CommandKind::Pause));
    Command seek = command(CommandKind::Seek);
    seek.frame = 12000;
    send(seek);
    server->shutdown();
    server->shutdown();
    EXPECT_THROW(server->submit(command(CommandKind::Play)), std::runtime_error);
    EXPECT_EQ(server->browseTrackRows(TrackSort::Title, false, 0, 10, std::nullopt).total, 1);
    auto row = store->openReader().prepare("SELECT value FROM app_state WHERE key='session'");
    ASSERT_TRUE(row.step());
    EXPECT_NE(row.text(0).find("\"position\":12000"), std::string::npos);
}

TEST_F(LibraryPlaybackTest, AClearedQueueStaysEmptyInsteadOfReopeningTheLastFile) {
    const auto a = testutil::generatedFile({"restore-cleared", 48000, 16, 2, 0.3});
    setUp({a});
    send(queueReplace({track(a)}));
    send(command(CommandKind::Stop));
    Command clear = command(CommandKind::QueueRemove);
    clear.entryIds = entryIds(server->snapshot());
    send(clear);
    server.reset();
    server = std::make_unique<LocalServer>(Settings{}, dir / "db" / "settings.json",
                                           [&](const Settings&) { return createCaptureOutput(options); });
    server->waitIdle();
    EXPECT_TRUE(server->snapshot().queue.entries.empty());
    EXPECT_TRUE(server->snapshot().playback.pathUtf8.empty());
}

TEST_F(LibraryPlaybackTest, GaplessTransitionsRecordEachTrackOnceWithItsFullLength) {
    const auto a = testutil::generatedFile({"play-gapless-a", 48000, 16, 2, 0.6});
    const auto b = testutil::generatedFile({"play-gapless-b", 48000, 16, 2, 0.4});
    setUp({a, b});
    send(queueReplace({track(a), track(b)}));
    ASSERT_TRUE(waitFor([&] { return stopped() && server->snapshot().queueEntryId == server->snapshot().queue.entries.back().id; }));
    const auto statistics = plays(2);
    ASSERT_EQ(statistics.history.size(), 2u);
    EXPECT_EQ(statistics.history[0].track.file, file(b)); // newest first
    EXPECT_EQ(statistics.history[1].track.file, file(a));
    EXPECT_NEAR(double(statistics.history[0].heardMs), 400.0, 60.0);
    EXPECT_NEAR(double(statistics.history[1].heardMs), 600.0, 60.0);
    EXPECT_TRUE(statistics.history[0].counted && statistics.history[1].counted);
    EXPECT_EQ(statistics.overview.plays, 2);
}

TEST_F(LibraryPlaybackTest, EachRepeatOneLoopIsANewPlay) {
    const auto a = testutil::generatedFile({"play-repeat", 48000, 16, 2, 0.4});
    setUp({a});
    Command repeat = command(CommandKind::SetRepeat);
    repeat.repeat = RepeatMode::One;
    send(repeat);
    send(queueReplace({track(a)}));
    ASSERT_TRUE(waitFor([&] { return server->playStatistics({}).overview.plays >= 3; }));
    send(command(CommandKind::Stop));
    const auto statistics = server->playStatistics({});
    EXPECT_EQ(statistics.tracks.size(), 1u);
    EXPECT_GE(statistics.tracks[0].plays, 3);
    EXPECT_GE(statistics.historyTotal, 3);
}

TEST_F(LibraryPlaybackTest, ASeekForwardCreditsOnlyTheAudioActuallyHeard) {
    const auto a = testutil::generatedFile({"play-seek", 48000, 16, 2, 3.0});
    setUp({a});
    send(queueReplace({track(a)}));
    ASSERT_TRUE(waitFor([&] { return server->snapshot().playback.position >= 9600; })); // 0.2 s
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

TEST_F(LibraryPlaybackTest, ADecodeFailureKeepsOnlyTheAudioHeardBeforeIt) {
    // The engine stops as soon as decoding fails, discarding predecoded audio, so little of the file is ever heard.
    const auto truncated = testutil::tempDir() / "play-truncated.flac";
    testaudio::writeTruncated(testutil::generatedFile({"play-truncated-source", 48000, 16, 2, 6.0}), truncated, 0.9);
    setUp({truncated});
    send(queueReplace({track(truncated)}));
    ASSERT_TRUE(waitFor([&] { return stopped(); }));
    EXPECT_EQ(server->snapshot().playback.error, ErrorCode::DecodeFailed);
    waitFor([&] { return server->playStatistics({}).historyTotal > 0; }, 2s); // nothing heard records nothing
    const auto statistics = server->playStatistics({});
    ASSERT_LE(statistics.history.size(), 1u);
    for (const auto& play : statistics.history) {
        EXPECT_LT(play.heardMs, 1000); // the rest of the six seconds is not credited
        EXPECT_FALSE(play.counted);
    }
}

TEST_F(LibraryPlaybackTest, TheCountIsSavedWhilePlayingSoACrashCannotLoseIt) {
    const auto a = testutil::generatedFile({"play-durable", 48000, 16, 2, 6.0});
    setUp({a});
    send(queueReplace({track(a)}));
    const auto sql = "SELECT COUNT(*) FROM play_events WHERE counted=1 AND file_id=" + std::to_string(file(a));
    ASSERT_TRUE(waitFor([&] { return store->openReader().scalar(sql) == 1; }, 10s));
    EXPECT_EQ(server->snapshot().playback.state, PlaybackState::Playing); // persisted at 3 s, before the play ended
    send(command(CommandKind::Stop));
}

TEST_F(LibraryPlaybackTest, PlaysSurviveRootRemovalAndReconnectWithTheRoot) {
    const auto a = testutil::generatedFile({"play-root", 48000, 16, 2, 0.4});
    setUp({a});
    const auto id = file(a);
    send(queueReplace({track(a)}));
    ASSERT_TRUE(waitFor([&] { return stopped(); }));
    ASSERT_EQ(plays(1).history.size(), 1u);
    Command remove = command(CommandKind::RemoveLibraryRoot);
    remove.rootId = root;
    ASSERT_FALSE(send(remove).commandError);
    auto removed = server->playStatistics({});
    ASSERT_EQ(removed.history.size(), 1u);
    EXPECT_FALSE(removed.history[0].available);
    EXPECT_EQ(removed.overview.plays, 1);
    ASSERT_EQ(scanner->addRoot(dir / "music"), root);
    ASSERT_TRUE(scanner->scan(root).complete);
    auto restored = server->playStatistics({});
    ASSERT_EQ(restored.history.size(), 1u);
    EXPECT_EQ(restored.history[0].track.file, id);
    EXPECT_TRUE(restored.history[0].available);
    EXPECT_EQ(restored.tracks[0].plays, 1);
}

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

TEST_F(ServerTest, RemovedRootKeepsQueueAndFinishesOnlyTheCurrentTrack) {
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
    server = std::make_unique<LocalServer>(Settings{}, dir / "db" / "settings.json", [&](const Settings&) {
        return std::make_unique<ManualOutput>(probe);
    });
    auto playing = send(queueReplace({TrackSource(toUtf8(a.wstring())), TrackSource(toUtf8(b.wstring()))}));
    const auto ids = entryIds(playing);
    std::vector<int32_t> heard;
    const auto pull = [&] {
        std::lock_guard lock(probe.renderMutex);
        if (!probe.source) return;
        std::vector<uint8_t> bytes(4096 * probe.format.bytesPerFrame());
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
    EXPECT_EQ(removed.queue.entries, playing.queue.entries);
    EXPECT_FALSE(removed.queue.upcomingEntryId);
    const auto end = std::chrono::steady_clock::now() + 5s;
    while (server->snapshot().playback.state == PlaybackState::Playing && std::chrono::steady_clock::now() < end) {
        pull();
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(server->snapshot().playback.state, PlaybackState::Stopped);
    EXPECT_EQ(server->snapshot().queueEntryId, ids[0]);
    EXPECT_TRUE(heard == concatenatedReferences({a}));
    EXPECT_EQ(store.openReader().scalar("SELECT COUNT(*) FROM files WHERE available=1").value(), 0);
    ASSERT_EQ(scanner.addRoot(dir / "music"), root);
    ASSERT_TRUE(scanner.scan(root).complete);
    const auto restored = std::chrono::steady_clock::now() + 3s;
    while (!server->snapshot().queue.upcomingEntryId && std::chrono::steady_clock::now() < restored)
        std::this_thread::sleep_for(10ms);
    EXPECT_EQ(server->snapshot().queue.upcomingEntryId, ids[1]);
    auto select = command(CommandKind::QueueSelect);
    select.entryId = ids[1];
    EXPECT_EQ(send(select).playback.state, PlaybackState::Playing);
    send(command(CommandKind::Stop));
    ASSERT_FALSE(send(remove).commandError);
    const auto blocked = send(queueReplace({TrackSource(toUtf8(b.wstring()))}));
    EXPECT_TRUE(blocked.commandError);
    EXPECT_EQ(blocked.queue.entries.size(), 1u);
    EXPECT_EQ(blocked.queueEntryId, 0u);
    EXPECT_FALSE(blocked.track);
    EXPECT_EQ(blocked.playback.state, PlaybackState::Stopped);
    EXPECT_TRUE(send(command(CommandKind::Play)).commandError);
    EXPECT_EQ(server->snapshot().playback.state, PlaybackState::Stopped);
    EXPECT_TRUE(send(command(CommandKind::TogglePause)).commandError);
    EXPECT_EQ(server->snapshot().queueEntryId, 0u);
    server.reset();
}

TEST_F(ServerTest, PlaybackReplacesAnEstimatedMp3LengthWithTheExactOne) {
    const auto dir = testutil::tempDir() / "server-estimated-mp3";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "music");
    const auto path = dir / "music" / "plain.mp3";
    testutil::writeMp3(path, testaudio::makeSignal({"short", 44100, 16, 2, 2.0}), 44100, 16, 2, false);
    const auto exact = openDecoder(DecoderKind::Mpg123, path)->info().totalFrames;
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    ASSERT_TRUE(scanner.scan(scanner.addRoot(dir / "music")).complete);
    store.write([](sqlite::Database& db) { db.exec("UPDATE tracks SET total_frames=1 WHERE length_estimated=1"); }).get();
    server = std::make_unique<LocalServer>(Settings{}, dir / "db" / "settings.json", [&](const Settings&) {
        return std::make_unique<ManualOutput>(probe);
    });
    ASSERT_EQ(send(queueReplace({TrackSource(toUtf8(path.wstring()))})).playback.state, PlaybackState::Playing);
    const auto end = std::chrono::steady_clock::now() + 5s;
    const auto corrected = [&] {
        return store.openReader().scalar("SELECT COUNT(*) FROM tracks WHERE length_estimated=0 AND total_frames=" +
                                         std::to_string(exact)) == 1;
    };
    while (!corrected() && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(20ms);
    EXPECT_TRUE(corrected());
    send(command(CommandKind::Stop));
    server.reset();
}

TEST_F(ServerTest, PlayingALibraryTrackRecordsOneCountedSessionThatResetClears) {
    const auto dir = testutil::tempDir() / "server-play-statistics";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "music");
    const auto path = dir / "music" / "a.flac";
    std::filesystem::copy_file(testutil::generatedFile({"play-statistics", 48000, 16, 2, 0.5}), path);
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    ASSERT_TRUE(scanner.scan(scanner.addRoot(dir / "music")).complete);
    const auto file = store.openReader().scalar("SELECT id FROM files WHERE kind='audio'").value();
    server = std::make_unique<LocalServer>(Settings{}, dir / "db" / "settings.json", [&](const Settings&) {
        return std::make_unique<ManualOutput>(probe);
    });
    ASSERT_EQ(send(queueReplace({TrackSource(toUtf8(path.wstring()))})).playback.state, PlaybackState::Playing);
    const auto end = std::chrono::steady_clock::now() + 10s;
    while (server->snapshot().playback.state == PlaybackState::Playing && std::chrono::steady_clock::now() < end) {
        {
            std::lock_guard lock(probe.renderMutex);
            if (probe.source) {
                std::vector<uint8_t> bytes(480 * probe.format.bytesPerFrame());
                probe.source->render(bytes.data(), 480);
            }
        }
        std::this_thread::sleep_for(10ms); // about real time, so the Server observes continuous playback
    }
    ASSERT_EQ(server->snapshot().playback.state, PlaybackState::Stopped);
    server->waitIdle();
    const auto statistics = server->playStatistics({});
    ASSERT_EQ(statistics.history.size(), 1u);
    EXPECT_EQ(statistics.history[0].track, (LibraryTrackKey{file, 0}));
    EXPECT_TRUE(statistics.history[0].counted);
    EXPECT_NEAR(double(statistics.history[0].heardMs), 500.0, 50.0);
    EXPECT_EQ(statistics.overview.plays, 1);
    EXPECT_EQ(store.openReader().scalar("SELECT play_count FROM stats WHERE file_id=" + std::to_string(file)), 1);

    Command reset = command(CommandKind::ResetPlayStatistics);
    EXPECT_THROW(server->submit(reset), std::invalid_argument);
    reset.tracks = {{file, 0}};
    ASSERT_FALSE(send(reset).commandError);
    EXPECT_EQ(server->playStatistics({}).historyTotal, 0);
    EXPECT_EQ(store.openReader().scalar("SELECT play_count+listened_ms FROM stats WHERE file_id=" + std::to_string(file)), 0);
    Command all = command(CommandKind::ResetAllPlayStatistics);
    all.tracks = {{file, 0}};
    EXPECT_THROW(server->submit(all), std::invalid_argument);
    all.tracks.clear();
    EXPECT_FALSE(send(all).commandError);
    server.reset();
}

TEST(ServerGaplessTest, ReorderReplacesTheQueuedFileNotTheOneAlreadyDecoding) {
    const std::vector<std::filesystem::path> files = {testutil::testFile("24-96k"),
                                                      testutil::generatedFile({"gapless-b-24-96k", 96000, 24, 2, 0.3}),
                                                      testutil::generatedFile({"gapless-c-24-96k", 96000, 24, 2, 0.4}),
                                                      testutil::generatedFile({"gapless-d-24-96k", 96000, 24, 2, 0.5})};
    const auto capture = testutil::tempDir() / "server-gapless-reorder.raw";
    CaptureOptions options;
    options.file = capture;
    options.accepts = [](const DeviceFormat& f) { return f.type == SampleType::Int32; };
    options.realtime = true;
    std::atomic<int> opens = 0;
    {
        LocalServer server(Settings{}, {}, [&](const Settings&) {
            return std::make_unique<CountingOutput>(createCaptureOutput(options), opens);
        });
        std::vector<TrackSource> sources;
        for (const auto& file : files) sources.emplace_back(toUtf8(file.wstring()));
        server.submit(queueReplace(sources));
        server.waitIdle();
        const auto ids = entryIds(server.snapshot());
        std::this_thread::sleep_for(500ms); // b is already decoding behind a; c waits in the slot
        Command reorder = command(CommandKind::QueueReorder);
        reorder.entryIds = {ids[0], ids[1], ids[3], ids[2]};
        server.submit(reorder);
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        Snapshot s = server.snapshot();
        while (!(s.queueEntryId == ids[2] && s.playback.state == PlaybackState::Stopped) &&
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

TEST(ServerGaplessTest, SeekRestartsTheStreamAndQueuesTheNextFileAgain) {
    const std::vector<std::filesystem::path> files = {testutil::testFile("24-96k"),
                                                      testutil::generatedFile({"gapless-b-24-96k", 96000, 24, 2, 0.3})};
    const auto capture = testutil::tempDir() / "server-gapless-seek.raw";
    CaptureOptions options;
    options.file = capture;
    options.accepts = [](const DeviceFormat& f) { return f.type == SampleType::Int32; };
    options.realtime = true;
    std::atomic<int> opens = 0;
    {
        LocalServer server(Settings{}, {}, [&](const Settings&) {
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
        while (!(s.queueEntryId == ids[1] && s.playback.state == PlaybackState::Stopped) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(10ms);
            s = server.snapshot();
        }
        EXPECT_EQ(s.queueEntryId, ids[1]);
        EXPECT_FALSE(s.playback.error) << s.playback.errorDetail;
    }
    EXPECT_EQ(opens.load(), 2); // the seek reopens; the next file then follows without another open
    const auto first = testaudio::referenceDecode(files[0]);
    auto expected = std::vector<int32_t>(first.samples.begin() + 96000 * 3, first.samples.end());
    const auto second = concatenatedReferences({files[1]});
    expected.insert(expected.end(), second.begin(), second.end());
    EXPECT_TRUE(capturedSamples(capture) == expected);
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
