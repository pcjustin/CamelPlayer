// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// ALAC, WAV and AIFF through every registered decoder (docs/Plan.md §3.1): lossless, so the generated PCM is the
// independent reference for full decodes and exact seeks.
#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>

#include "Engine/decode/FormatProbe.h"
#include "Engine/decode/IDecoder.h"
#include "Engine/PlaybackEngine.h"
#include "Engine/SamplePacker.h"
#include "OutputBridge/CaptureOutput.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::audio;

namespace {

const testaudio::GenSpec kSpecs[] = {
    {"16-44.1k", 44100, 16, 2, 2.0}, {"24-96k", 96000, 24, 2, 2.0}, {"24-192k", 192000, 24, 2, 1.0}};

const char* extension(AudioFormat format) {
    return format == AudioFormat::Alac ? ".m4a" : format == AudioFormat::Wav ? ".wav" : ".aiff";
}

std::filesystem::path fixture(AudioFormat format, const testaudio::GenSpec& spec) {
    return testutil::tempDir() / (std::string("format-") + formatId(format) + "-" + spec.name + extension(format));
}

// Every fixture is written once per run from the same deterministic signal.
const std::vector<int32_t>& reference(const testaudio::GenSpec& spec) {
    static std::once_flag once;
    static std::vector<std::vector<int32_t>> signals;
    std::call_once(once, [] {
        for (const auto& s : kSpecs) {
            signals.push_back(testaudio::makeSignal(s));
            testaudio::writeWav(fixture(AudioFormat::Wav, s), signals.back(), s.sampleRate, s.bits, s.channels);
            testaudio::writeAiff(fixture(AudioFormat::Aiff, s), signals.back(), s.sampleRate, s.bits, s.channels);
            testaudio::writeAlac(fixture(AudioFormat::Alac, s), signals.back(), s.sampleRate, s.bits, s.channels);
        }
    });
    return signals[size_t(&spec - kSpecs)];
}

std::vector<int32_t> readAll(IDecoder& decoder) {
    std::vector<int32_t> samples, buffer(size_t(4096) * decoder.info().channels);
    while (const size_t n = decoder.read(buffer.data(), 4096))
        samples.insert(samples.end(), buffer.begin(), buffer.begin() + std::ptrdiff_t(n * decoder.info().channels));
    return samples;
}

void expectRejected(DecoderKind decoder, const std::filesystem::path& path) {
    try {
        openDecoder(decoder, path);
        ADD_FAILURE() << decoderName(decoder) << " accepted " << path.filename();
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::UnsupportedStream) << decoderName(decoder) << " " << path.filename() << ": " << e.what();
    }
}

using Case = std::tuple<AudioFormat, DecoderKind, size_t>;

std::vector<Case> cases() {
    std::vector<Case> all;
    for (const auto format : {AudioFormat::Alac, AudioFormat::Wav, AudioFormat::Aiff})
        for (const auto decoder : decodersFor(format))
            for (size_t spec = 0; spec < std::size(kSpecs); spec++) all.emplace_back(format, decoder, spec);
    return all;
}

} // namespace

class FormatDecoder : public ::testing::TestWithParam<Case> {};

TEST_P(FormatDecoder, DecodesExactlyTheSourcePcm) {
    const auto [format, kind, index] = GetParam();
    const auto& spec = kSpecs[index];
    const auto& expected = reference(spec);
    auto decoder = openDecoder(kind, fixture(format, spec));
    const auto& info = decoder->info();
    EXPECT_EQ(info.sampleRate, spec.sampleRate);
    EXPECT_EQ(info.channels, spec.channels);
    EXPECT_EQ(info.bitsPerSample, spec.bits);
    EXPECT_EQ(info.totalFrames, expected.size() / spec.channels);
    EXPECT_FALSE(info.md5);
    const auto got = readAll(*decoder);
    ASSERT_EQ(got.size(), expected.size());
    EXPECT_TRUE(got == expected);
    EXPECT_EQ(decoder->read(std::vector<int32_t>(spec.channels).data(), 1), 0u);
}

