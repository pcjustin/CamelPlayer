// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// ALAC, WAV and AIFF through every registered decoder (docs/Plan.md §3.1): lossless, so the generated PCM is the
// independent reference for full decodes and exact seeks.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
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
    {"16-44.1k", 44100, 16, 2, 0.5}, {"24-96k", 96000, 24, 2, 0.25}, {"24-192k", 192000, 24, 2, 0.25}};

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
            testutil::copyFixture("alac-" + s.name + ".m4a", fixture(AudioFormat::Alac, s));
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

namespace {

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// `m4a` with its media timescale, and every time counted in it, doubled: the same audio, its time counted at twice the
// sample rate, as a muxer may write it.
std::string doubledTimescale(std::string m4a) {
    const auto get = [&](size_t at, int bytes) {
        uint64_t value = 0;
        for (int i = 0; i < bytes; i++) value = value << 8 | uint8_t(m4a[at + size_t(i)]);
        return value;
    };
    const auto put = [&](size_t at, int bytes, uint64_t value) {
        for (int i = bytes - 1; i >= 0; i--, value >>= 8) m4a[at + size_t(i)] = char(value & 0xFF);
    };
    const auto box = [&](const char* type) {
        const size_t at = m4a.find(type);
        EXPECT_NE(at, std::string::npos) << type;
        return at - 4;
    };
    const size_t mdhd = box("mdhd");
    const bool mdhdLong = m4a[mdhd + 8] == 1;
    const size_t timescale = mdhd + (mdhdLong ? 28 : 20);
    put(timescale, 4, get(timescale, 4) * 2);
    put(timescale + 4, mdhdLong ? 8 : 4, get(timescale + 4, mdhdLong ? 8 : 4) * 2);
    const size_t stts = box("stts");
    for (uint64_t i = 0, n = get(stts + 12, 4); i < n; i++) {
        const size_t delta = stts + 16 + size_t(i) * 8 + 4;
        put(delta, 4, get(delta, 4) * 2);
    }
    if (const size_t found = m4a.find("elst"); found != std::string::npos) {
        const size_t elst = found - 4;
        const bool elstLong = m4a[elst + 8] == 1;
        const int width = elstLong ? 8 : 4;
        for (uint64_t i = 0, n = get(elst + 12, 4); i < n; i++) {
            const size_t mediaTime = elst + 16 + size_t(i) * (elstLong ? 20 : 12) + size_t(width);
            const uint64_t value = get(mediaTime, width);
            if (value != (width == 8 ? ~uint64_t{0} : 0xFFFFFFFFu)) put(mediaTime, width, value * 2); // -1: an empty edit
        }
    }
    return m4a;
}

} // namespace

// An MP4 whose media timescale is not its sample rate: its length, its decode and every seek are counted in frames at
// the rate the track reports, not in the container's ticks.
TEST(FormatDecoderTimescale, AnMp4CountedAtAnotherRateKeepsItsLengthAndSeeks) {
    const auto& spec = kSpecs[0];
    const auto& expected = reference(spec);
    const uint64_t frames = expected.size() / spec.channels;
    const auto alac = testutil::tempDir() / "format-alac-timescale-x2.m4a";
    std::ofstream(alac, std::ios::binary) << doubledTimescale(readFile(fixture(AudioFormat::Alac, spec)));
    std::vector<int32_t> buffer(size_t(4096) * spec.channels);
    for (const auto kind : decodersFor(AudioFormat::Alac)) {
        auto decoder = openDecoder(kind, alac);
        EXPECT_EQ(decoder->info().sampleRate, spec.sampleRate) << decoderName(kind);
        EXPECT_EQ(decoder->info().totalFrames, frames) << decoderName(kind);
        EXPECT_TRUE(readAll(*decoder) == expected) << decoderName(kind);
        decoder = openDecoder(kind, alac); // the MP4 reader cannot seek once it has read to the end
        for (const uint64_t target : {frames / 2 + 4097, frames / 3, uint64_t(1)}) {
            decoder->seek(target);
            const size_t want = size_t(std::min<uint64_t>(4096, frames - target));
            size_t got = 0;
            while (got < want) {
                const size_t n = decoder->read(buffer.data() + got * spec.channels, want - got);
                ASSERT_GT(n, 0u) << decoderName(kind) << ": short read after seek to " << target;
                got += n;
            }
            EXPECT_TRUE(std::equal(buffer.begin(), buffer.begin() + std::ptrdiff_t(want * spec.channels),
                                   expected.begin() + std::ptrdiff_t(target * spec.channels)))
                << decoderName(kind) << ": seek to " << target;
        }
    }

    // AAC, against the same file counted at its own rate.
    const auto plain = testutil::tempDir() / "format-aac-timescale-x1.m4a";
    const auto doubled = testutil::tempDir() / "format-aac-timescale-x2.m4a";
    testutil::copyFixture("aac.m4a", plain);
    std::ofstream(doubled, std::ios::binary) << doubledTimescale(readFile(plain));
    for (const auto kind : decodersFor(AudioFormat::Aac)) {
        auto counted = openDecoder(kind, plain);
        auto decoder = openDecoder(kind, doubled);
        EXPECT_EQ(decoder->info().sampleRate, counted->info().sampleRate) << decoderName(kind);
        EXPECT_EQ(decoder->info().totalFrames, counted->info().totalFrames) << decoderName(kind);
        EXPECT_TRUE(readAll(*decoder) == readAll(*counted)) << decoderName(kind);
        counted = openDecoder(kind, plain);
        decoder = openDecoder(kind, doubled);
        const uint64_t target = counted->info().totalFrames / 2;
        counted->seek(target);
        decoder->seek(target);
        std::vector<int32_t> want(size_t(1024) * counted->info().channels), got(want.size());
        EXPECT_EQ(decoder->read(got.data(), 1024), counted->read(want.data(), 1024)) << decoderName(kind);
        EXPECT_TRUE(got == want) << decoderName(kind) << ": seek to " << target;
    }
}

