// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include <atomic>
#include <fstream>
#include <optional>
#include <thread>
#include <mutex>
#include <vector>
#include <wil/resource.h>

#include "Engine/PlaybackEngine.h"
#include "Engine/decode/IDecoder.h"
#include "Common/DecoderRegistry.h"
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

class EngineStateTest : public ::testing::Test {
protected:
    OutputProbe probe;
    ManualOutput* output = nullptr;
    Settings settings;
    std::unique_ptr<PlaybackEngine> engine;

    void SetUp() override {
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

TEST_F(EngineStateTest, IdleCommandsDoNotCreateOutput) {
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

TEST_F(EngineStateTest, LoadPublishesMetadataWithoutPlaying) {
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

TEST_F(EngineStateTest, PauseResumeAndTogglePreservePosition) {
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

TEST_F(EngineStateTest, RendersNothingWhilePaused) {
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

TEST_F(EngineStateTest, QueuedFileFollowsWithoutReopeningTheDevice) {
    const auto next = testutil::generatedFile({"gapless-next-24-96k", 96000, 24, 2, 0.5});
    open();
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

TEST_F(EngineStateTest, QueuedFileOfAnotherContainerFollowsWithoutReopeningTheDevice) {
    const testaudio::GenSpec spec{"gapless-next-24-96k-wav", 96000, 24, 2, 0.5};
    const auto next = testutil::tempDir() / "gapless-next-24-96k.wav";
    testaudio::writeWav(next, testaudio::makeSignal(spec), spec.sampleRate, spec.bits, spec.channels);
    open();
    ASSERT_TRUE(engine->queueNext(next));
    for (int i = 0; i < 200 && !engine->snapshot().nextSlotFree; ++i) std::this_thread::sleep_for(5ms);
    ASSERT_TRUE(engine->snapshot().nextSlotFree);
    size_t frames = 0;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    bool sawWav = false;
    while (engine->snapshot().state != PlayState::Stopped && std::chrono::steady_clock::now() < deadline) {
        frames += pull(4096);
        engine->waitIdle();
        const auto s = engine->snapshot();
        if (s.transitions == 1 && s.file == next.wstring()) {
            sawWav = true;
            EXPECT_EQ(s.format, AudioFormat::Wav);
            EXPECT_EQ(s.decoder, DecoderKind::Sndfile);
            EXPECT_EQ(s.total, 48000u);
        }
    }
    const auto s = engine->snapshot();
    EXPECT_TRUE(sawWav);
    EXPECT_EQ(s.transitions, 1u);
    EXPECT_FALSE(s.error) << s.errorDetail;
    EXPECT_EQ(frames, 192000u + 48000u);
    EXPECT_EQ(probe.opens, 1);
}

TEST_F(EngineStateTest, QueuedFileWithAnotherOutputFormatIsRefused) {
    open();
    EXPECT_FALSE(engine->queueNext(testutil::testFile("16-44.1k")));
    EXPECT_EQ(engine->snapshot().queuedFiles, 0u);
}

// The player supports 16/24-bit sources only; other depths are reported and never reach the device.
TEST_F(EngineStateTest, OtherBitDepthsAreRefusedWithoutOpeningTheDevice) {
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

TEST_F(EngineStateTest, StopClearsOutputAndNextPlayStartsAtZero) {
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

TEST_F(EngineStateTest, SeekWhilePausedKeepsPausedState) {
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

TEST_F(EngineStateTest, SeekWhileStoppedIsUsedByNextPlay) {
    engine->load(testutil::testFile("24-96k"));
    engine->seek(54321);
    engine->waitIdle();
    EXPECT_EQ(engine->snapshot().position, 54321u);
    EXPECT_EQ(probe.opens, 0);
    engine->play();
    engine->waitIdle();
    EXPECT_EQ(engine->snapshot().position, 54321u);
}

TEST_F(EngineStateTest, SeekPastEndRestartsAtBeginning) {
    open();
    engine->seek(192000);
    engine->waitIdle();
    EXPECT_EQ(engine->snapshot().position, 0u);
    EXPECT_FALSE(engine->snapshot().error);
}

TEST_F(EngineStateTest, NaturalEndResetsPositionAndClosesOutput) {
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

TEST_F(EngineStateTest, InactiveBackendSettingsDoNotRestartPlayback) {
    open();
    settings.asio.bufferSize = 512;
    settings.ks.packetCount = 8;
    settings.language = "en-US";
    engine->applySettings(settings);
    engine->waitIdle();
    EXPECT_EQ(probe.opens, 1);
    EXPECT_EQ(probe.creations, 1);
}

TEST_F(EngineStateTest, DecoderSwitchPreservesPausedPosition) {
    open();
    ASSERT_EQ(pull(17), 17u);
    engine->pause();
    settings.decoderByFormat[AudioFormat::Flac] = DecoderKind::Symphonia;
    engine->applySettings(settings);
    engine->waitIdle();
    EXPECT_EQ(engine->snapshot().decoder, DecoderKind::Symphonia);
    EXPECT_EQ(engine->snapshot().position, 17u);
    EXPECT_EQ(engine->snapshot().state, PlayState::Paused);
    EXPECT_EQ(probe.opens, 2);
    EXPECT_EQ(probe.creations, 1);
}

TEST_F(EngineStateTest, BackendSwitchRecreatesOutputAtCurrentPosition) {
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

TEST_F(EngineStateTest, BackendSwitchPreservesPausedPosition) {
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

TEST_F(EngineStateTest, AsynchronousDeviceLossPausesAndPlayRecovers) {
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

TEST_F(EngineStateTest, DriverRestartPreservesPositionAndState) {
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

TEST_F(EngineStateTest, QueuedOldSessionErrorDoesNotAffectNewSession) {
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

TEST_F(EngineStateTest, DeviceBusyOnOpenPausesAndAllowsRetry) {
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

TEST_F(EngineStateTest, UnsupportedFormatNeverOpensDevice) {
    probe.accepts = false;
    open();
    EXPECT_EQ(probe.opens, 0);
    EXPECT_EQ(engine->snapshot().error, ErrorCode::FormatNotSupported);
    EXPECT_EQ(engine->snapshot().state, PlayState::Stopped);
}

TEST_F(EngineStateTest, UnsupportedFormatFailsWithoutSubstitutingADecoder) {
    open();
    EXPECT_EQ(engine->snapshot().format, AudioFormat::Flac);
    const auto wav = testutil::tempDir() / "engine-input.flac"; // the extension is ignored
    std::ofstream(wav, std::ios::binary | std::ios::trunc) << std::string("RIFF\x04\0\0\0WAVEfmt ", 16);
    EXPECT_FALSE(engine->queueNext(wav));
    engine->openAndPlay(wav);
    engine->waitIdle();
    const auto snapshot = engine->snapshot();
    EXPECT_EQ(snapshot.state, PlayState::Stopped);
    EXPECT_EQ(snapshot.error, ErrorCode::UnsupportedStream);
    EXPECT_EQ(snapshot.format, AudioFormat::Wav);
    EXPECT_FALSE(snapshot.source);
    EXPECT_FALSE(snapshot.output);
}

TEST_F(EngineStateTest, MissingNewFileDoesNotPublishPreviousTrackMetadata) {
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

TEST_F(EngineStateTest, StartFailureIsPublishedAndSessionIsClosed) {
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

TEST_F(EngineStateTest, UnexpectedCommandExceptionIsPublished) {
    probe.unexpectedStartError = true;
    open();
    EXPECT_EQ(engine->snapshot().error, ErrorCode::Other);
    EXPECT_EQ(engine->snapshot().state, PlayState::Stopped);
    EXPECT_EQ(probe.source, nullptr);
}

TEST_F(EngineStateTest, ResumeFailureIsPublished) {
    open();
    engine->pause();
    engine->waitIdle();
    probe.startError = ErrorCode::DeviceLost;
    engine->play();
    engine->waitIdle();
    EXPECT_EQ(engine->snapshot().error, ErrorCode::DeviceLost);
    EXPECT_FALSE(engine->snapshot().output);
}

TEST_F(EngineStateTest, PauseFailureIsPublished) {
    open();
    probe.pauseError = ErrorCode::DeviceLost;
    engine->pause();
    engine->waitIdle();
    EXPECT_EQ(engine->snapshot().error, ErrorCode::DeviceLost);
    EXPECT_EQ(probe.source, nullptr);
}

TEST_F(EngineStateTest, ListenerMayReadSnapshotAndCanBeRemoved) {
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

TEST_F(EngineStateTest, DestructionClosesActiveSession) {
    open();
    engine.reset();
    EXPECT_EQ(probe.source, nullptr);
    EXPECT_GT(probe.closes, 0);
}

TEST_F(EngineStateTest, TruncatedAudioPublishesDecodeFailureAndNextFileRecovers) {
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

TEST_F(EngineStateTest, LostSourcePausesWithoutEndingTheTrackAndRetriesAtTheSavedPosition) {
    const auto file = testutil::tempDir() / "interrupted-source.flac";
    std::filesystem::copy_file(testutil::testFile("16-44.1k"), file, std::filesystem::copy_options::overwrite_existing);
    const auto info = openDecoder(DecoderKind::Sndfile, file)->info();
    const auto original = decoderRegistry().find("sndfile")->open;
    auto restore = wil::scope_exit([&] { decoderRegistry().setOpener("sndfile", original); });
    auto interrupt = std::make_shared<std::atomic<bool>>(false);
    class Source final : public IDecoder {
    public:
        Source(SourceInfo info, std::filesystem::path path, std::shared_ptr<std::atomic<bool>> interrupt)
            : info_(info), path_(std::move(path)), interrupt_(std::move(interrupt)) { info_.totalFrames = 44100 * 60; }
        const SourceInfo& info() const override { return info_; }
        size_t read(int32_t* dst, size_t frames) override {
            if (interrupt_->load()) {
                std::filesystem::remove(path_);
                throw Error(ErrorCode::DecodeFailed, "injected source interruption");
            }
            const auto count = size_t(std::min<uint64_t>(frames, info_.totalFrames - position_));
            std::fill_n(dst, count * info_.channels, 0);
            position_ += count;
            return count;
        }
        void seek(uint64_t frame) override { position_ = frame; }
    private:
        SourceInfo info_;
        std::filesystem::path path_;
        std::shared_ptr<std::atomic<bool>> interrupt_;
        uint64_t position_ = 0;
    };
    decoderRegistry().setOpener("sndfile", [=](const std::filesystem::path& path, std::string_view, uint32_t) {
        return std::make_unique<Source>(info, path, interrupt);
    });
    engine->openAndPlay(file, DecoderKind::Sndfile);
    engine->waitIdle();
    ASSERT_EQ(engine->snapshot().state, PlayState::Playing);
    const auto ends = engine->snapshot().trackEnds;
    interrupt->store(true);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (engine->snapshot().state == PlayState::Playing && std::chrono::steady_clock::now() < deadline) {
        pull(8192);
        std::this_thread::sleep_for(1ms);
    }
    engine->waitIdle();
    const auto paused = engine->snapshot();
    ASSERT_EQ(paused.state, PlayState::Paused);
    EXPECT_FALSE(std::filesystem::exists(file));
    EXPECT_EQ(paused.trackEnds, ends);
    EXPECT_GT(paused.position, 0u);
    EXPECT_NE(paused.errorDetail.find("Source unavailable"), std::string::npos);
    engine->play();
    engine->waitIdle();
    EXPECT_EQ(engine->snapshot().state, PlayState::Paused);
    EXPECT_EQ(engine->snapshot().position, paused.position);
    EXPECT_EQ(engine->snapshot().trackEnds, ends);
    std::filesystem::copy_file(testutil::testFile("16-44.1k"), file);
    interrupt->store(false);
    engine->play();
    engine->waitIdle();
    EXPECT_EQ(engine->snapshot().state, PlayState::Playing);
    EXPECT_EQ(engine->snapshot().position, paused.position);
    EXPECT_FALSE(engine->snapshot().error);
    engine->stop();
    engine->waitIdle();
}

TEST_F(EngineStateTest, DiscardPreloadsWithdrawsAnAlreadyDecodedSuccessor) {
    open();
    ASSERT_TRUE(engine->queueNext(testutil::generatedFile({"withdraw-next", 96000, 24, 2, 0.3})));
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!engine->snapshot().nextSlotFree && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    ASSERT_TRUE(engine->snapshot().nextSlotFree);
    ASSERT_EQ(engine->snapshot().queuedFiles, 1u);
    ASSERT_EQ(pull(4096), 4096u);
    const auto before = engine->snapshot();
    engine->discardPreloads();
    const auto after = engine->snapshot();
    EXPECT_EQ(after.queuedFiles, 0u);
    EXPECT_EQ(after.file, before.file);
    EXPECT_EQ(after.position, before.position);
    EXPECT_EQ(after.state, PlayState::Playing);
    EXPECT_EQ(after.transitions, before.transitions);
    EXPECT_EQ(after.trackEnds, before.trackEnds);
}

TEST_F(EngineStateTest, DiscardPreloadsPreservesTheTrackThatJustBecameAudible) {
    open();
    const auto first = engine->snapshot();
    const auto next = testutil::generatedFile({"withdraw-audible", 96000, 24, 2, 0.3});
    ASSERT_TRUE(engine->queueNext(next));
    const auto waitDecoded = [&] {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!engine->snapshot().nextSlotFree && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        return engine->snapshot().nextSlotFree;
    };
    ASSERT_TRUE(waitDecoded());
    ASSERT_TRUE(engine->queueNext(testutil::generatedFile({"withdraw-future", 96000, 24, 2, 0.4})));
    ASSERT_EQ(pull(size_t(first.total)), first.total);
    ASSERT_TRUE(waitDecoded());
    ASSERT_EQ(pull(64), 64u);
    engine->discardPreloads();
    const auto current = engine->snapshot();
    EXPECT_EQ(current.file, next.wstring());
    EXPECT_EQ(current.position, 64u);
    EXPECT_EQ(current.queuedFiles, 0u);
    EXPECT_EQ(current.state, PlayState::Playing);
    EXPECT_EQ(current.transitions, first.transitions + 1);
}

TEST_F(EngineStateTest, LoadingAnotherTrackClosesOutputAndResetsPosition) {
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

TEST_F(EngineStateTest, SettingsWhileStoppedPublishWithoutCreatingOutput) {
    settings.backend = Backend::Ks;
    settings.decoderByFormat[AudioFormat::Flac] = DecoderKind::DrFlac;
    engine->applySettings(settings);
    engine->waitIdle();
    EXPECT_EQ(engine->snapshot().backend, Backend::Ks);
    EXPECT_EQ(engine->snapshot().decoder, DecoderKind::DrFlac);
    EXPECT_EQ(probe.creations, 0);
}

TEST_F(EngineStateTest, ActiveBackendConfigurationChangeRecreatesOutput) {
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

TEST_F(EngineStateTest, FixedContainerCannotTruncateSource) {
    settings.wasapi.sampleType = SampleType::Int16;
    engine->applySettings(settings);
    open();
    EXPECT_EQ(probe.opens, 0);
    EXPECT_EQ(engine->snapshot().error, ErrorCode::FormatNotSupported);
}

TEST_F(EngineStateTest, ThrowingListenerDoesNotStopPlaybackOrControlThread) {
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
