// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// AAC-LC through Symphonia and MP3 through mpg123 (docs/Plan.md §3.1, §12): lossy, so the encoded source PCM is the
// reference with declared tolerances. Files that declare encoder delay and padding must decode to exactly the source
// frames at lag 0; files without trim metadata are only required to decode consistently.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <windows.h>

#include "Engine/decode/FormatProbe.h"
#include "Engine/decode/IDecoder.h"
#include "Engine/PlaybackEngine.h"
#include "Engine/SamplePacker.h"
#include "OutputBridge/CaptureOutput.h"
#include "LameFixture.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::audio;

namespace {

constexpr uint32_t kRate = 44100, kBits = 24, kChannels = 2;
constexpr double kSeconds = 3.0;

// Tolerances (docs/Plan.md §12). Measured: SNR 54.5 dB for AAC-LC at 256 kbit/s and 76 dB for MP3 at 320 kbit/s.
constexpr double kMinSnrDb = 45.0;
constexpr int32_t kSeekAgreement = 0; // a seek against the linear decode of the same decoder: exact

// Two inharmonic, slowly modulated tones: no period short enough to make the lag search ambiguous.
std::vector<int32_t> signal(double amplitude = 1.0) {
    const auto frames = size_t(kSeconds * kRate);
    std::vector<int32_t> out(frames * kChannels);
    const double full = std::ldexp(1.0, int(kBits) - 1) - 1;
    for (size_t i = 0; i < frames; i++)
        for (uint32_t c = 0; c < kChannels; c++) {
            const double t = double(i) / kRate;
            const double phase = 0.4 * c;
            const double v =
                0.35 * std::sin(2 * std::numbers::pi * 440.7 * t + 0.3 * std::sin(2 * std::numbers::pi * 0.7 * t) + phase) +
                0.25 * std::sin(2 * std::numbers::pi * 1987.3 * t + phase) *
                    (0.6 + 0.4 * std::sin(2 * std::numbers::pi * 1.9 * t));
            out[i * kChannels + c] = int32_t(std::lround(amplitude * v * full));
        }
    return out;
}

struct Fixture {
    const char* name;
    AudioFormat format;
    bool trimMetadata;
};
const Fixture kFixtures[] = {{"aac.m4a", AudioFormat::Aac, true},
                             {"aac.aac", AudioFormat::Aac, false},
                             {"mp3-lame.mp3", AudioFormat::Mp3, true},
                             {"mp3-notag.mp3", AudioFormat::Mp3, false}};

std::filesystem::path path(const Fixture& f) {
    return testutil::tempDir() / (std::string("lossy-") + f.name);
}

const std::vector<int32_t>& reference() {
    static std::once_flag once;
    static std::vector<int32_t> pcm;
    std::call_once(once, [] {
        pcm = signal();
        testutil::copyFixture("aac.m4a", path(kFixtures[0]));
        testutil::copyFixture("aac.aac", path(kFixtures[1]));
        testutil::writeMp3(path(kFixtures[2]), pcm, kRate, kBits, kChannels, true);
        testutil::writeMp3(path(kFixtures[3]), pcm, kRate, kBits, kChannels, false);
    });
    return pcm;
}

std::vector<int32_t> decodeAll(IDecoder& decoder) {
    std::vector<int32_t> samples, buffer(size_t(4096) * decoder.info().channels);
    while (const size_t n = decoder.read(buffer.data(), 4096))
        samples.insert(samples.end(), buffer.begin(), buffer.begin() + std::ptrdiff_t(n * decoder.info().channels));
    return samples;
}

// The lag (frames) at which `got` best matches `ref` on channel 0, searched over +-`range`.
int bestLag(const std::vector<int32_t>& ref, const std::vector<int32_t>& got, int range) {
    const size_t frames = std::min(ref.size(), got.size()) / kChannels;
    double best = -1e300;
    int lag = 0;
    for (int l = -range; l <= range; l++) {
        double sum = 0;
        for (size_t i = size_t(range) + kRate / 2; i + size_t(range) < frames; i += 7)
            sum += double(ref[i * kChannels]) * double(got[(i + size_t(int64_t(l))) * kChannels]);
        if (sum > best) {
            best = sum;
            lag = l;
        }
    }
    return lag;
}

double snrDb(const std::vector<int32_t>& ref, const std::vector<int32_t>& got) {
    double signal = 0, noise = 0;
    const size_t edge = size_t(2048) * kChannels;
    for (size_t i = edge; i + edge < std::min(ref.size(), got.size()); i++) {
        signal += double(ref[i]) * ref[i];
        noise += (double(got[i]) - ref[i]) * (double(got[i]) - ref[i]);
    }
    return 10 * std::log10(signal / std::max(noise, 1.0));
}

int32_t maxDifference(const std::vector<int32_t>& a, const std::vector<int32_t>& b) {
    int32_t worst = 0;
    for (size_t i = 0; i < std::min(a.size(), b.size()); i++) worst = std::max(worst, std::abs(a[i] - b[i]));
    return worst;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::filesystem::path writeFile(const std::string& name, const std::string& bytes) {
    const auto path = testutil::tempDir() / ("lossy-" + name);
    std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
    return path;
}

// Two minutes of LAME's VBR of noise that turns loud and quiet every second, so its frames differ in size; the scan of
// so long a file has not covered its end right after it opens.
std::filesystem::path vbrMp3(bool lameTag) {
    static std::once_flag once;
    const auto tagged = testutil::tempDir() / "lossy-vbr.mp3";
    const auto untagged = testutil::tempDir() / "lossy-vbr-notag.mp3";
    std::call_once(once, [&] {
        std::vector<int32_t> pcm(size_t(kRate) * 120 * kChannels);
        uint32_t seed = 1;
        for (size_t i = 0; i < pcm.size(); i++) {
            seed = seed * 1664525u + 1013904223u;
            const int32_t noise = int32_t(seed >> 9) - (1 << 22); // within 24 bits
            pcm[i] = (i / kChannels / kRate) % 2 ? noise / 4 : noise / 64;
        }
        testutil::writeMp3(tagged, pcm, kRate, kBits, kChannels, true, true);
        testutil::writeMp3(untagged, pcm, kRate, kBits, kChannels, false, true);
    });
    return lameTag ? tagged : untagged;
}

std::string be32(uint32_t v) {
    return {char(v >> 24), char(v >> 16), char(v >> 8), char(v)};
}

uint32_t be32At(const std::string& b, size_t at) {
    return (uint32_t(uint8_t(b[at])) << 24) | (uint32_t(uint8_t(b[at + 1])) << 16) | (uint32_t(uint8_t(b[at + 2])) << 8) |
           uint8_t(b[at + 3]);
}

// Changes the size of the box whose type is at `type` by `bytes`.
void grow(std::string& b, size_t type, int64_t bytes) {
    b.replace(type - 4, 4, be32(uint32_t(int64_t(be32At(b, type - 4)) + bytes)));
}

std::string id3(size_t size) {
    return std::string("ID3\4\0\0", 6) + char((size >> 21) & 0x7f) + char((size >> 14) & 0x7f) + char((size >> 7) & 0x7f) +
           char(size & 0x7f) + std::string(size, '\0');
}

// The Xing VBR file behind a tag as large as cover art makes one. Its Xing table counts from the Xing frame.
std::filesystem::path taggedVbrMp3() {
    return writeFile("tagged-vbr.mp3", id3(300000) + readFile(vbrMp3(true)));
}

// Where the movie box of FFmpeg's M4A starts; it follows the media data, so growing it moves no chunk offset.
size_t movieBox(const std::string& m4a) {
    const size_t type = m4a.rfind("moov");
    EXPECT_NE(type, std::string::npos);
    EXPECT_EQ(type - 4 + be32At(m4a, type - 4), m4a.size());
    return type;
}

void expectSameSound(const std::filesystem::path& file, const std::filesystem::path& original) {
    EXPECT_EQ(probeFormat(file), probeFormat(original)) << file.filename();
    for (const auto kind : decodersFor(probeFormat(original)))
        EXPECT_EQ(decodeAll(*openDecoder(kind, file)), decodeAll(*openDecoder(kind, original)))
            << decoderName(kind) << " on " << file.filename();
}

using Case = std::tuple<size_t, DecoderKind>;

std::vector<Case> cases() {
    std::vector<Case> all;
    for (size_t f = 0; f < std::size(kFixtures); f++)
        for (const auto decoder : decodersFor(kFixtures[f].format)) all.emplace_back(f, decoder);
    return all;
}

std::string caseName(const ::testing::TestParamInfo<Case>& info) {
    const auto [index, decoder] = info.param;
    std::string name = kFixtures[index].name;
    std::replace(name.begin(), name.end(), '-', '_');
    std::replace(name.begin(), name.end(), '.', '_');
    return name + "_" + decoderId(decoder);
}

} // namespace

class LossyDecoder : public ::testing::TestWithParam<Case> {};

TEST_P(LossyDecoder, MatchesTheSourceWithinTolerance) {
    const auto [index, kind] = GetParam();
    const auto& fixture = kFixtures[index];
    const auto& source = reference();
    EXPECT_EQ(probeFormat(path(fixture)), fixture.format);
    auto decoder = openDecoder(kind, path(fixture));
    EXPECT_EQ(decoder->info().sampleRate, kRate);
    EXPECT_EQ(decoder->info().channels, kChannels);
    EXPECT_EQ(decoder->info().bitsPerSample, 24u);
    const auto got = decodeAll(*decoder);
    const int lag = bestLag(source, got, 2500);
    // A length is exact or reported as unknown (0); the Engine treats a nonzero length as a promise.
    EXPECT_TRUE(decoder->info().totalFrames == got.size() / kChannels || decoder->info().totalFrames == 0);
    if (!fixture.trimMetadata) return; // disclosed: no sample-exact claim without trim metadata (docs/Plan.md §5.4)
    EXPECT_EQ(got.size(), source.size()) << "delay and padding must be trimmed exactly once";
    EXPECT_EQ(decoder->info().totalFrames, source.size() / kChannels);
    EXPECT_EQ(lag, 0);
    EXPECT_GE(snrDb(source, got), kMinSnrDb);
}

TEST_P(LossyDecoder, SeekMatchesTheLinearDecode) {
    const auto [index, kind] = GetParam();
    const auto& fixture = kFixtures[index];
    reference();
    auto linear = openDecoder(kind, path(fixture));
    const auto all = decodeAll(*linear);
    const uint64_t frames = all.size() / kChannels;
    auto decoder = openDecoder(kind, path(fixture));
    std::vector<int32_t> buffer(size_t(4096) * kChannels);
    int32_t worst = 0;
    for (const uint64_t target : {frames / 2 + 777, uint64_t(1), frames / 3, frames - 5000, uint64_t(0)}) {
        decoder->seek(target);
        const size_t want = size_t(std::min<uint64_t>(4096, frames - target));
        size_t got = 0;
        while (got < want) {
            const size_t n = decoder->read(buffer.data() + got * kChannels, want - got);
            ASSERT_GT(n, 0u) << "short read after seek to " << target;
            got += n;
        }
        const std::vector<int32_t> expected(all.begin() + std::ptrdiff_t(target * kChannels),
                                            all.begin() + std::ptrdiff_t((target + want) * kChannels));
        worst = std::max(worst, maxDifference(expected, std::vector<int32_t>(buffer.begin(),
                                                                             buffer.begin() + std::ptrdiff_t(want * kChannels))));
    }
    EXPECT_LE(worst, kSeekAgreement);
}

INSTANTIATE_TEST_SUITE_P(Lossy, LossyDecoder, ::testing::ValuesIn(cases()), caseName);

// Decoded overs clip to full scale instead of wrapping around.
TEST(LossyDecoderPolicy, OversClipInsteadOfWrapping) {
    const auto loud = signal(1.6);
    std::vector<int32_t> clipped(loud.size());
    const int32_t full = (1 << 23) - 1;
    for (size_t i = 0; i < loud.size(); i++) clipped[i] = std::clamp(loud[i], -full, full);
    const auto aac = testutil::tempDir() / "lossy-loud.m4a";
    const auto mp3 = testutil::tempDir() / "lossy-loud.mp3";
    testutil::copyFixture("aac-loud.m4a", aac);
    testutil::writeMp3(mp3, clipped, kRate, kBits, kChannels, true);
    for (const auto& [file, format] : {std::pair{aac, AudioFormat::Aac}, std::pair{mp3, AudioFormat::Mp3}})
        for (const auto kind : decodersFor(format)) {
            const auto got = decodeAll(*openDecoder(kind, file));
            int32_t jump = 0;
            for (size_t i = kChannels; i < got.size(); i++) jump = std::max(jump, std::abs(got[i] - got[i - kChannels]));
            EXPECT_LT(jump, 1 << 22) << decoderName(kind) << " wraps on " << file.filename();
        }
}

// The Rust Engine path hands each decoder's float output to the device, quantized once (docs/Plan.md §5.2: lossy
// source, no processing), for files with and without trim metadata.
TEST(LossyDecoderPolicy, PlaysThroughTheEngineUnchanged) {
    reference();
    for (const auto& fixture : kFixtures)
        for (const auto kind : decodersFor(fixture.format)) {
            const auto expected = decodeAll(*openDecoder(kind, path(fixture)));
            const auto out = testutil::tempDir() / "lossy-capture.raw";
            Settings settings;
            settings.decoderByFormat[fixture.format] = kind;
            CaptureOptions options;
            options.file = out;
            options.accepts = [](const DeviceFormat& f) { return f.type == SampleType::Int32; };
            {
                PlaybackEngine engine(settings, [&](const Settings&) { return createCaptureOutput(options); });
                engine.openAndPlay(path(fixture));
                engine.waitIdle();
                ASSERT_TRUE(engine.waitForState(PlayState::Stopped, std::chrono::seconds(30)));
                const auto snapshot = engine.snapshot();
                ASSERT_FALSE(snapshot.error) << fixture.name << " " << decoderName(kind) << ": " << snapshot.errorDetail;
                EXPECT_EQ(snapshot.format, fixture.format);
            }
            std::ifstream in(out, std::ios::binary);
            const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            in.close();
            std::vector<int32_t> got(bytes.size() / 4);
            unpackSamples(reinterpret_cast<const uint8_t*>(bytes.data()), got.size(), 24, SampleType::Int32, got.data());
            // The Engine quantizes the float decode once with TPDF dither; the adapter read rounds the same samples.
            ASSERT_EQ(got.size(), expected.size()) << fixture.name << " via " << decoderName(kind);
            EXPECT_LE(maxDifference(got, expected), 1) << fixture.name << " via " << decoderName(kind);
            std::filesystem::remove(out);
        }
}

// Two MP3 files that declare their delay and padding follow each other with no gap and no extra frames.
TEST(LossyDecoderPolicy, TrimmedMp3TracksAreGapless) {
    reference();
    const auto second = testutil::tempDir() / "lossy-second.mp3";
    testutil::writeMp3(second, signal(0.5), kRate, kBits, kChannels, true);
    const auto out = testutil::tempDir() / "lossy-gapless.raw";
    CaptureOptions options;
    options.file = out;
    options.accepts = [](const DeviceFormat& f) { return f.type == SampleType::Int32; };
    options.realtime = true; // the second track is queued while the first plays
    {
        PlaybackEngine engine(Settings{}, [&](const Settings&) { return createCaptureOutput(options); });
        engine.openAndPlay(path(kFixtures[2]));
        engine.waitIdle();
        bool queued = false;
        for (int i = 0; i < 400 && !queued; ++i) {
            queued = engine.queueNext(second);
            if (!queued) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ASSERT_TRUE(queued);
        ASSERT_TRUE(engine.waitForState(PlayState::Stopped, std::chrono::seconds(30)));
        EXPECT_EQ(engine.snapshot().transitions, 1u);
        EXPECT_FALSE(engine.snapshot().error) << engine.snapshot().errorDetail;
    }
    std::ifstream in(out, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    auto expected = decodeAll(*openDecoder(DecoderKind::Mpg123, path(kFixtures[2])));
    const auto tail = decodeAll(*openDecoder(DecoderKind::Mpg123, second));
    expected.insert(expected.end(), tail.begin(), tail.end());
    std::vector<int32_t> got(bytes.size() / 4);
    unpackSamples(reinterpret_cast<const uint8_t*>(bytes.data()), got.size(), 24, SampleType::Int32, got.data());
    EXPECT_EQ(got.size(), size_t(2 * kSeconds * kRate) * kChannels);
    ASSERT_EQ(got.size(), expected.size());
    EXPECT_LE(maxDifference(got, expected), 1); // dithered once by the Engine
    std::filesystem::remove(out);
}

// Taggers leave a second tag, padding or other bytes before the first frame; every decoder finds the same audio, and
// the LAME frame still declares its length.
TEST(LossyDecoderPolicy, Mp3PastStackedTagsPaddingAndOtherDataDecodesTheSame) {
    reference();
    const auto original = path(kFixtures[2]);
    const auto clean = readFile(original);
    ASSERT_EQ(uint8_t(clean[0]), 0xFF) << "the fixture starts with its first frame";
    std::string other(929, '\0');
    for (size_t i = 0; i < other.size(); i++) other[i] = char(0xA1 + i * 5 / 2 % 0x5E);
    for (const auto& [name, bytes] : {std::pair{"stacked.mp3", id3(40).append(id3(30)).append(clean)},
                                      std::pair{"padded.mp3", id3(40).append(1011, '\0').append(clean)},
                                      std::pair{"other.mp3", id3(40).append(other).append(clean)}}) {
        const auto file = writeFile(name, bytes);
        EXPECT_TRUE(lossyLengthDeclared(file)) << name;
        bool estimated = true;
        EXPECT_EQ(probeMp3Info(file, estimated).sampleRate, kRate) << name;
        EXPECT_FALSE(estimated) << name;
        expectSameSound(file, original);
    }
}

// The LAME file followed by the frames of the one without a LAME frame: the LAME frame declares too few.
std::filesystem::path shortDeclaredMp3() {
    reference();
    return writeFile("short-declared.mp3", readFile(path(kFixtures[2])) + readFile(path(kFixtures[3])));
}

// mpg123 opens without scanning the file: the length counts once the scan in the background knows it, and the LAME
// frame's is the estimate until then.
TEST(LossyDecoderPolicy, Mp3LengthIsLearnedWhilePlaying) {
    const auto& source = reference();
    auto decoder = openDecoder(DecoderKind::Mpg123, path(kFixtures[2]));
    EXPECT_EQ(decoder->info().totalFrames, 0u);
    EXPECT_EQ(decoder->estimatedFrames(), source.size() / kChannels);
    const auto got = decodeAll(*decoder);
    EXPECT_EQ(got.size(), source.size());
    EXPECT_EQ(decoder->info().totalFrames, source.size() / kChannels);
}

// A file that holds more than its LAME frame declares plays to its end, and its length is what it holds.
TEST(LossyDecoderPolicy, AnMp3DeclaringTooFewFramesPlaysToItsEnd) {
    const auto file = shortDeclaredMp3();
    const uint64_t appended = decodeAll(*openDecoder(DecoderKind::Mpg123, path(kFixtures[3]))).size() / kChannels;
    auto decoder = openDecoder(DecoderKind::Mpg123, file);
    const uint64_t declared = decoder->estimatedFrames();
    const uint64_t frames = decodeAll(*decoder).size() / kChannels;
    EXPECT_EQ(decoder->info().totalFrames, frames);
    EXPECT_NEAR(double(frames), double(declared + appended), 3 * 1152.0); // delay and padding meet in the middle
}

// Cut short after encoding, the file holds less than its LAME frame declares: its length is what is there.
TEST(LossyDecoderPolicy, AnMp3DeclaringTooManyFramesEndsWhereItsAudioDoes) {
    reference();
    const auto file = testutil::tempDir() / "lossy-truncated.mp3";
    testaudio::writeTruncated(path(kFixtures[2]), file, 0.7);
    auto decoder = openDecoder(DecoderKind::Mpg123, file);
    const uint64_t declared = decoder->estimatedFrames();
    const uint64_t frames = decodeAll(*decoder).size() / kChannels;
    EXPECT_LT(frames, declared);
    EXPECT_EQ(decoder->info().totalFrames, frames);
}

// A seek waits for the scan and lands exactly where a linear decode does.
TEST(LossyDecoderPolicy, AnMp3SeekRightAfterOpeningIsExact) {
    reference();
    const auto all = decodeAll(*openDecoder(DecoderKind::Mpg123, path(kFixtures[2])));
    const uint64_t target = all.size() / kChannels / 2 + 777;
    auto decoder = openDecoder(DecoderKind::Mpg123, path(kFixtures[2]));
    decoder->seek(target);
    std::vector<int32_t> buffer(size_t(4096) * kChannels);
    for (size_t got = 0; got < 4096;) {
        const size_t n = decoder->read(buffer.data() + got * kChannels, 4096 - got);
        ASSERT_GT(n, 0u);
        got += n;
    }
    EXPECT_EQ(buffer, std::vector<int32_t>(all.begin() + std::ptrdiff_t(target * kChannels),
                                           all.begin() + std::ptrdiff_t((target + 4096) * kChannels)));
    decodeAll(*decoder);
    EXPECT_EQ(decoder->info().totalFrames, all.size() / kChannels); // measured by then
}

// The Engine shows the LAME frame's length first and the exact one once the scan knows it; the track plays in full.
TEST(LossyDecoderPolicy, TheEngineLearnsAnMp3sLengthWhileItPlays) {
    const auto file = shortDeclaredMp3();
    const auto expected = decodeAll(*openDecoder(DecoderKind::Mpg123, file));
    const auto out = testutil::tempDir() / "lossy-learned.raw";
    CaptureOptions options;
    options.file = out;
    options.accepts = [](const DeviceFormat& f) { return f.type == SampleType::Int32; };
    options.realtime = true; // the control loop takes the length while the track plays
    {
        PlaybackEngine engine(Settings{}, [&](const Settings&) { return createCaptureOutput(options); });
        engine.openAndPlay(file);
        engine.waitIdle();
        ASSERT_TRUE(engine.waitForState(PlayState::Stopped, std::chrono::seconds(30)));
        const auto snapshot = engine.snapshot();
        ASSERT_FALSE(snapshot.error) << snapshot.errorDetail;
        ASSERT_TRUE(snapshot.source);
        EXPECT_EQ(snapshot.source->totalFrames, expected.size() / kChannels);
        EXPECT_EQ(snapshot.total, expected.size() / kChannels);
    }
    std::ifstream in(out, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    EXPECT_EQ(bytes.size() / 4, expected.size());
    std::filesystem::remove(out);
}

// A VBRI frame (Fraunhofer encoders) holds no sound: mpg123 plays what follows it, and its frame count, which includes
// itself, gives the estimate.
TEST(LossyDecoderPolicy, Mpg123SkipsAVbriFrame) {
    reference();
    const auto plain = readFile(path(kFixtures[3]));
    const auto expected = decodeAll(*openDecoder(DecoderKind::Mpg123, path(kFixtures[3])));
    std::string vbri = plain.substr(0, 4) + std::string(1044 - 4, '\0');
    vbri[2] = char(vbri[2] & ~0x02); // no padding: 1044 bytes at 320 kbit/s and 44.1 kHz
    const uint32_t frames = uint32_t(expected.size() / kChannels / 1152) + 1;
    vbri.replace(36, 18, "VBRI" + std::string("\0\1\0\0\0\x4b", 6) + be32(uint32_t(plain.size())) + be32(frames));
    const auto file = writeFile("vbri.mp3", vbri + plain);
    auto decoder = openDecoder(DecoderKind::Mpg123, file);
    EXPECT_EQ(decoder->estimatedFrames(), expected.size() / kChannels);
    EXPECT_EQ(decodeAll(*decoder), expected);
}

// A last frame cut short, here by an ID3v1 tag written over the end of the file, ends the stream without an error,
// and its missing part plays as silence up to the length the scan counted.
TEST(LossyDecoderPolicy, Mpg123EndsAtALastFrameCutShort) {
    reference();
    const auto plain = readFile(path(kFixtures[3]));
    const auto file = writeFile("cut-last-frame.mp3", plain.substr(0, plain.size() - 300) + "TAG" + std::string(125, '\0'));
    std::vector<int32_t> decoded;
    EXPECT_NO_THROW(decoded = decodeAll(*openDecoder(DecoderKind::Mpg123, file)));
    EXPECT_GT(decoded.size(), size_t(0));
    const auto out = testutil::tempDir() / "lossy-cut-last-frame.raw";
    CaptureOptions options;
    options.file = out;
    options.accepts = [](const DeviceFormat& f) { return f.type == SampleType::Int32; };
    {
        PlaybackEngine engine(Settings{}, [&](const Settings&) { return createCaptureOutput(options); });
        engine.openAndPlay(file);
        engine.waitIdle();
        ASSERT_TRUE(engine.waitForState(PlayState::Stopped, std::chrono::seconds(30)));
        const auto snapshot = engine.snapshot();
        ASSERT_FALSE(snapshot.error) << snapshot.errorDetail;
        EXPECT_EQ(std::filesystem::file_size(out) / 4 / kChannels, snapshot.total);
    }
    std::filesystem::remove(out);
}

// A CBR seek before the scan of a long file has reached it lands at once exactly where a linear decode does.
TEST(LossyDecoderPolicy, Mpg123SeeksACbrMp3ExactlyBeforeItsScanEnds) {
    std::vector<int32_t> pcm;
    for (int i = 0; i < 20; i++) pcm.insert(pcm.end(), reference().begin(), reference().end());
    const auto file = testutil::tempDir() / "lossy-long.mp3";
    testutil::writeMp3(file, pcm, kRate, kBits, kChannels, true);
    const auto all = decodeAll(*openDecoder(DecoderKind::Mpg123, file));
    for (const uint64_t target : {all.size() / kChannels / 2 + 777, all.size() / kChannels - 5000}) {
        auto decoder = openDecoder(DecoderKind::Mpg123, file);
        decoder->seek(target);
        std::vector<int32_t> buffer(size_t(4096) * kChannels);
        for (size_t got = 0; got < 4096;) {
            const size_t n = decoder->read(buffer.data() + got * kChannels, 4096 - got);
            ASSERT_GT(n, 0u);
            got += n;
        }
        EXPECT_EQ(buffer, std::vector<int32_t>(all.begin() + std::ptrdiff_t(target * kChannels),
                                               all.begin() + std::ptrdiff_t((target + 4096) * kChannels)))
            << target;
    }
}

// A VBR seek the scan has not reached lands at once, near the target: by the Xing table, also behind a large tag, or in
// proportion to the length measured before, which also shows as the length at once.
TEST(LossyDecoderPolicy, Mpg123SeeksAVbrMp3AtOnceNearTheTarget) {
    const std::pair<std::filesystem::path, const char*> files[] = {
        {vbrMp3(true), "Xing table"}, {taggedVbrMp3(), "Xing table behind a 300 KB tag"}, {vbrMp3(false), "measured length"}};
    for (const auto& [file, by] : files) {
        const bool measured = file == vbrMp3(false);
        const auto all = decodeAll(*openDecoder(DecoderKind::Mpg123, file));
        const uint64_t total = all.size() / kChannels;
        if (measured) rememberLibraryLength(file, total, false);
        const uint64_t target = total * 9 / 10;
        auto decoder = openDecoder(DecoderKind::Mpg123, file);
        if (measured) EXPECT_EQ(decoder->estimatedFrames(), total);
        decoder->seek(target);
        const auto rest = decodeAll(*decoder);
        // Where the audio after the seek lies in the linear decode, past the frames that warm the decoder up.
        constexpr size_t kSkip = 4096, kWindow = 1024;
        ASSERT_GT(rest.size(), (kSkip + kWindow) * kChannels);
        std::optional<int64_t> lag;
        for (int64_t d = -int64_t(kRate); std::cmp_less_equal(d, kRate) && !lag; d++) {
            const int64_t at = int64_t(target + kSkip) + d;
            if (at < 0 || (uint64_t(at) + kWindow) * kChannels > all.size()) continue;
            if (std::equal(rest.begin() + std::ptrdiff_t(kSkip * kChannels),
                           rest.begin() + std::ptrdiff_t((kSkip + kWindow) * kChannels),
                           all.begin() + std::ptrdiff_t(at * kChannels)))
                lag = d;
        }
        ASSERT_TRUE(lag) << by << ": not within a second of the target";
        std::printf("VBR seek by its %s: %lld frames off\n", by, static_cast<long long>(*lag));
    }
}

// Where the library knows the exact length, a file played through is read once: the scan, a second reading of the
// whole file, waits for a seek. A seek starts it and still lands where the linear decode does.
TEST(LossyDecoderPolicy, Mpg123ReadsAnMp3OnceWhenTheLibraryKnowsItsLength) {
    std::vector<int32_t> pcm;
    for (int i = 0; i < 20; i++) pcm.insert(pcm.end(), reference().begin(), reference().end());
    const auto file = testutil::tempDir() / "lossy-read-once.mp3";
    testutil::writeMp3(file, pcm, kRate, kBits, kChannels, true);
    const auto all = decodeAll(*openDecoder(DecoderKind::Mpg123, file));
    const uint64_t total = all.size() / kChannels;
    rememberLibraryLength(file, total, false);
    const auto bytesRead = [] {
        IO_COUNTERS io{};
        GetProcessIoCounters(GetCurrentProcess(), &io);
        return io.ReadTransferCount;
    };
    const uint64_t before = bytesRead();
    auto decoder = openDecoder(DecoderKind::Mpg123, file);
    EXPECT_EQ(decoder->estimatedFrames(), total);
    EXPECT_EQ(decodeAll(*decoder), all);
    const auto read = double(bytesRead() - before);
    const auto size = double(std::filesystem::file_size(file));
    EXPECT_LT(read, size * 1.5) << read << " bytes read for a file of " << size;

    const uint64_t target = total / 2 + 777;
    decoder = openDecoder(DecoderKind::Mpg123, file);
    decoder->seek(target);
    const auto rest = decodeAll(*decoder);
    ASSERT_EQ(rest.size(), all.size() - target * kChannels);
    EXPECT_TRUE(std::equal(rest.begin(), rest.end(), all.begin() + std::ptrdiff_t(target * kChannels)));
}

// After a seek the scan had not reached, the positions count from the estimate, and the Engine holds the frames read
// to the length a decoder reports: the exact one the scan learns meanwhile waits for a seek that lands exactly.
TEST(LossyDecoderPolicy, Mpg123HoldsItsLengthBackAfterAnEstimatedSeek) {
    const auto file = vbrMp3(true);
    const uint64_t total = decodeAll(*openDecoder(DecoderKind::Mpg123, file)).size() / kChannels;
    const uint64_t target = total * 9 / 10;
    auto decoder = openDecoder(DecoderKind::Mpg123, file);
    decoder->seek(target);
    std::vector<int32_t> buffer(size_t(4096) * kChannels);
    uint64_t position = target + decoder->read(buffer.data(), 4096);
    std::this_thread::sleep_for(std::chrono::seconds(1)); // the scan of the whole file ends meanwhile
    while (const size_t n = decoder->read(buffer.data(), 4096)) {
        position += n;
        const uint64_t length = decoder->info().totalFrames;
        ASSERT_TRUE(!length || position <= length) << position << " frames read past the length " << length;
    }
    EXPECT_TRUE(!decoder->info().totalFrames || decoder->info().totalFrames == position);
    decoder->seek(target);
    EXPECT_EQ(target + decodeAll(*decoder).size() / kChannels, total);
    EXPECT_EQ(decoder->info().totalFrames, total);
}

// The length the library knows, even one it estimated, is the one mpg123 shows at once; the length its scan measures
// still replaces it, so an estimate is not taken as exact.
TEST(LossyDecoderPolicy, Mpg123ShowsTheLibrarysLength) {
    const auto& source = reference();
    const auto file = writeFile("library-length.mp3", readFile(path(kFixtures[3])));
    const uint64_t known = source.size() / kChannels + 5000; // not mpg123's own estimate
    rememberLibraryLength(file, known, true);
    auto decoder = openDecoder(DecoderKind::Mpg123, file);
    EXPECT_EQ(decoder->estimatedFrames(), known);
    const uint64_t frames = decodeAll(*decoder).size() / kChannels;
    EXPECT_EQ(decoder->info().totalFrames, frames);
}

// Without a header an MP3's length is estimated from its size; a tag before the first frame, which cover art makes
// hundreds of kilobytes, is not audio and adds nothing to it.
TEST(LossyDecoderPolicy, AnMp3sEstimatedLengthLeavesOutTheTagBeforeIt) {
    reference();
    const auto plain = path(kFixtures[3]);
    const auto tagged = writeFile("headerless-behind-tag.mp3", id3(300000) + readFile(plain));
    bool estimated = false;
    const uint64_t expected = probeMp3Info(plain, estimated).totalFrames;
    EXPECT_TRUE(estimated);
    EXPECT_EQ(probeMp3Info(tagged, estimated).totalFrames, expected);
    EXPECT_EQ(openDecoder(DecoderKind::Mpg123, tagged)->estimatedFrames(), expected);
}

// The first frame's side info is that of a real file's first frame, which counts more bits than the frame holds; the
// file still plays to its end.
TEST(LossyDecoderPolicy, AnMp3FrameThatDoesNotDecodePlaysToItsEnd) {
    reference();
    auto mp3 = readFile(path(kFixtures[3]));
    const std::string side("\x00\x00\x0f\xff\x00\x69\x00\x00\x00\x01\xf1\x20\x0d\x20\x00\x00\x00\x00\x00\x01\xa4\x00\x00\x00"
                           "\x00\x00\x00\x34\x80\x00\x00\x00",
                           32);
    mp3.replace(4, side.size(), side); // after the header of a frame without CRC
    const auto file = writeFile("undecodable-frame.mp3", mp3);
    const auto expected = decodeAll(*openDecoder(DecoderKind::Mpg123, file));
    const auto out = testutil::tempDir() / "lossy-undecodable.raw";
    CaptureOptions options;
    options.file = out;
    options.accepts = [](const DeviceFormat& f) { return f.type == SampleType::Int32; };
    {
        PlaybackEngine engine(Settings{}, [&](const Settings&) { return createCaptureOutput(options); });
        engine.openAndPlay(file);
        engine.waitIdle();
        ASSERT_TRUE(engine.waitForState(PlayState::Stopped, std::chrono::seconds(30)));
        const auto snapshot = engine.snapshot();
        ASSERT_FALSE(snapshot.error) << snapshot.errorDetail;
        EXPECT_EQ(snapshot.total, expected.size() / kChannels);
    }
    EXPECT_EQ(std::filesystem::file_size(out) / 4, expected.size());
    std::filesystem::remove(out);
}

// Some taggers store the iTunes genre as one byte of text; Symphonia read it as a number and refused the whole file.
TEST(LossyDecoderPolicy, AMalformedMp4TagStopsNoDecoder) {
    reference();
    const auto original = path(kFixtures[0]);
    auto m4a = readFile(original);
    const size_t moov = movieBox(m4a);
    const size_t udta = m4a.find("udta", moov), meta = m4a.find("meta", udta), ilst = m4a.find("ilst", meta);
    ASSERT_NE(ilst, std::string::npos);
    const std::string data = "data" + be32(1) + be32(0) + " ";
    const std::string genre = be32(uint32_t(8 + 4 + data.size())) + "gnre" + be32(uint32_t(4 + data.size())) + data;
    m4a.insert(ilst - 4 + be32At(m4a, ilst - 4), genre);
    for (const size_t box : {moov, udta, meta, ilst}) grow(m4a, box, int64_t(genre.size()));
    const auto file = writeFile("genre.m4a", m4a);
    expectSameSound(file, original);
    EXPECT_EQ(probeAacInfo(file), probeAacInfo(original));
}

// A music video's picture track comes first; its sound plays.
TEST(LossyDecoderPolicy, AMusicVideoPlaysItsSound) {
    reference();
    const auto original = path(kFixtures[0]);
    auto mp4 = readFile(original);
    const size_t moov = movieBox(mp4);
    const size_t trak = mp4.find("trak", moov);
    ASSERT_NE(trak, std::string::npos);
    // A copy of the sound track as a picture track: its own track ID, a video handler and a visual sample entry of a
    // codec no decoder knows.
    std::string video = mp4.substr(trak - 4, be32At(mp4, trak - 4));
    const size_t tkhd = video.find("tkhd");
    ASSERT_EQ(video[tkhd + 4], '\0') << "a version 0 track header";
    video.replace(tkhd + 16, 4, be32(2)); // after the version, flags and two times
    video.replace(video.find("soun"), 4, "vide");
    const std::string visual = std::string(16, '\0') + std::string("\0\x10\0\x10", 4) + be32(0x480000) + be32(0x480000) +
                               be32(0) + std::string("\0\1", 2) + std::string(32, '\0') + std::string("\0\x18\xFF\xFF", 4);
    const std::string entry =
        be32(uint32_t(16 + visual.size())) + "zzzz" + std::string(6, '\0') + std::string("\0\1", 2) + visual;
    const std::string stsd = be32(uint32_t(16 + entry.size())) + "stsd" + be32(0) + be32(1) + entry;
    const size_t oldStsd = video.find("stsd") - 4;
    const int64_t change = int64_t(stsd.size()) - int64_t(be32At(video, oldStsd));
    video.replace(oldStsd, be32At(video, oldStsd), stsd);
    for (const char* box : {"trak", "mdia", "minf", "stbl"}) grow(video, video.find(box), change);
    mp4.insert(trak - 4, video);
    grow(mp4, moov, int64_t(video.size()));
    const auto file = writeFile("video.mp4", mp4);
    expectSameSound(file, original);
    EXPECT_EQ(probeAacInfo(file), probeAacInfo(original));
}