namespace {

// 64-bit float PCM, which testaudio does not write.
void writeFloat64Wav(const std::filesystem::path& path, const std::vector<int32_t>& samples24) {
    std::string data;
    for (const int32_t s : samples24) {
        const double v = std::ldexp(double(s), -23);
        data.append(reinterpret_cast<const char*>(&v), sizeof v);
    }
    std::string header = "RIFF";
    const auto le = [&](uint64_t v, int bytes) {
        for (int i = 0; i < bytes; i++) header += char(v >> (8 * i));
    };
    le(36 + data.size(), 4);
    header += "WAVEfmt ";
    // fmt size, IEEE float, channels, rate, byte rate, block align, bits
    const std::pair<uint32_t, int> fields[] = {{16, 4}, {3, 2}, {2, 2}, {44100, 4}, {44100 * 16, 4}, {16, 2}, {64, 2}};
    for (const auto& [value, bytes] : fields) le(value, bytes);
    header += "data";
    le(data.size(), 4);
    std::ofstream(path, std::ios::binary) << header << data;
}

} // namespace

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
    writeFloat64Wav(dir / "format-wav-f64.wav", make(24));
    testaudio::writeAiff(dir / "format-aiff-8.aiff", make(8), 44100, 8, 2);
    testaudio::writeAiff(dir / "format-aiff-32.aiff", make(32), 44100, 32, 2);
    for (const auto decoder : decodersFor(AudioFormat::Wav))
        for (const char* name : {"format-wav-8.wav", "format-wav-32.wav", "format-wav-f32.wav", "format-wav-f64.wav"})
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
    const auto form = uint32_t(bytes.size() - 8);
    for (size_t i = 0; i < 4; i++) bytes[4 + i] = char(form >> (24 - 8 * i));
    std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
    for (const auto decoder : decodersFor(AudioFormat::Aiff)) {
        auto opened = openDecoder(decoder, path);
        EXPECT_EQ(opened->info().totalFrames, expected.size() / spec.channels) << decoderName(decoder);
        EXPECT_TRUE(readAll(*opened) == expected) << decoderName(decoder);
    }
}

// A file that can no longer be read, as on a share that went away, is a decode error, not the end of the track.
TEST(FormatDecoderPolicy, AReadFailureIsNotTheEndOfTheTrack) {
    const testaudio::GenSpec spec{"read-failure", 44100, 16, 2, 2.0};
    const auto signal = testaudio::makeSignal(spec);
    for (const auto format : {AudioFormat::Wav, AudioFormat::Aiff}) {
        SCOPED_TRACE(formatId(format));
        const auto path = testutil::tempDir() / (std::string("format-read-failure") + extension(format));
        if (format == AudioFormat::Wav)
            testaudio::writeWav(path, signal, spec.sampleRate, spec.bits, spec.channels);
        else
            testaudio::writeAiff(path, signal, spec.sampleRate, spec.bits, spec.channels);
        auto decoder = openDecoder(DecoderKind::Sndfile, path);
        std::vector<int32_t> buffer(size_t{4096} * spec.channels);
        ASSERT_EQ(decoder->read(buffer.data(), 4096), 4096U);
        const HANDLE other = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                         nullptr, OPEN_EXISTING, 0, nullptr);
        ASSERT_NE(other, INVALID_HANDLE_VALUE);
        OVERLAPPED from{};
        from.Offset = 128 * 1024;
        ASSERT_TRUE(LockFileEx(other, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, 0, &from));
        try {
            while (decoder->read(buffer.data(), 4096) != 0) {
            }
            ADD_FAILURE() << "the read ended as at the end of the track";
        } catch (const Error& e) {
            EXPECT_EQ(e.code(), ErrorCode::DecodeFailed) << e.what();
        }
        UnlockFileEx(other, 0, MAXDWORD, 0, &from);
        CloseHandle(other);
    }
}

