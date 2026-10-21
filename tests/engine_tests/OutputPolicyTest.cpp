// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// WASAPI shared mode and follow-default (docs/Plan.md §2, §5.4; M2-R08) through simulated devices: shared mode always
// converts to the mixer's format and is processed; a followed default moves an open stream and resumes only active
// playback; a fixed device pauses on loss and never follows the default.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

#include "DsdFixture.h"
#include "Engine/PlaybackEngine.h"
#include "OutputBridge/CaptureOutput.h"
#include "StartedSnapshot.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::audio;
using namespace std::chrono_literals;

namespace {

// The default device the test controls: whether it is there, how often it was opened, and the output now open on it.
struct DefaultDevice {
    std::atomic<bool> present{true};
    std::atomic<int> opens{0};
    std::function<bool(const DeviceFormat&)> accepts = [](const DeviceFormat& f) { return f.type == SampleType::Int32; };
};

class TestOutput final : public IOutput {
public:
    TestOutput(DefaultDevice& device, std::unique_ptr<IOutput> inner) : device_(device), inner_(std::move(inner)) {}
    bool supports(const DeviceFormat& f) override {
        requirePresent();
        return inner_->supports(f);
    }
    void open(const DeviceFormat& f, IRenderSource* source) override {
        requirePresent();
        ++device_.opens;
        inner_->open(f, source);
    }
    void start() override { inner_->start(); }
    void pause() override { inner_->pause(); }
    void close() override { inner_->close(); }
    OutputStatus status() const override { return inner_->status(); }
    void lose() { raiseError(Error(ErrorCode::DeviceLost, "test device removed")); }

private:
    void requirePresent() const {
        if (!device_.present) throw Error(ErrorCode::DeviceNotFound, "no Windows default output device");
    }
    DefaultDevice& device_;
    std::unique_ptr<IOutput> inner_;
};

class NoWatcher final : public DeviceWatcher {};

// Stands in for the Windows endpoint notifications.
struct Watcher {
    std::function<void()> fire;
    std::atomic<int> created{0};
    DeviceWatcherFactory factory() {
        return [this](std::function<void()> onChange) {
            fire = std::move(onChange);
            ++created;
            return std::make_unique<NoWatcher>();
        };
    }
};

struct Rig {
    DefaultDevice device;
    Watcher watcher;
    std::atomic<TestOutput*> current{nullptr};
    std::unique_ptr<PlaybackEngine> engine;

    explicit Rig(const Settings& settings) {
        CaptureOptions options;
        options.file = testutil::tempDir() / "output-policy.raw";
        options.realtime = true; // the stream lasts while the test acts on it
        options.accepts = [this](const DeviceFormat& f) { return device.accepts(f); };
        engine = std::make_unique<PlaybackEngine>(
            settings,
            [this, options](const Settings&) {
                auto out = std::make_unique<TestOutput>(device, createCaptureOutput(options));
                current = out.get();
                return out;
            },
            watcher.factory());
    }
};

bool waitFor(PlaybackEngine& engine, const std::function<bool(const EngineSnapshot&)>& done) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        engine.waitIdle();
        if (done(engine.snapshot())) return true;
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

const auto playing = [](const EngineSnapshot& s) { return s.state == PlayState::Playing && s.position > 0; };

} // namespace

TEST(SharedMode, ConvertsToTheMixFormatWithoutTheConvertSetting) {
    Settings settings;
    settings.wasapi.shared = true;
    ASSERT_FALSE(settings.convertWhenUnsupported);
    Rig rig(settings);
    rig.device.accepts = [](const DeviceFormat& f) { return f == DeviceFormat{48000, 2, SampleType::Float32}; };
    rig.engine->openAndPlay(testutil::testFile("16-44.1k"));
    rig.engine->waitIdle();
    const auto s = rig.engine->snapshot();
    ASSERT_FALSE(s.error) << s.errorDetail;
    ASSERT_TRUE(s.plan);
    EXPECT_TRUE(s.plan->shared);
    EXPECT_EQ(s.plan->state, PipelineState::Processed);
    EXPECT_TRUE(s.plan->resample);
    EXPECT_EQ(s.plan->format, (DeviceFormat{48000, 2, SampleType::Float32}));
    rig.engine->stop();

    // The same mixer-only device in exclusive mode is not converted without permission.
    Settings exclusive;
    Rig exclusiveRig(exclusive);
    exclusiveRig.device.accepts = rig.device.accepts;
    exclusiveRig.engine->openAndPlay(testutil::testFile("16-44.1k"));
    exclusiveRig.engine->waitIdle();
    EXPECT_EQ(exclusiveRig.engine->snapshot().error, ErrorCode::FormatNotSupported);
}