TEST_P(FormatDecoder, SeeksToTheExactFrame) {
    const auto [format, kind, index] = GetParam();
    const auto& spec = kSpecs[index];
    const auto& expected = reference(spec);
    const uint64_t frames = expected.size() / spec.channels;
    auto decoder = openDecoder(kind, fixture(format, spec));
    std::vector<int32_t> buffer(size_t(4096) * spec.channels);
    for (const uint64_t target : {frames / 2 + 4097, uint64_t(1), frames / 3, frames - 1, uint64_t(0), frames - 5000}) {
        decoder->seek(target);
        const size_t want = size_t(std::min<uint64_t>(4096, frames - target));
        size_t got = 0;
        while (got < want) {
            const size_t n = decoder->read(buffer.data() + got * spec.channels, want - got);
            ASSERT_GT(n, 0u) << "short read after seek to " << target;
            got += n;
        }
        const auto begin = expected.begin() + std::ptrdiff_t(target * spec.channels);
        EXPECT_TRUE(std::equal(buffer.begin(), buffer.begin() + std::ptrdiff_t(want * spec.channels), begin))
            << "seek to " << target;
    }
}

namespace {
std::string caseName(const ::testing::TestParamInfo<Case>& info) {
    const auto [format, decoder, index] = info.param;
    std::string spec = kSpecs[index].name;
    std::replace(spec.begin(), spec.end(), '-', '_');
    std::replace(spec.begin(), spec.end(), '.', '_');
    return std::string(formatId(format)) + "_" + decoderId(decoder) + "_" + spec;
}
} // namespace

INSTANTIATE_TEST_SUITE_P(Lossless, FormatDecoder, ::testing::ValuesIn(cases()), caseName);

TEST(FormatDecoderPolicy, RejectsOtherDepthsAndFloat) {
    const auto dir = testutil::tempDir();
    const testaudio::GenSpec spec{"depth", 44100, 16, 2, 0.2};
    const auto make = [&](uint32_t bits) {
        auto s = spec;
        s.bits = bits;
        return testaudio::makeSignal(s);
    };
    testaudio::writeWav(dir / "format-wav-8.wav", make(8), 44100, 8, 2);
    testaudio::writeWav(dir / "format-wav-32.wav", make(32), 44100, 32, 2);
    testaudio::writeWav(dir / "format-wav-f32.wav", make(24), 44100, 24, 2, true);
    testaudio::writeAiff(dir / "format-aiff-8.aiff", make(8), 44100, 8, 2);
    testaudio::writeAiff(dir / "format-aiff-32.aiff", make(32), 44100, 32, 2);
    for (const auto decoder : decodersFor(AudioFormat::Wav))
        for (const char* name : {"format-wav-8.wav", "format-wav-32.wav", "format-wav-f32.wav"})
            expectRejected(decoder, dir / name);
    for (const auto decoder : decodersFor(AudioFormat::Aiff))
        for (const char* name : {"format-aiff-8.aiff", "format-aiff-32.aiff"}) expectRejected(decoder, dir / name);
}