// The Engine then reports the failure rather than a finished track, so the queue does not run on to the next one.
TEST(FormatDecoderPolicy, AReadFailureWhilePlayingIsReportedNotFinished) {
    const testaudio::GenSpec spec{"read-failure-playing", 44100, 16, 2, 30.0};
    const auto path = testutil::tempDir() / "format-read-failure-playing.wav";
    testaudio::writeWav(path, testaudio::makeSignal(spec), spec.sampleRate, spec.bits, spec.channels);
    CaptureOptions options;
    options.file = testutil::tempDir() / "format-read-failure.raw";
    options.realtime = true;
    PlaybackEngine engine(Settings{}, [&](const Settings&) { return createCaptureOutput(options); });
    engine.openAndPlay(path, DecoderKind::Sndfile);
    ASSERT_TRUE(engine.waitForState(PlayState::Playing, std::chrono::seconds(5)));
    const HANDLE other = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                     OPEN_EXISTING, 0, nullptr);
    ASSERT_NE(other, INVALID_HANDLE_VALUE);
    OVERLAPPED from{};
    from.Offset = 1024 * 1024; // about 6 s in, past what the Engine reads ahead at the start
    ASSERT_TRUE(LockFileEx(other, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, 0, &from));
    EXPECT_TRUE(engine.waitForState(PlayState::Stopped, std::chrono::seconds(20)));
    const auto snapshot = engine.snapshot();
    UnlockFileEx(other, 0, MAXDWORD, 0, &from);
    CloseHandle(other);
    EXPECT_EQ(snapshot.error, ErrorCode::DecodeFailed) << snapshot.errorDetail;
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

TEST(FormatDecoderPolicy, LibsndfileAndSymphoniaOpenAFlacWithoutReadingItsCover) {
    const testaudio::GenSpec spec{"cover", 96000, 24, 2, 1.0};
    const auto plain = testutil::tempDir() / "cover-plain.flac", covered = testutil::tempDir() / "cover-3mb.flac";
    testaudio::writeFlac(plain, spec);
    std::ifstream in(plain, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
    in.close();
    ASSERT_EQ(bytes[4] & 0x7f, 0); // STREAMINFO, then more blocks or the frames
    const uint32_t length = 3 << 20;
    std::vector<uint8_t> picture{6, uint8_t((length >> 16) & 0xff), uint8_t((length >> 8) & 0xff), uint8_t(length & 0xff)};
    picture.resize(4 + length, 'p');
    if (bytes[4] & 0x80) {
        bytes[4] &= 0x7f;
        picture[0] |= 0x80;
    }
    bytes.insert(bytes.begin() + 8 + 34, picture.begin(), picture.end());
    std::ofstream(covered, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));

    const auto expected = testaudio::makeSignal(spec);
    for (const auto kind : {DecoderKind::Sndfile, DecoderKind::Symphonia, DecoderKind::DrFlac}) {
        IO_COUNTERS before{}, after{};
        GetProcessIoCounters(GetCurrentProcess(), &before);
        auto decoder = openDecoder(kind, covered);
        GetProcessIoCounters(GetCurrentProcess(), &after);
        EXPECT_LT(after.ReadTransferCount - before.ReadTransferCount, 512u * 1024) << decoderName(kind);
        EXPECT_EQ(decoder->info().totalFrames, expected.size() / spec.channels) << decoderName(kind);
        const auto got = readAll(*decoder);
        EXPECT_TRUE(got == expected) << decoderName(kind);
        const uint64_t target = 30000;
        decoder->seek(target);
        std::vector<int32_t> buffer(size_t(1000) * spec.channels);
        ASSERT_EQ(decoder->read(buffer.data(), 1000), 1000u) << decoderName(kind);
        EXPECT_TRUE(std::equal(buffer.begin(), buffer.end(), expected.begin() + std::ptrdiff_t(target * spec.channels)))
            << decoderName(kind);
    }
}
