#include <gtest/gtest.h>

#include <atomic>
#include <optional>
#include <thread>
#include <mutex>
#include <vector>

#include "audio/engine/PlaybackEngine.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::audio;
using namespace std::chrono_literals;

namespace {

struct OutputProbe {
    std::mutex renderMutex;
    IRenderSource* source = nullptr;
    DeviceFormat format;
    int creations = 0, opens = 0, starts = 0, pauses = 0, closes = 0;
    bool accepts = true;
    std::optional<ErrorCode> openError, startError, pauseError;
    bool unexpectedStartError = false;
};

class ManualOutput final : public IOutput {
public:
    explicit ManualOutput(OutputProbe& probe) : probe_(probe) { ++probe_.creations; }
    bool supports(const DeviceFormat&) override { return probe_.accepts; }
    void open(const DeviceFormat& format, IRenderSource* source) override {
        ++probe_.opens;
        if (probe_.openError) throw Error(*probe_.openError, "injected open failure");
        probe_.format = format;
        probe_.source = source;
    }
    void start() override {
        ++probe_.starts;
        if (probe_.startError) throw Error(*probe_.startError, "injected start failure");
        if (probe_.unexpectedStartError) throw std::runtime_error("unexpected start failure");
    }
    void pause() override {
        ++probe_.pauses;
        if (probe_.pauseError) throw Error(*probe_.pauseError, "injected pause failure");
    }
    void close() override {
        std::lock_guard lock(probe_.renderMutex);
        ++probe_.closes;
        probe_.source = nullptr;
    }
    OutputStatus status() const override { return {"Manual test output", 256, "No hardware"}; }
    void fail(ErrorCode code) { raiseError(Error(code, "injected asynchronous failure")); }
    void restart() { raiseRestart(); }

private:
    OutputProbe& probe_;
};

class EngineStateTest : public ::testing::TestWithParam<EngineKind> {
protected:
    OutputProbe probe;
    ManualOutput* output = nullptr;
    Settings settings;
    std::unique_ptr<PlaybackEngine> engine;

