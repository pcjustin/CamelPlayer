// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// DTS-CD: a DTS bitstream posing as 16-bit PCM in a WAVE file. Every WAVE decoder reads it and the DTS stage decodes
// it, but only with "convert when unsupported"; played as PCM it would be loud noise.
#include <gtest/gtest.h>

#include <windows.h>
// windows.h comes first
#include <psapi.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <numbers>
#include <iterator>
#include <optional>
#include <stdexcept>
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
    static const testutil::DtsCd file = testutil::copyDtsCd(testutil::tempDir() / "dts-cd.wav");
    return file;
}

std::filesystem::path dtsPath() {
    dtsCd();
    return testutil::tempDir() / "dts-cd.wav";
}

constexpr float kDecoderTolerance = 1e-5f; // -100 dBFS, between dts_core and FFmpeg's decode

float worstDifference(const float* a, const float* b, size_t count) {
    float worst = 0.0f;
    for (size_t i = 0; i < count; i++) worst = std::max(worst, std::abs(a[i] - b[i]));
    return worst;
}

std::vector<float> readAll(IDecoder& d) {
    std::vector<float> out, block(size_t{4096} * d.info().channels);
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

// The fixture with `leading` word frames of zeros before its bitstream and `silence` after it, and with `resumes` the
// bitstream again after those.
std::filesystem::path withSilence(const char* name, uint64_t leading, uint64_t silence, bool resumes) {
    std::ifstream in(dtsPath(), std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const size_t data = bytes.find("data");
    const auto le32 = [&](size_t at) {
        return uint32_t(uint8_t(bytes[at])) | uint32_t(uint8_t(bytes[at + 1])) << 8 | uint32_t(uint8_t(bytes[at + 2])) << 16 |
               uint32_t(uint8_t(bytes[at + 3])) << 24;
    };
    const auto put32 = [&](size_t at, uint64_t value) {
        for (size_t i = 0; i < 4; i++) bytes[at + i] = char(uint8_t(value >> (8 * i)));
    };
    if (data == std::string::npos || data + 8 + le32(data + 4) != bytes.size())
        throw std::runtime_error("the fixture's data chunk is not its last");
    const std::string stream = bytes.substr(data + 8);
    bytes.insert(data + 8, size_t(leading * 4), '\0');
    bytes += std::string(size_t(silence * 4), '\0');
    if (resumes) bytes += stream;
    put32(data + 4, bytes.size() - data - 8);
    put32(4, bytes.size() - 8);
    const auto path = testutil::tempDir() / name;
    std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
    return path;
}

int64_t privateBytes() {
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
    return int64_t(memory.PrivateUsage);
}

} // namespace

// Silence the data chunk holds where the bitstream stops, at its end or before it goes on, is made as it is read: its
// length is the file's to say, not a measure of what one allocation may take.
TEST(Dts, SilenceWhereTheBitstreamStopsTakesBoundedMemory) {
    const uint64_t silence = uint64_t{44100} * 300;
    for (const bool resumes : {false, true}) {
        SCOPED_TRACE(resumes ? "a gap" : "the end");
        const auto path = withSilence(resumes ? "dts-gap.wav" : "dts-tail.wav", 0, silence, resumes);
        const auto d = openDecoder(DecoderKind::Sndfile, path);
        const uint64_t expected = dtsCd().frames * (resumes ? 2 : 1) + silence;
        ASSERT_EQ(d->info().totalFrames, expected);
        std::vector<float> block(size_t{4096} * 6);
        const int64_t before = privateBytes();
        int64_t most = 0;
        uint64_t frames = 0;
        const uint64_t silentFrom = dtsCd().frames, silentTo = silentFrom + silence;
        float inSilence = 0.0f, after = 0.0f;
        while (const size_t got = d->readFloat(block.data(), 4096)) {
            for (size_t i = 0; i < got; i++) {
                const uint64_t at = frames + i;
                // Well inside the silence, past the last frame's tail, and well inside the resumed bitstream.
                for (size_t c = 0; c < 6; c++)
                    if (at >= silentFrom + 4096 && at < silentTo)
                        inSilence = std::max(inSilence, std::abs(block[i * 6 + c]));
                    else if (at >= silentTo + 8192)
                        after = std::max(after, std::abs(block[i * 6 + c]));
            }
            frames += got;
            most = std::max(most, privateBytes() - before);
        }
        EXPECT_EQ(frames, expected);
        EXPECT_EQ(inSilence, 0.0f);
        if (resumes) EXPECT_GT(after, 0.01f) << "the bitstream plays again after the gap";
        EXPECT_LT(most, int64_t{32} << 20) << "bytes held at most";
    }
}

// A rip can start with digital silence before its first DTS frame, longer than the start that is looked at for it.
TEST(Dts, ARipThatStartsWithSilenceIsStillDts) {
    const auto& file = dtsCd();
    for (const uint64_t leading : {uint64_t{44100}, uint64_t{44100} * 30}) {
        SCOPED_TRACE(leading);
        const auto path = withSilence("dts-leading.wav", leading, 0, false);
        ASSERT_EQ(probeFormat(path), AudioFormat::Dts);
        const auto d = openDecoder(DecoderKind::Sndfile, path);
        ASSERT_EQ(d->info().totalFrames, leading + file.frames);
        const auto decoded = readAll(*d);
        ASSERT_EQ(decoded.size(), (leading + file.frames) * 6);
        EXPECT_EQ(*std::max_element(decoded.begin(), decoded.begin() + ptrdiff_t(leading * 6)), 0.0f);
        EXPECT_LT(worstDifference(decoded.data() + leading * 6, file.reference.data(), file.reference.size()), kDecoderTolerance);
    }
    // PCM after as much silence is still PCM.
    std::vector<int32_t> words(size_t{44100} * 2 * 2, 0);
    for (size_t i = size_t{44100} * 2; i < words.size(); i++)
        words[i] = int32_t(std::lround(8000.0 * std::sin(double(i) * 0.01)));
    const auto pcm = testutil::tempDir() / "dts-leading-pcm.wav";
    testaudio::writeWav(pcm, words, 44100, 16, 2);
    EXPECT_EQ(probeFormat(pcm), AudioFormat::Wav);
}

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
    ASSERT_GT(file.reference.size(), 6u * 20000);
    for (const DecoderKind kind : {DecoderKind::Sndfile, DecoderKind::DrWav, DecoderKind::Symphonia}) {
        SCOPED_TRACE(decoderName(kind));
        const auto d = openDecoder(kind, dtsPath());
        EXPECT_TRUE(d->readsFloat());
        EXPECT_EQ(d->info().sampleRate, 44100u);
        EXPECT_EQ(d->info().channels, 6u);
        EXPECT_EQ(d->info().bitsPerSample, 24u);
        EXPECT_EQ(d->info().totalFrames, file.frames);
        const auto decoded = readAll(*d);
        ASSERT_EQ(decoded.size(), file.frames * 6);
        EXPECT_LT(worstDifference(decoded.data(), file.reference.data(), decoded.size()), kDecoderTolerance);
    }
}

