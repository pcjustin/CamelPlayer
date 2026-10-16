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
        server = std::make_unique<LocalServer>(Settings{}, std::filesystem::path{},
                                               [&](const Settings&) { return std::make_unique<ManualOutput>(probe); });
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
    EXPECT_EQ(cursor.current()->revision, 10u);
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
    EXPECT_EQ(s.playback.source->bitsPerSample, 16u);
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
    std::promise<void> opened, release;
    auto gate = release.get_future().share();
    probe.onOpen = [&] {
        opened.set_value();
        gate.wait();
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
        std::promise<void>& promise;
        ~Release() { promise.set_value(); }
    };
    {
        Release unblock{release};
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
    EXPECT_EQ(s.settings.decoder, DecoderKind::FFmpeg);
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
    server.submit(load);
    Command c = command(CommandKind::ApplySettings);
    c.settings = stale;
    c.settings->language = "en-US";
    server.submit(c);
    server.waitIdle();
    auto s = server.snapshot();
    EXPECT_FALSE(s.commandError);
    EXPECT_EQ(s.settings.lastFileUtf8, load.source->pathUtf8());
    auto saved = loadSettings(path);
    EXPECT_EQ(saved.language, "en-US");
    EXPECT_EQ(toUtf8(saved.lastFile), load.source->pathUtf8());
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