// The Rust Engine path with each format's decoders: the captured device bytes unpack to the source PCM.
TEST(FormatDecoderPolicy, PlaysBitExactThroughTheEngine) {
    const auto& spec = kSpecs[1];
    const auto& expected = reference(spec);
    for (const auto format : {AudioFormat::Alac, AudioFormat::Wav, AudioFormat::Aiff})
        for (const auto decoder : decodersFor(format)) {
            const auto out = testutil::tempDir() / "format-capture.raw";
            Settings settings;
            settings.decoderByFormat[format] = decoder;
            CaptureOptions options;
            options.file = out;
            options.accepts = [](const DeviceFormat& f) { return f.type == SampleType::Int32; };
            {
                PlaybackEngine engine(settings, [&](const Settings&) { return createCaptureOutput(options); });
                engine.openAndPlay(fixture(format, spec));
                engine.waitIdle();
                ASSERT_TRUE(engine.waitForState(PlayState::Stopped, std::chrono::seconds(30)));
                const auto snapshot = engine.snapshot();
                ASSERT_FALSE(snapshot.error) << decoderName(decoder) << ": " << snapshot.errorDetail;
                EXPECT_EQ(snapshot.decoder, decoder);
            }
            std::ifstream in(out, std::ios::binary);
            const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            in.close();
            std::vector<int32_t> got(bytes.size() / 4);
            unpackSamples(reinterpret_cast<const uint8_t*>(bytes.data()), got.size(), spec.bits, SampleType::Int32, got.data());
            EXPECT_TRUE(got == expected) << formatName(format) << " via " << decoderName(decoder);
            std::filesystem::remove(out);
        }
}

TEST(FormatDecoderPolicy, AiffChunkAfterSoundDataIsNotAudio) {
    const auto& spec = kSpecs[0];
    const auto& expected = reference(spec);
    const auto path = testutil::tempDir() / "format-aiff-trailing.aiff";
    testaudio::writeAiff(path, expected, spec.sampleRate, spec.bits, spec.channels);
    std::string bytes;
    {
        std::ifstream in(path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), {});
    }
    bytes += std::string("ANNO\0\0\0\x08", 8) + "trailing";
    const uint32_t form = uint32_t(bytes.size() - 8);
    for (size_t i = 0; i < 4; i++) bytes[4 + i] = char(form >> (24 - 8 * i));
    std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
    for (const auto decoder : decodersFor(AudioFormat::Aiff)) {
        auto opened = openDecoder(decoder, path);
        EXPECT_EQ(opened->info().totalFrames, expected.size() / spec.channels) << decoderName(decoder);
        EXPECT_TRUE(readAll(*opened) == expected) << decoderName(decoder);
    }
}

TEST(FormatDecoderPolicy, DecoderMustSupportTheFileFormat) {
    const auto& spec = kSpecs[0];
    reference(spec);
    expectRejected(DecoderKind::DrFlac, fixture(AudioFormat::Wav, spec));
    expectRejected(DecoderKind::DrFlac, fixture(AudioFormat::Alac, spec));
    expectRejected(DecoderKind::Sndfile, fixture(AudioFormat::Alac, spec));
    expectRejected(DecoderKind::DrWav, fixture(AudioFormat::Alac, spec));
    expectRejected(DecoderKind::DrWav, testutil::testFile("16-44.1k"));
}

// Local real-world samples (testdata/, not in Git): every registered decoder agrees on every sample.
TEST(FormatDecoderPolicy, RealSamplesAgreeAcrossDecoders) {
    std::filesystem::path testdata;
    for (auto d = std::filesystem::current_path(); !d.empty() && d != d.root_path(); d = d.parent_path())
        if (std::filesystem::exists(d / "testdata")) {
            testdata = d / "testdata";
            break;
        }
    if (testdata.empty()) GTEST_SKIP() << "testdata/ not present";
    for (const char* name : {"sample-6s.wav", "sample-2.alac", "01 - Dont Know Why.m4a"}) {
        const auto path = testdata / name;
        if (!std::filesystem::exists(path)) continue;
        std::optional<std::vector<int32_t>> first;
        for (const auto decoder : decodersFor(probeFormat(path))) {
            auto opened = openDecoder(decoder, path);
            const auto samples = readAll(*opened);
            EXPECT_EQ(samples.size(), opened->info().totalFrames * opened->info().channels)
                << decoderName(decoder) << " " << name;
            if (!first)
                first = samples;
            else
                EXPECT_TRUE(samples == *first) << decoderName(decoder) << " differs on " << name;
        }
    }
}
