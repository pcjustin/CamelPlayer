// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The DSP chain outside the Engine (docs/Plan.md §5.9): its settings, impulse responses from .wav and .zip files,
// and what the chain does for the path and the headroom.
#include <gtest/gtest.h>
#include <windows.h>

#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <numeric>

#include "Common/settings/Settings.h"
#include "Engine/Dsp.h"
#include "Engine/ImpulseResponse.h"
#include "Server/Support/ZipWriter.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::audio;

namespace {

// A WAV of IEEE float samples, interleaved.
std::string wav(uint32_t rate, uint16_t channels, const std::vector<float>& samples) {
    std::string out;
    const auto put = [&](uint32_t value, int bytes) {
        for (int i = 0; i < bytes; ++i) out.push_back(char((value >> (8 * i)) & 0xff));
    };
    const auto data = uint32_t(samples.size() * sizeof(float));
    out += "RIFF";
    put(36 + data, 4);
    out += "WAVEfmt ";
    put(16, 4);
    put(3, 2); // IEEE float
    put(channels, 2);
    put(rate, 4);
    put(rate * channels * 4, 4);
    put(channels * 4, 2);
    put(32, 2);
    out += "data";
    put(data, 4);
    out.append(reinterpret_cast<const char*>(samples.data()), data);
    return out;
}

std::filesystem::path write(const std::string& name, const std::string& bytes) {
    const auto dir = testutil::tempDir() / "impulse";
    std::filesystem::create_directories(dir);
    const auto path = dir / name;
    std::ofstream(path, std::ios::binary | std::ios::trunc).write(bytes.data(), std::streamsize(bytes.size()));
    return path;
}

// A unit impulse with a short decaying tail.
std::vector<float> response(size_t frames, uint16_t channels) {
    std::vector<float> samples(frames * channels, 0.0f);
    for (size_t i = 0; i < std::min<size_t>(frames, 64); ++i)
        for (uint16_t c = 0; c < channels; ++c) samples[i * channels + c] = float(std::pow(0.5, double(i)) * (c + 1) / channels);
    return samples;
}

// Points the data folder at a test folder while it lives.
class DataFolder {
public:
    DataFolder() {
        dir_ = testutil::tempDir() / "impulse-data";
        std::filesystem::remove_all(dir_);
        SetEnvironmentVariableW(L"CAMELPLAYER_DATA", dir_.c_str());
    }
    ~DataFolder() { SetEnvironmentVariableW(L"CAMELPLAYER_DATA", nullptr); }
    const std::filesystem::path& path() const { return dir_; }

private:
    std::filesystem::path dir_;
};

ImpulseProblem problemOf(const std::filesystem::path& file, uint32_t rate) {
    try {
        loadImpulse(file, rate);
    } catch (const ImpulseError& e) {
        return e.problem();
    }
    ADD_FAILURE() << "loaded " << file.string();
    return ImpulseProblem::Unreadable;
}

} // namespace

TEST(DspSettings, CrossfeedConvolutionAndOrderRoundTripAndFallBack) {
    const Settings defaults;
    EXPECT_FALSE(defaults.crossfeed.enabled);
    EXPECT_FALSE(defaults.convolution.enabled);
    EXPECT_EQ(defaults.dspOrder, kDefaultDspOrder);
    EXPECT_EQ(crossfeedValues(defaults.crossfeed), std::make_pair(700u, 4.5));

    Settings settings;
    settings.crossfeed = {true, CrossfeedPreset::Custom, 1234, 7.5};
    settings.convolution = {true, "D:\\Room\\Filters 48k.zip"};
    settings.dspOrder = {DspNode::Convolution, DspNode::Eq, DspNode::Crossfeed};
    const auto back = fromJson(toJson(settings));
    EXPECT_EQ(back.crossfeed, settings.crossfeed);
    EXPECT_EQ(back.convolution, settings.convolution);
    EXPECT_EQ(back.dspOrder, settings.dspOrder);
    EXPECT_EQ(crossfeedValues(back.crossfeed), std::make_pair(1234u, 7.5));
    EXPECT_EQ(crossfeedValues({true, CrossfeedPreset::ChuMoy}), std::make_pair(700u, 6.0));
    EXPECT_EQ(crossfeedValues({true, CrossfeedPreset::JanMeier}), std::make_pair(650u, 9.5));

    const auto invalid = fromJson(
        R"({"crossfeed":{"enabled":true,"preset":"loud","cutoff":5000,"feed":0.2},"dspOrder":["eq","eq","convolution"]})");
    EXPECT_TRUE(invalid.crossfeed.enabled);
    EXPECT_EQ(invalid.crossfeed.preset, CrossfeedPreset::Default);
    EXPECT_EQ(invalid.crossfeed.cutoffHz, 700u);
    EXPECT_EQ(invalid.crossfeed.feedDb, 4.5);
    EXPECT_EQ(invalid.dspOrder, kDefaultDspOrder);

    settings.eq.presets = {{"Room", defaultEqBands()}};
    const auto reset = resetPlayback(settings);
    EXPECT_EQ(reset.crossfeed, CrossfeedSettings{});
    EXPECT_EQ(reset.convolution, ConvolutionSettings{});
    EXPECT_EQ(reset.dspOrder, kDefaultDspOrder);
    EXPECT_EQ(reset.eq.presets, settings.eq.presets);
}

