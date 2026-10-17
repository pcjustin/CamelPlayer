#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <fstream>
#include <mutex>
#include <thread>

#include "server/LocalServer.h"
#include "audio/output/CaptureOutput.h"
#include "audio/engine/SamplePacker.h"
#include "util/Text.h"
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
};

class ManualOutput final : public IOutput {
public:
    explicit ManualOutput(Probe& probe) : probe_(probe) {}
    bool supports(const DeviceFormat&) override { return true; }
    void open(const DeviceFormat&, IRenderSource*) override {
        ++probe_.opens;
        if (probe_.onOpen) probe_.onOpen();
    }
    void start() override {}
    void pause() override {}
    void close() override { ++probe_.closes; }
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
    EXPECT_EQ(s.settings.decoder, DecoderKind::Sndfile);
    send(command(CommandKind::Pause));
    Command c = command(CommandKind::ApplySettings);
    c.settings = s.settings;
    c.settings->decoder = DecoderKind::DrFlac;
    s = send(c);
    EXPECT_EQ(s.playback.state, PlaybackState::Paused);
    EXPECT_EQ(s.playback.decoder, DecoderKind::Sndfile);
    EXPECT_EQ(s.settings.decoder, DecoderKind::DrFlac);
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
    EXPECT_EQ(server->snapshot().command.id, 0u);
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
            settings.decoder = GetParam();
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

INSTANTIATE_TEST_SUITE_P(AllDecoders, ServerCaptureTest, ::testing::ValuesIn(kAllDecoders));

} // namespace