TEST(SharedMode, PlaysDsdOnlyConvertedToPcm) {
    const auto file = testutil::tempDir() / "output-policy.dsf";
    dsdfixture::writeFile(file, dsdfixture::dsf(dsdfixture::rawDsd(20000, 2, 31), {}));
    Settings settings;
    settings.wasapi.shared = true;
    Rig refused(settings);
    refused.device.accepts = [](const DeviceFormat& f) { return f == DeviceFormat{48000, 2, SampleType::Float32}; };
    refused.engine->openAndPlay(file);
    refused.engine->waitIdle();
    const auto r = refused.engine->snapshot();
    EXPECT_EQ(r.error, ErrorCode::FormatNotSupported);
    EXPECT_NE(r.errorDetail.find("shared mode"), std::string::npos) << r.errorDetail;

    settings.wasapi.dsdOutput = DsdOutput::Pcm;
    Rig converted(settings);
    converted.device.accepts = refused.device.accepts;
    testutil::StartedSnapshot started(*converted.engine);
    converted.engine->openAndPlay(file);
    converted.engine->waitIdle();
    const auto c = started.get().value_or(converted.engine->snapshot());
    converted.engine->stop();
    ASSERT_FALSE(c.error) << c.errorDetail;
    ASSERT_TRUE(c.plan);
    EXPECT_EQ(c.plan->dsd, DsdPath::Pcm);
    EXPECT_TRUE(c.plan->shared);
    EXPECT_EQ(c.plan->format, (DeviceFormat{48000, 2, SampleType::Float32}));
}

TEST(FollowDefault, AnOpenStreamMovesToTheNewDefaultAndKeepsPlaying) {
    Rig rig(Settings{});
    ASSERT_TRUE(waitFor(*rig.engine, [&](const EngineSnapshot&) { return rig.watcher.created == 1; }));
    rig.engine->openAndPlay(testutil::testFile("16-44.1k"));
    ASSERT_TRUE(waitFor(*rig.engine, playing));
    const auto before = rig.engine->snapshot().position;
    rig.watcher.fire();
    ASSERT_TRUE(waitFor(*rig.engine, [&](const EngineSnapshot& s) { return rig.device.opens == 2 && playing(s); }));
    const auto s = rig.engine->snapshot();
    EXPECT_FALSE(s.error) << s.errorDetail;
    EXPECT_GE(s.position, before);
    rig.engine->stop();
}

TEST(FollowDefault, PausedPlaybackDoesNotStart) {
    Rig rig(Settings{});
    rig.engine->openAndPlay(testutil::testFile("16-44.1k"));
    ASSERT_TRUE(waitFor(*rig.engine, playing));
    rig.engine->pause();
    rig.engine->waitIdle();
    ASSERT_TRUE(rig.watcher.fire);
    rig.watcher.fire();
    ASSERT_TRUE(waitFor(*rig.engine, [&](const EngineSnapshot&) { return rig.device.opens == 2; }));
    std::this_thread::sleep_for(200ms);
    const auto s = rig.engine->snapshot();
    EXPECT_EQ(s.state, PlayState::Paused);
    EXPECT_FALSE(s.error) << s.errorDetail;
    rig.engine->stop();
}

TEST(FollowDefault, PlaybackLostWithTheOldDefaultResumesOnTheNewOne) {
    Rig rig(Settings{});
    rig.engine->openAndPlay(testutil::testFile("16-44.1k"));
    ASSERT_TRUE(waitFor(*rig.engine, playing));
    rig.current.load()->lose();
    ASSERT_TRUE(waitFor(*rig.engine, [](const EngineSnapshot& s) { return s.state == PlayState::Paused; }));
    EXPECT_EQ(rig.engine->snapshot().error, ErrorCode::DeviceLost);
    rig.watcher.fire();
    ASSERT_TRUE(waitFor(*rig.engine, playing));
    EXPECT_FALSE(rig.engine->snapshot().error);
    rig.engine->stop();
}

TEST(FollowDefault, ANewDefaultThatCannotPlayPausesWithANotice) {
    Rig rig(Settings{});
    rig.engine->openAndPlay(testutil::testFile("16-44.1k"));
    ASSERT_TRUE(waitFor(*rig.engine, playing));
    const auto trackEnds = rig.engine->snapshot().trackEnds;
    rig.device.present = false;
    rig.watcher.fire();
    ASSERT_TRUE(waitFor(*rig.engine, [](const EngineSnapshot& s) { return s.state == PlayState::Paused && s.error; }));
    const auto s = rig.engine->snapshot();
    EXPECT_EQ(s.error, ErrorCode::DeviceNotFound);
    EXPECT_EQ(s.trackEnds, trackEnds); // paused, not ended: the queue does not move on
}

TEST(FixedDevice, PausesOnLossAndNeverFollowsTheDefault) {
    Settings settings;
    settings.wasapi.deviceId = "{fixed-endpoint}";
    Rig rig(settings);
    rig.engine->openAndPlay(testutil::testFile("16-44.1k"));
    ASSERT_TRUE(waitFor(*rig.engine, playing));
    EXPECT_EQ(rig.watcher.created, 0);
    rig.current.load()->lose();
    ASSERT_TRUE(waitFor(*rig.engine, [](const EngineSnapshot& s) { return s.state == PlayState::Paused; }));
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(rig.engine->snapshot().state, PlayState::Paused);
    EXPECT_EQ(rig.device.opens, 1);

    // Switching to follow the default starts watching; switching back stops it.
    settings.wasapi.deviceId.clear();
    rig.engine->applySettings(settings);
    ASSERT_TRUE(waitFor(*rig.engine, [&](const EngineSnapshot&) { return rig.watcher.created == 1; }));
}