TEST(Dts, SeeksToTheExactSample) {
    const auto& file = dtsCd();
    for (const DecoderKind kind : {DecoderKind::Sndfile, DecoderKind::Symphonia}) {
        SCOPED_TRACE(decoderName(kind));
        const auto d = openDecoder(kind, dtsPath());
        for (const uint64_t target : {uint64_t(0), uint64_t(777), file.frames / 2 + 123, file.frames - 300}) {
            d->seek(target);
            std::vector<float> block(size_t{256} * 6);
            ASSERT_EQ(d->readFloat(block.data(), 256), 256u) << target;
            EXPECT_LT(worstDifference(block.data(), file.reference.data() + target * 6, block.size()), kDecoderTolerance)
                << target;
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
    const auto decoded = readAll(*openDecoder(DecoderKind::Sndfile, dtsPath()));
    const double a = std::numbers::sqrt2 / 2;
    int32_t worst = 0;
    for (size_t i = 0; i < file.frames; i++) {
        const float* f = &decoded[i * 6]; // L R C LFE Ls Rs; the LFE is dropped
        const double left = (f[0] + a * f[2] + a * f[4]) * 8388608.0, right = (f[1] + a * f[2] + a * f[5]) * 8388608.0;
        worst = std::max({worst, std::abs(samples[i * 2] - int32_t(std::lround(left))),
                          std::abs(samples[i * 2 + 1] - int32_t(std::lround(right)))});
    }
    EXPECT_LE(worst, 2); // TPDF dither of one LSB and float rounding
}