TEST(ImpulseResponse, ReadsEachChannelLayoutAtItsOwnRate) {
    DataFolder data;
    for (const uint16_t channels : std::array<uint16_t, 3>{1, 2, 4}) {
        const auto file = write("layout" + std::to_string(channels) + ".wav", wav(48000, channels, response(500, channels)));
        const auto loaded = loadImpulse(file, 48000);
        ASSERT_TRUE(loaded.impulse);
        EXPECT_EQ(loaded.info.channels, channels);
        EXPECT_EQ(loaded.info.frames, 500u);
        EXPECT_EQ(loaded.info.rates, std::vector<uint32_t>{48000});
        EXPECT_FALSE(loaded.info.converted);
    }
    EXPECT_EQ(problemOf(write("three.wav", wav(48000, 3, response(100, 3))), 48000), ImpulseProblem::Layout);
    EXPECT_EQ(problemOf(write("text.wav", "not a wave file"), 48000), ImpulseProblem::Unreadable);
    EXPECT_EQ(problemOf(testutil::tempDir() / "impulse" / "missing.wav", 48000), ImpulseProblem::Unreadable);
}

TEST(ImpulseResponse, AZipYieldsItsPlayingRateAndAMissingRateIsConvertedOnceKeepingItsGain) {
    DataFolder data;
    const auto file = testutil::tempDir() / "impulse" / "rates.zip";
    {
        server::ZipWriter zip(file);
        zip.add("Room/filter 44100.wav", wav(44100, 2, response(4410, 2)));
        zip.add("Room/filter 96000.wav", wav(96000, 2, response(9600, 2)));
        zip.add("readme.txt", "two rates");
        zip.finish();
    }
    const auto exact = loadImpulse(file, 96000);
    EXPECT_EQ(exact.info.rates, (std::vector<uint32_t>{44100, 96000}));
    EXPECT_EQ(exact.info.sourceRate, 96000u);
    EXPECT_FALSE(exact.info.converted);

    // 48 kHz is nearest to 44.1 kHz.
    const auto converted = loadImpulse(file, 48000);
    EXPECT_EQ(converted.info.sourceRate, 44100u);
    EXPECT_TRUE(converted.info.converted);
    // The silence after the response's 64 audible taps is trimmed, the resampler's ringing kept.
    EXPECT_GT(converted.info.frames, 64u);
    EXPECT_LT(converted.info.frames, 1000u);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(data.path() / "ir-cache"), std::filesystem::directory_iterator{}),
              1);

    // The converted response keeps the original's level at low frequencies: the sum of its taps.
    DspChain chain;
    chain.order = {CAMEL_DSP_NODE_EQ, CAMEL_DSP_NODE_CROSSFEED, CAMEL_DSP_NODE_CONVOLUTION};
    chain.impulse = converted.impulse;
    // The right channel's taps sum to about 2, twice the left's, so the preamp takes about 6 dB.
    const auto right = response(64, 1);
    const double dc = std::accumulate(right.begin(), right.end(), 0.0);
    const auto analysis = analyzeDsp(chain, 48000);
    EXPECT_TRUE(analysis.active);
    EXPECT_NEAR(analysis.preampDb, -20.0 * std::log10(dc), 0.1);
}

TEST(ImpulseResponse, ALongerResponseThanTheLimitAtThePlayingRateIsRefused) {
    DataFolder data;
    const auto file = write("long.wav", wav(48000, 1, std::vector<float>(size_t(kMaxImpulseTaps) / 2 + 1, 0.001f)));
    EXPECT_TRUE(loadImpulse(file, 48000).impulse);
    // At 96 kHz the same response needs twice the taps.
    EXPECT_EQ(problemOf(file, 96000), ImpulseProblem::TooLong);
}

TEST(DspChain, TheActingNodesFollowTheOrderAndRawDsdHasNone) {
    DataFolder data;
    Settings settings;
    settings.eq.enabled = true;
    settings.eq.bands[0].gainDb = 3.0;
    settings.crossfeed.enabled = true;
    settings.convolution.enabled = true;
    settings.dspOrder = {DspNode::Crossfeed, DspNode::Convolution, DspNode::Eq};
    const auto impulse = loadImpulse(write("chain.wav", wav(44100, 1, response(100, 1))), 44100).impulse;
    const auto chain = dspChain(settings, impulse);
    EXPECT_EQ(actingNodes(chain, 44100, 2), (std::vector{DspNode::Crossfeed, DspNode::Convolution, DspNode::Eq}));
    // Mono has no other ear to feed.
    EXPECT_EQ(actingNodes(chain, 44100, 1), (std::vector{DspNode::Convolution, DspNode::Eq}));
    settings.eq.bands[0].gainDb = 0.0;
    settings.convolution.enabled = false;
    EXPECT_EQ(actingNodes(dspChain(settings, impulse), 44100, 2), std::vector{DspNode::Crossfeed});
    EXPECT_TRUE(actingNodes(dspChain(Settings{}, nullptr), 44100, 2).empty());
}