    void SetUp() override {
        settings.engine = GetParam();
        engine = std::make_unique<PlaybackEngine>(settings, [&](const Settings&) {
            auto result = std::make_unique<ManualOutput>(probe);
            output = result.get();
            return result;
        });
    }
    void open() {
        engine->openAndPlay(testutil::testFile("24-96k"));
        engine->waitIdle();
    }
    size_t pull(size_t frames) {
        std::lock_guard lock(probe.renderMutex);
        std::vector<uint8_t> buffer(frames * probe.format.bytesPerFrame());
        return probe.source ? probe.source->render(buffer.data(), frames) : 0;
    }
};

TEST_P(EngineStateTest, IdleCommandsDoNotCreateOutput) {
    engine->play();
    engine->pause();
    engine->togglePause();
    engine->stop();
    engine->waitIdle();
    EXPECT_EQ(probe.creations, 0);
    EXPECT_EQ(engine->snapshot().state, PlayState::Stopped);
    EXPECT_FALSE(engine->showControlPanel());
    EXPECT_FALSE(engine->waitForState(PlayState::Playing, 1ms));
}

TEST_P(EngineStateTest, LoadPublishesMetadataWithoutPlaying) {
    const auto file = testutil::testFile("24-96k");
    engine->load(file);
    engine->waitIdle();
    const auto snapshot = engine->snapshot();
    EXPECT_EQ(snapshot.state, PlayState::Stopped);
    EXPECT_EQ(snapshot.file, file.wstring());
    ASSERT_TRUE(snapshot.source);
    EXPECT_EQ(snapshot.total, 192000u);
    EXPECT_EQ(snapshot.source->bitsPerSample, 24u);
    EXPECT_FALSE(snapshot.output);
    EXPECT_FALSE(snapshot.error);
    EXPECT_EQ(probe.creations, 0);
    engine->play();
    engine->waitIdle();
    EXPECT_EQ(engine->snapshot().state, PlayState::Playing);
}

TEST_P(EngineStateTest, PauseResumeAndTogglePreservePosition) {
    open();
    ASSERT_EQ(engine->snapshot().state, PlayState::Playing);
    ASSERT_EQ(pull(123), 123u);
    engine->pause();
    engine->pause();
    engine->waitIdle();
    EXPECT_EQ(probe.pauses, 1);
    EXPECT_EQ(engine->snapshot().position, 123u);
    EXPECT_EQ(engine->snapshot().state, PlayState::Paused);
    engine->play();
    engine->play();
    engine->waitIdle();
    EXPECT_EQ(probe.starts, 2);
    engine->togglePause();
    engine->waitIdle();
    EXPECT_EQ(engine->snapshot().state, PlayState::Paused);
    engine->togglePause();
    engine->waitIdle();
    EXPECT_EQ(engine->snapshot().state, PlayState::Playing);
    EXPECT_EQ(engine->snapshot().position, 123u);
}

TEST_P(EngineStateTest, RustEngineRendersNothingWhilePaused) {
    if (GetParam() != EngineKind::Rust) GTEST_SKIP() << "the C++ reference ring does not track playback state";
    open();
    ASSERT_EQ(pull(10), 10u);
    engine->pause();
    engine->waitIdle();
    EXPECT_EQ(pull(100), 0u);
    EXPECT_EQ(engine->snapshot().position, 10u);
    engine->play();
    engine->waitIdle();
    EXPECT_EQ(pull(5), 5u);
    EXPECT_EQ(engine->snapshot().position, 15u);
}

TEST_P(EngineStateTest, QueuedFileFollowsWithoutReopeningTheDevice) {
    const auto next = testutil::generatedFile({"gapless-next-24-96k", 96000, 24, 2, 0.5});
    open();
    if (GetParam() != EngineKind::Rust) {
        EXPECT_FALSE(engine->queueNext(next));
        return;
    }
    ASSERT_TRUE(engine->queueNext(next));
    EXPECT_EQ(engine->snapshot().queuedFiles, 1u);
    // A device takes seconds to play the current file; this test pulls it at once, so first let the decoder reach
    // the queued file.
    for (int i = 0; i < 200 && !engine->snapshot().nextSlotFree; ++i) std::this_thread::sleep_for(5ms);
    ASSERT_TRUE(engine->snapshot().nextSlotFree);
    size_t frames = 0;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (engine->snapshot().state != PlayState::Stopped && std::chrono::steady_clock::now() < deadline) {
        frames += pull(4096);
        engine->waitIdle();
        const auto s = engine->snapshot();
        if (s.transitions == 1 && s.file == next.wstring()) {
            EXPECT_EQ(s.queuedFiles, 0u);
            EXPECT_EQ(s.total, 48000u);
            EXPECT_LE(s.position, 48000u);
        }
    }
    const auto s = engine->snapshot();
    EXPECT_EQ(s.state, PlayState::Stopped);
    EXPECT_EQ(s.transitions, 1u);
    EXPECT_FALSE(s.error) << s.errorDetail;
    EXPECT_EQ(frames, 192000u + 48000u);
    EXPECT_EQ(probe.opens, 1);
}

TEST_P(EngineStateTest, QueuedFileWithAnotherOutputFormatIsRefused) {
    open();
    EXPECT_FALSE(engine->queueNext(testutil::testFile("16-44.1k")));
    EXPECT_EQ(engine->snapshot().queuedFiles, 0u);
}

// The player supports 16/24-bit sources only; other depths are reported and never reach the device.
TEST_P(EngineStateTest, OtherBitDepthsAreRefusedWithoutOpeningTheDevice) {
    for (const auto& spec :
         {testaudio::GenSpec{"refused-20-48k", 48000, 20, 2, 0.2}, testaudio::GenSpec{"refused-32-48k", 48000, 32, 2, 0.2}}) {
        engine->openAndPlay(testutil::generatedFile(spec));
        engine->waitIdle();
        const auto s = engine->snapshot();
        EXPECT_EQ(s.state, PlayState::Stopped) << spec.name;
        EXPECT_EQ(s.error, ErrorCode::UnsupportedStream) << spec.name;
    }
    EXPECT_EQ(probe.opens, 0);
}

TEST_P(EngineStateTest, StopClearsOutputAndNextPlayStartsAtZero) {
    open();
    ASSERT_EQ(pull(100), 100u);
    engine->stop();
    engine->waitIdle();
    EXPECT_EQ(engine->snapshot().position, 0u);
    EXPECT_FALSE(engine->snapshot().bitPerfect());
    EXPECT_EQ(probe.source, nullptr);
    engine->play();
    engine->waitIdle();
    EXPECT_EQ(engine->snapshot().position, 0u);
    EXPECT_EQ(engine->snapshot().state, PlayState::Playing);
}

TEST_P(EngineStateTest, SeekWhilePausedKeepsPausedState) {
    open();
    engine->pause();
    engine->seek(12345);
    engine->waitIdle();
    EXPECT_EQ(engine->snapshot().state, PlayState::Paused);
    EXPECT_EQ(engine->snapshot().position, 12345u);
    EXPECT_EQ(probe.starts, 1);
    engine->play();
    engine->waitIdle();
    ASSERT_EQ(pull(7), 7u);
    EXPECT_EQ(engine->snapshot().position, 12352u);
}

TEST_P(EngineStateTest, SeekWhileStoppedIsUsedByNextPlay) {
    engine->load(testutil::testFile("24-96k"));
    engine->seek(54321);
    engine->waitIdle();
    EXPECT_EQ(engine->snapshot().position, 54321u);
    EXPECT_EQ(probe.opens, 0);
    engine->play();
    engine->waitIdle();
    EXPECT_EQ(engine->snapshot().position, 54321u);
}

TEST_P(EngineStateTest, SeekPastEndRestartsAtBeginning) {
    open();
    engine->seek(192000);
    engine->waitIdle();
    EXPECT_EQ(engine->snapshot().position, 0u);
    EXPECT_FALSE(engine->snapshot().error);
}

TEST_P(EngineStateTest, NaturalEndResetsPositionAndClosesOutput) {
    open();
    size_t frames = 0;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (engine->snapshot().state != PlayState::Stopped && std::chrono::steady_clock::now() < deadline) {
        frames += pull(4096);
        engine->waitIdle();
    }
    EXPECT_EQ(frames, 192000u);
    ASSERT_TRUE(engine->waitForState(PlayState::Stopped, 2s));
    EXPECT_EQ(engine->snapshot().position, 0u);
    EXPECT_FALSE(engine->snapshot().output);
    EXPECT_FALSE(engine->snapshot().error);
    EXPECT_EQ(probe.source, nullptr);
}

TEST_P(EngineStateTest, InactiveBackendSettingsDoNotRestartPlayback) {
    open();
    settings.asio.bufferSize = 512;
    settings.ks.packetCount = 8;
    settings.language = "en-US";
    engine->applySettings(settings);
    engine->waitIdle();
    EXPECT_EQ(probe.opens, 1);
    EXPECT_EQ(probe.creations, 1);
}

TEST_P(EngineStateTest, DecoderSwitchPreservesPausedPosition) {
    open();
    ASSERT_EQ(pull(17), 17u);
    engine->pause();
    settings.decoder = DecoderKind::Symphonia;
    engine->applySettings(settings);
    engine->waitIdle();
    EXPECT_EQ(engine->snapshot().decoder, DecoderKind::Symphonia);
    EXPECT_EQ(engine->snapshot().position, 17u);
    EXPECT_EQ(engine->snapshot().state, PlayState::Paused);
    EXPECT_EQ(probe.opens, 2);
    EXPECT_EQ(probe.creations, 1);
}

TEST_P(EngineStateTest, BackendSwitchRecreatesOutputAtCurrentPosition) {
    open();
    ASSERT_EQ(pull(23), 23u);
    settings.backend = Backend::Ks;
    engine->applySettings(settings);
    engine->waitIdle();
    EXPECT_EQ(engine->snapshot().backend, Backend::Ks);
    EXPECT_EQ(engine->snapshot().position, 23u);
    EXPECT_EQ(engine->snapshot().state, PlayState::Playing);
    EXPECT_EQ(probe.creations, 2);
}

TEST_P(EngineStateTest, BackendSwitchPreservesPausedPosition) {
    open();
    ASSERT_EQ(pull(23), 23u);
    engine->pause();
    engine->waitIdle();
    settings.backend = Backend::Ks;
    engine->applySettings(settings);
    engine->waitIdle();
    EXPECT_EQ(engine->snapshot().backend, Backend::Ks);
    EXPECT_EQ(engine->snapshot().position, 23u);
    EXPECT_EQ(engine->snapshot().state, PlayState::Paused);
    EXPECT_EQ(probe.creations, 2);
}

TEST_P(EngineStateTest, AsynchronousDeviceLossPausesAndPlayRecovers) {
    open();
    ASSERT_EQ(pull(41), 41u);
    output->fail(ErrorCode::DeviceLost);
    engine->waitIdle();
    EXPECT_EQ(engine->snapshot().state, PlayState::Paused);
    EXPECT_EQ(engine->snapshot().error, ErrorCode::DeviceLost);
    EXPECT_EQ(engine->snapshot().position, 41u);
    EXPECT_FALSE(engine->snapshot().output);
    engine->play();
    engine->waitIdle();
    EXPECT_EQ(engine->snapshot().state, PlayState::Playing);
    EXPECT_FALSE(engine->snapshot().error);
    EXPECT_EQ(engine->snapshot().position, 41u);
    EXPECT_EQ(probe.creations, 2);
}

TEST_P(EngineStateTest, DriverRestartPreservesPositionAndState) {
    open();
    ASSERT_EQ(pull(29), 29u);
    engine->pause();
    engine->waitIdle();
    output->restart();
    engine->waitIdle();
    EXPECT_EQ(engine->snapshot().state, PlayState::Paused);
    EXPECT_EQ(engine->snapshot().position, 29u);
    EXPECT_EQ(probe.creations, 2);
}

TEST_P(EngineStateTest, QueuedOldSessionErrorDoesNotAffectNewSession) {
    open();
    engine->stop();
    output->fail(ErrorCode::DeviceLost);
    engine->openAndPlay(testutil::testFile("16-44.1k"));
    engine->waitIdle();
    EXPECT_EQ(engine->snapshot().state, PlayState::Playing);
    EXPECT_FALSE(engine->snapshot().error);
    ASSERT_TRUE(engine->snapshot().source);
    EXPECT_EQ(engine->snapshot().source->bitsPerSample, 16u);
}

TEST_P(EngineStateTest, DeviceBusyOnOpenPausesAndAllowsRetry) {
    probe.openError = ErrorCode::DeviceInUse;
    open();
    EXPECT_EQ(engine->snapshot().state, PlayState::Paused);
    EXPECT_EQ(engine->snapshot().error, ErrorCode::DeviceInUse);
    probe.openError.reset();
    engine->play();
    engine->waitIdle();
    EXPECT_EQ(engine->snapshot().state, PlayState::Playing);
    EXPECT_FALSE(engine->snapshot().error);
}

TEST_P(EngineStateTest, UnsupportedFormatNeverOpensDevice) {
    probe.accepts = false;
    open();
    EXPECT_EQ(probe.opens, 0);
    EXPECT_EQ(engine->snapshot().error, ErrorCode::FormatNotSupported);
    EXPECT_EQ(engine->snapshot().state, PlayState::Stopped);
}

TEST_P(EngineStateTest, MissingNewFileDoesNotPublishPreviousTrackMetadata) {
    open();
    const auto missing = testutil::tempDir() / "missing-engine-input.flac";
    engine->openAndPlay(missing);
    engine->waitIdle();
    const auto snapshot = engine->snapshot();
    EXPECT_EQ(snapshot.state, PlayState::Stopped);
    EXPECT_EQ(snapshot.error, ErrorCode::FileOpen);
    EXPECT_EQ(snapshot.file, missing.wstring());
    EXPECT_FALSE(snapshot.source);
    EXPECT_EQ(snapshot.total, 0u);
    EXPECT_FALSE(snapshot.output);
}

TEST_P(EngineStateTest, StartFailureIsPublishedAndSessionIsClosed) {
    probe.startError = ErrorCode::DeviceLost;
    open();
    EXPECT_EQ(engine->snapshot().error, ErrorCode::DeviceLost);
    EXPECT_EQ(engine->snapshot().state, PlayState::Paused);
    EXPECT_EQ(probe.source, nullptr);
    probe.startError.reset();
    engine->play();
    engine->waitIdle();
    EXPECT_EQ(engine->snapshot().state, PlayState::Playing);
    EXPECT_FALSE(engine->snapshot().error);
}

TEST_P(EngineStateTest, UnexpectedCommandExceptionIsPublished) {
    probe.unexpectedStartError = true;
    open();
    EXPECT_EQ(engine->snapshot().error, ErrorCode::Other);
    EXPECT_EQ(engine->snapshot().state, PlayState::Stopped);
    EXPECT_EQ(probe.source, nullptr);
}

TEST_P(EngineStateTest, ResumeFailureIsPublished) {
    open();
    engine->pause();
    engine->waitIdle();
    probe.startError = ErrorCode::DeviceLost;
    engine->play();
    engine->waitIdle();
    EXPECT_EQ(engine->snapshot().error, ErrorCode::DeviceLost);
    EXPECT_FALSE(engine->snapshot().output);
}

TEST_P(EngineStateTest, PauseFailureIsPublished) {
    open();
    probe.pauseError = ErrorCode::DeviceLost;
    engine->pause();
    engine->waitIdle();
    EXPECT_EQ(engine->snapshot().error, ErrorCode::DeviceLost);
    EXPECT_EQ(probe.source, nullptr);
}

TEST_P(EngineStateTest, ListenerMayReadSnapshotAndCanBeRemoved) {
    std::atomic<int> notifications = 0;
    engine->setListener([&] {
        EXPECT_GT(engine->snapshot().revision, 0u);
        ++notifications;
    });
    open();
    engine->pause();
    engine->waitIdle();
    EXPECT_EQ(notifications.load(), 2);
    engine->setListener({});
    engine->stop();
    engine->waitIdle();
    EXPECT_EQ(notifications.load(), 2);
}

TEST_P(EngineStateTest, DestructionClosesActiveSession) {
    open();
    engine.reset();
    EXPECT_EQ(probe.source, nullptr);
    EXPECT_GT(probe.closes, 0);
}

TEST_P(EngineStateTest, TruncatedAudioPublishesDecodeFailureAndNextFileRecovers) {
    engine->openAndPlay(testutil::testFile("truncated"));
    engine->waitIdle();
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (engine->snapshot().state != PlayState::Stopped && std::chrono::steady_clock::now() < deadline) {
        pull(4096);
        engine->waitIdle();
    }
    ASSERT_TRUE(engine->waitForState(PlayState::Stopped, 2s));
    EXPECT_EQ(engine->snapshot().error, ErrorCode::DecodeFailed);
    EXPECT_FALSE(engine->snapshot().output);
    open();
    EXPECT_EQ(engine->snapshot().state, PlayState::Playing);
    EXPECT_FALSE(engine->snapshot().error);
}

TEST_P(EngineStateTest, LoadingAnotherTrackClosesOutputAndResetsPosition) {
    open();
    ASSERT_EQ(pull(31), 31u);
    engine->load(testutil::testFile("16-44.1k"));
    engine->waitIdle();
    const auto snapshot = engine->snapshot();
    EXPECT_EQ(snapshot.state, PlayState::Stopped);
    EXPECT_EQ(snapshot.position, 0u);
    EXPECT_FALSE(snapshot.output);
    ASSERT_TRUE(snapshot.source);
    EXPECT_EQ(snapshot.source->bitsPerSample, 16u);
    EXPECT_EQ(probe.source, nullptr);
}

TEST_P(EngineStateTest, SettingsWhileStoppedPublishWithoutCreatingOutput) {
    settings.backend = Backend::Ks;
    settings.decoder = DecoderKind::DrFlac;
    engine->applySettings(settings);
    engine->waitIdle();
    EXPECT_EQ(engine->snapshot().backend, Backend::Ks);
    EXPECT_EQ(engine->snapshot().decoder, DecoderKind::DrFlac);
    EXPECT_EQ(probe.creations, 0);
}

TEST_P(EngineStateTest, ActiveBackendConfigurationChangeRecreatesOutput) {
    open();
    ASSERT_EQ(pull(43), 43u);
    settings.wasapi.eventDriven = false;
    engine->applySettings(settings);
    engine->waitIdle();
    EXPECT_EQ(probe.creations, 2);
    EXPECT_EQ(engine->snapshot().position, 43u);
    EXPECT_EQ(engine->snapshot().state, PlayState::Playing);
    engine->applySettings(settings);
    engine->waitIdle();
    EXPECT_EQ(probe.creations, 2);
}

TEST_P(EngineStateTest, FixedContainerCannotTruncateSource) {
    settings.wasapi.sampleType = SampleType::Int16;
    engine->applySettings(settings);
    open();
    EXPECT_EQ(probe.opens, 0);
    EXPECT_EQ(engine->snapshot().error, ErrorCode::FormatNotSupported);
}

TEST_P(EngineStateTest, ThrowingListenerDoesNotStopPlaybackOrControlThread) {
    engine->setListener([] { throw std::runtime_error("injected listener failure"); });
    open();
    EXPECT_EQ(engine->snapshot().state, PlayState::Playing);
    EXPECT_FALSE(engine->snapshot().error);
    engine->pause();
    engine->waitIdle();
    EXPECT_EQ(engine->snapshot().state, PlayState::Paused);
    engine->setListener({});
}

} // namespace

INSTANTIATE_TEST_SUITE_P(BothEngines, EngineStateTest, ::testing::Values(EngineKind::Rust, EngineKind::Cpp),
                         [](const ::testing::TestParamInfo<EngineKind>& i) { return std::string(engineId(i.param)); });
