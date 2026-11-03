// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// IMA and Microsoft ADPCM in WAVE files, with their own decoder choice: every decoder reads the same 16-bit samples,
// which play unchanged as a lossy source without "convert when unsupported".
#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "Engine/PlaybackEngine.h"
#include "Engine/SamplePacker.h"
#include "Engine/decode/FormatProbe.h"
#include "OutputBridge/CaptureOutput.h"
#include "StartedSnapshot.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::audio;

namespace {

std::filesystem::path adpcmFile(bool microsoft) {
    const auto path = testutil::tempDir() / (microsoft ? "adpcm-ms.wav" : "adpcm-ima.wav");
    static bool written[2] = {};
    if (!written[microsoft]) {
        const testaudio::GenSpec spec{"adpcm", 44100, 16, 2, 1.0};
        testaudio::writeAdpcm(path, testaudio::makeSignal(spec), 44100, 2, microsoft);
        written[microsoft] = true;
    }
    return path;
}

std::vector<int32_t> readAll(IDecoder& d) {
    std::vector<int32_t> out, block(size_t(4096) * d.info().channels);
    while (const size_t got = d.read(block.data(), 4096))
        out.insert(out.end(), block.begin(), block.begin() + ptrdiff_t(got * d.info().channels));
    return out;
}

class Adpcm : public ::testing::TestWithParam<bool> {};

} // namespace

TEST_P(Adpcm, IsFoundByItsFormatTag) {
    EXPECT_EQ(probeFormat(adpcmFile(GetParam())), AudioFormat::Adpcm);
}

TEST_P(Adpcm, EveryDecoderReadsTheSameSamples) {
    const auto path = adpcmFile(GetParam());
    const auto reference = readAll(*openDecoder(DecoderKind::Sndfile, path));
    ASSERT_GE(reference.size(), size_t(44100) * 2);
    for (const DecoderKind kind : decodersFor(AudioFormat::Adpcm)) {
        SCOPED_TRACE(decoderName(kind));
        const auto d = openDecoder(kind, path);
        EXPECT_FALSE(d->readsFloat());
        EXPECT_EQ(d->info().sampleRate, 44100u);
        EXPECT_EQ(d->info().channels, 2u);
        EXPECT_EQ(d->info().bitsPerSample, 16u);
        EXPECT_EQ(d->info().totalFrames * 2, reference.size());
        EXPECT_EQ(readAll(*d), reference) << "differs from libsndfile";
    }
}

TEST(AdpcmSettings, PlaysWithLibsndfileWhateverWaveDecoderIsChosen) {
    DecoderByFormat choices;
    choices[AudioFormat::Wav] = DecoderKind::DrWav;
    EXPECT_EQ(decoderFor(choices, AudioFormat::Adpcm), DecoderKind::Sndfile);
    EXPECT_EQ(decodersFor(AudioFormat::Adpcm), (std::vector<DecoderKind>{DecoderKind::Sndfile}));
}

TEST_P(Adpcm, EveryDecoderSeeksExactly) {
    const auto path = adpcmFile(GetParam());
    for (const DecoderKind kind : decodersFor(AudioFormat::Adpcm)) {
        SCOPED_TRACE(decoderName(kind));
        const auto whole = readAll(*openDecoder(kind, path));
        const auto d = openDecoder(kind, path);
        for (const uint64_t target : {uint64_t(0), uint64_t(1000), uint64_t(20000), whole.size() / 2 - 300}) {
            d->seek(target);
            std::vector<int32_t> block(size_t(256) * 2);
            ASSERT_EQ(d->read(block.data(), 256), 256u) << target;
            EXPECT_TRUE(std::equal(block.begin(), block.end(), whole.begin() + ptrdiff_t(target * 2))) << target;
        }
    }
}

TEST_P(Adpcm, PlaysUnchangedAsALossySourceWithoutConversion) {
    const auto path = adpcmFile(GetParam());
    const auto expected = readAll(*openDecoder(DecoderKind::Sndfile, path));
    const auto out = testutil::tempDir() / "adpcm-capture.raw";
    std::filesystem::remove(out);
    Settings settings;
    settings.convertWhenUnsupported = false;
    CaptureOptions options;
    options.file = out;
    options.accepts = [](const DeviceFormat& f) { return f.sampleRate == 44100 && f.type == SampleType::Int16; };
    std::optional<OutputPlan> plan;
    EngineSnapshot ended;
    {
        PlaybackEngine engine(settings, [&](const Settings&) { return createCaptureOutput(options); });
        testutil::StartedSnapshot started(engine);
        engine.openAndPlay(path);
        engine.waitIdle();
        plan = started.get().value_or(engine.snapshot()).plan;
        EXPECT_TRUE(engine.waitForState(PlayState::Stopped, std::chrono::seconds(30)));
        ended = engine.snapshot();
    }
    ASSERT_FALSE(ended.error) << ended.errorDetail;
    ASSERT_TRUE(plan);
    EXPECT_EQ(plan->state, PipelineState::LossySource);
    EXPECT_EQ(ended.format, AudioFormat::Adpcm);
    std::ifstream in(out, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<int32_t> captured(bytes.size() / 2);
    unpackSamples(reinterpret_cast<const uint8_t*>(bytes.data()), captured.size(), 16, SampleType::Int16, captured.data());
    EXPECT_EQ(captured, expected);
}

INSTANTIATE_TEST_SUITE_P(WaveAdpcm, Adpcm, ::testing::Values(false, true),
                         [](const ::testing::TestParamInfo<bool>& i) { return std::string(i.param ? "Microsoft" : "Ima"); });
