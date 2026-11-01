// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The DSP integration matrix (docs/Plan.md §5.2, §12.1): while no node changes the signal, whether each is off or on
// without effect, the captured output of the 16/24-bit reference matrix and of a gapless sequence is the reference
// decode bit for bit.
#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <thread>

#include "Engine/PlaybackEngine.h"
#include "Engine/SamplePacker.h"
#include "Engine/testing/TestAudio.h"
#include "OutputBridge/CaptureOutput.h"
#include "StartedSnapshot.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::audio;
using namespace std::chrono_literals;

namespace {

// Every node off, though set up to act.
Settings configuredButOff() {
    Settings s;
    s.eq.enabled = false;
    s.eq.bands.front().gainDb = 9.0;
    s.crossfeed = {false, CrossfeedPreset::JanMeier};
    s.convolution = {false, "C:\\Filters\\room.wav"};
    s.volume = {VolumeMode::Fixed, -20.0, false};
    s.replayGain = {ReplayGainMode::Off, 6.0, -6.0, false, AnalysisSpeed::Fast};
    s.crossfade = {false, 5.0};
    return s;
}

// Nodes on that change nothing: a flat EQ, the software volume at 0 dB and crossfade, which a gapless sequence (an
// album in order) does not use.
Settings onButNeutral() {
    Settings s;
    s.eq.enabled = true;
    s.volume = {VolumeMode::Software, 0.0, false};
    s.crossfade = {true, 3.0};
    return s;
}

struct Variant {
    const char* name;
    Settings (*settings)();
};

const Variant kVariants[] = {{"configured_but_off", configuredButOff}, {"on_but_neutral", onButNeutral}};

struct Played {
    std::optional<OutputPlan> plan;
    EngineSnapshot snapshot;
    std::vector<int32_t> samples;
    int opens = 0;
};

// Counts device opens, which a gapless sequence must not repeat.
class CountingOutput final : public IOutput {
public:
    CountingOutput(std::unique_ptr<IOutput> inner, int& opens) : inner_(std::move(inner)), opens_(opens) {}
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
    int& opens_;
};

// Plays `files` one after another without a gap on a capture device that takes only `type`, queuing each as decoding
// moves on to the one before it, and unpacks what reached the device as `bits`-bit samples.
Played play(const Settings& settings, const std::vector<std::filesystem::path>& files, SampleType type, uint32_t bits) {
    const auto out = testutil::tempDir() / "bypass-matrix.raw";
    std::filesystem::remove(out);
    CaptureOptions options;
    options.file = out;
    options.accepts = [type](const DeviceFormat& f) { return f.type == type; };
    // Queuing the following file takes polls; real playback leaves seconds for that.
    options.realtime = files.size() > 1;
    Played played;
    {
        PlaybackEngine engine(settings, [&](const Settings&) {
            return std::make_unique<CountingOutput>(createCaptureOutput(options), played.opens);
        });
        testutil::StartedSnapshot started(engine);
        engine.openAndPlay(files.front());
        engine.waitIdle();
        for (size_t i = 1; i < files.size(); ++i) {
            const auto deadline = std::chrono::steady_clock::now() + 10s;
            while (!engine.snapshot().nextSlotFree && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(5ms);
            EXPECT_TRUE(engine.queueNext(files[i], std::nullopt, 0.0, 0)) << files[i];
        }
        EXPECT_TRUE(engine.waitForState(PlayState::Stopped, 60s));
        played.plan = started.get().value_or(engine.snapshot()).plan;
        played.snapshot = engine.snapshot();
    }
    std::ifstream in(out, std::ios::binary | std::ios::ate);
    const auto size = in ? size_t(in.tellg()) : 0;
    std::vector<uint8_t> bytes(size);
    in.seekg(0);
    if (size) in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size));
    in.close();
    played.samples.resize(size / bytesPerSample(type));
    unpackSamples(bytes.data(), played.samples.size(), bits, type, played.samples.data());
    std::filesystem::remove(out);
    return played;
}

std::vector<int32_t> reference(const std::vector<std::filesystem::path>& files) {
    std::vector<int32_t> samples;
    for (const auto& file : files) {
        const auto decoded = testaudio::referenceDecode(file);
        EXPECT_TRUE(decoded.error.empty()) << decoded.error;
        samples.insert(samples.end(), decoded.samples.begin(), decoded.samples.end());
    }
    return samples;
}

void expectBitPerfect(const Played& played, const std::vector<int32_t>& expected) {
    EXPECT_FALSE(played.snapshot.error) << played.snapshot.errorDetail;
    ASSERT_TRUE(played.plan);
    EXPECT_EQ(played.plan->state, PipelineState::BitPerfect);
    EXPECT_TRUE(played.plan->dsp.empty());
    EXPECT_EQ(played.samples.size(), expected.size());
    EXPECT_TRUE(played.samples == expected);
}

} // namespace

class BypassMatrix : public ::testing::TestWithParam<std::tuple<Variant, const char*>> {};

TEST_P(BypassMatrix, CapturesTheReferenceInItsOwnAndAPaddedContainer) {
    const auto [variant, name] = GetParam();
    const auto file = testutil::acceptanceFile(name);
    const auto decoded = testaudio::referenceDecode(file);
    ASSERT_TRUE(decoded.error.empty()) << decoded.error;
    const uint32_t bits = decoded.info.bitsPerSample;
    const auto native = bits == 16 ? SampleType::Int16 : SampleType::Int24;
    expectBitPerfect(play(variant.settings(), {file}, native, bits), decoded.samples);
    expectBitPerfect(play(variant.settings(), {file}, SampleType::Int32, bits), decoded.samples);
}

INSTANTIATE_TEST_SUITE_P(Reference, BypassMatrix,
                         ::testing::Combine(::testing::ValuesIn(kVariants),
                                            ::testing::Values("16-44.1k", "16-48k", "24-96k", "24-192k", "24-352.8k", "24-384k",
                                                              "24-768k")),
                         [](const auto& info) {
                             std::string name = std::string(std::get<0>(info.param).name) + "_" + std::get<1>(info.param);
                             for (auto& c : name)
                                 if (c == '-' || c == '.') c = '_';
                             return name;
                         });

class BypassGapless : public ::testing::TestWithParam<Variant> {};

TEST_P(BypassGapless, ASequenceConcatenatesBitExactlyThroughOneDeviceOpen) {
    const std::vector<std::filesystem::path> files = {testutil::testFile("24-96k"),
                                                      testutil::generatedFile({"bypass-short-24-96k", 96000, 24, 2, 0.3}),
                                                      testutil::generatedFile({"bypass-tail-24-96k", 96000, 24, 2, 0.7})};
    const auto played = play(GetParam().settings(), files, SampleType::Int32, 24);
    expectBitPerfect(played, reference(files));
    EXPECT_EQ(played.opens, 1);
}

INSTANTIATE_TEST_SUITE_P(Settings, BypassGapless, ::testing::ValuesIn(kVariants),
                         [](const auto& info) { return std::string(info.param.name); });
