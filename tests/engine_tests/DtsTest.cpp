// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// DTS-CD: a DTS bitstream posing as 16-bit PCM in a WAVE file. Every WAVE decoder reads it and the DTS stage decodes
// it, but only with "convert when unsupported"; played as PCM it would be loud noise.
#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#include "DtsFixture.h"
#include "Engine/PlaybackEngine.h"
#include "Engine/SamplePacker.h"
#include "Engine/decode/FormatProbe.h"
#include "OutputBridge/CaptureOutput.h"
#include "StartedSnapshot.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::audio;

namespace {

const testutil::DtsCd& dtsCd() {
    static const testutil::DtsCd file = testutil::writeDtsCd(testutil::tempDir() / "dts-cd.wav", 1.0);
    return file;
}

std::filesystem::path dtsPath() {
    dtsCd();
    return testutil::tempDir() / "dts-cd.wav";
}

std::vector<float> readAll(IDecoder& d) {
    std::vector<float> out, block(4096 * d.info().channels);
    while (const size_t got = d.readFloat(block.data(), 4096))
        out.insert(out.end(), block.begin(), block.begin() + ptrdiff_t(got * d.info().channels));
    return out;
}

// A 16-bit stereo PCM WAVE file whose data holds a 14-bit DTS sync word at `offsets` (bytes) in silence.
std::filesystem::path syncsInPcm(const char* name, std::initializer_list<size_t> offsets) {
    std::vector<int32_t> words(16384, 0);
    for (const size_t at : offsets) {
        words[at / 2] = 0x1FFF;
        words[at / 2 + 1] = std::bit_cast<int16_t>(uint16_t{0xE800});
        words[at / 2 + 2] = 0x07F1;
    }
    const auto path = testutil::tempDir() / name;
    testaudio::writeWav(path, words, 44100, 16, 2);
    return path;
}

} // namespace

TEST(Dts, ProbeFindsDtsInsideWave) {
    EXPECT_EQ(probeFormat(dtsPath()), AudioFormat::Dts);
    EXPECT_EQ(probeFormat(syncsInPcm("dts-steady.wav", {12, 4108, 8204})), AudioFormat::Dts);
    // PCM that happens to hold the pattern, but not as a steady stream of frames.
    EXPECT_EQ(probeFormat(syncsInPcm("dts-one.wav", {0})), AudioFormat::Wav);
    EXPECT_EQ(probeFormat(syncsInPcm("dts-two.wav", {0, 4096})), AudioFormat::Wav);
    EXPECT_EQ(probeFormat(syncsInPcm("dts-uneven.wav", {0, 4096, 9000})), AudioFormat::Wav);
}

TEST(Dts, EveryWaveDecoderReadsTheSameDecode) {
    const auto& file = dtsCd();
    ASSERT_GT(file.reference.size(), 6u * 40000);
    for (const DecoderKind kind : {DecoderKind::Sndfile, DecoderKind::DrWav, DecoderKind::Symphonia, DecoderKind::FFmpeg}) {
        SCOPED_TRACE(decoderName(kind));
        const auto d = openDecoder(kind, dtsPath());
        EXPECT_TRUE(d->readsFloat());
        EXPECT_EQ(d->info().sampleRate, 44100u);
        EXPECT_EQ(d->info().channels, 6u);
        EXPECT_EQ(d->info().bitsPerSample, 24u);
        EXPECT_EQ(d->info().totalFrames, file.frames);
        const auto decoded = readAll(*d);
        ASSERT_EQ(decoded.size(), file.frames * 6);
        EXPECT_TRUE(std::equal(decoded.begin(), decoded.end(), file.reference.begin())) << "differs from FFmpeg's decode";
    }
}

TEST(Dts, SeeksToTheExactSample) {
    const auto& file = dtsCd();
    for (const DecoderKind kind : {DecoderKind::Sndfile, DecoderKind::FFmpeg}) {
        SCOPED_TRACE(decoderName(kind));
        const auto d = openDecoder(kind, dtsPath());
        for (const uint64_t target : {uint64_t(0), uint64_t(777), file.frames / 2 + 123, file.frames - 300}) {
            d->seek(target);
            std::vector<float> block(256 * 6);
            ASSERT_EQ(d->readFloat(block.data(), 256), 256u) << target;
            EXPECT_TRUE(std::equal(block.begin(), block.end(), file.reference.begin() + ptrdiff_t(target * 6))) << target;
        }
    }
}

namespace {

struct Played {
    EngineSnapshot snapshot;
    std::optional<OutputPlan> plan;
    std::vector<int32_t> samples;
};

Played playOnInt24(bool convert) {
    const auto out = testutil::tempDir() / "dts-capture.raw";
    std::filesystem::remove(out);
    Settings settings;
    settings.convertWhenUnsupported = convert;
    CaptureOptions options;
    options.file = out;
    options.accepts = [](const DeviceFormat& f) { return f.sampleRate == 44100 && f.type == SampleType::Int24; };
    Played played;
    {
        PlaybackEngine engine(settings, [&](const Settings&) { return createCaptureOutput(options); });
        testutil::StartedSnapshot started(engine);
        engine.openAndPlay(dtsPath());
        engine.waitIdle();
        played.plan = started.get().value_or(engine.snapshot()).plan;
        EXPECT_TRUE(engine.waitForState(PlayState::Stopped, std::chrono::seconds(30)));
        played.snapshot = engine.snapshot();
    }
    std::ifstream in(out, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    played.samples.resize(bytes.size() / 3);
    unpackSamples(reinterpret_cast<const uint8_t*>(bytes.data()), played.samples.size(), 24, SampleType::Int24,
                  played.samples.data());
    return played;
}

} // namespace

TEST(Dts, PlaysOnlyWithConversionAndThenDownmixes) {
    const auto& file = dtsCd();
    const auto refused = playOnInt24(false);
    EXPECT_EQ(refused.snapshot.error, ErrorCode::FormatNotSupported);
    EXPECT_NE(refused.snapshot.errorDetail.find("convert when unsupported"), std::string::npos) << refused.snapshot.errorDetail;
    EXPECT_TRUE(refused.samples.empty());

    const auto played = playOnInt24(true);
    ASSERT_FALSE(played.snapshot.error) << played.snapshot.errorDetail;
    ASSERT_TRUE(played.plan);
    EXPECT_TRUE(played.plan->downmix);
    EXPECT_EQ(played.plan->state, PipelineState::Processed);
    EXPECT_EQ(played.snapshot.format, AudioFormat::Dts);
    const auto& samples = played.samples;
    ASSERT_EQ(samples.size(), file.frames * 2);
    const double a = std::numbers::sqrt2 / 2;
    int32_t worst = 0;
    for (size_t i = 0; i < file.frames; i++) {
        const float* f = &file.reference[i * 6]; // L R C LFE Ls Rs; the LFE is dropped
        const double left = (f[0] + a * f[2] + a * f[4]) * 8388608.0, right = (f[1] + a * f[2] + a * f[5]) * 8388608.0;
        worst = std::max({worst, std::abs(samples[i * 2] - int32_t(std::lround(left))),
                          std::abs(samples[i * 2 + 1] - int32_t(std::lround(right)))});
    }
    EXPECT_LE(worst, 2); // TPDF dither of one LSB and float rounding
}
