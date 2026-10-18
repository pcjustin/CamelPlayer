// AAC-LC and MP3 through every registered decoder (docs/Plan.md §3.1, §12): lossy, so the encoded source PCM is the
// reference with declared tolerances. Files that declare encoder delay and padding must decode to exactly the source
// frames at lag 0; files without trim metadata are only required to decode consistently.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>
#include <thread>
#include <tuple>

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

// Tolerances (docs/Plan.md §12). Measured: SNR 54.5 dB for AAC-LC at 256 kbit/s and 76 dB for MP3 at 320 kbit/s; the
// largest difference between two decoders of one file was 361 LSB (dr_mp3 against FFmpeg, about -87 dBFS).
constexpr double kMinSnrDb = 45.0;
constexpr int32_t kDecoderAgreement = 1 << 10; // about -78 dBFS between two decoders of the same file
constexpr int32_t kSeekAgreement = 0;          // a seek against the linear decode of the same decoder: exact

// Two inharmonic, slowly modulated tones: no period short enough to make the lag search ambiguous.
std::vector<int32_t> signal(double amplitude = 1.0) {
    const size_t frames = size_t(kSeconds * kRate);
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
        testaudio::writeAac(path(kFixtures[0]), pcm, kRate, kBits, kChannels, false);
        testaudio::writeAac(path(kFixtures[1]), pcm, kRate, kBits, kChannels, true);
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

TEST_P(LossyDecoder, SixteenBitOptionQuantizesTheSameDecode) {
    const auto [index, kind] = GetParam();
    const auto& fixture = kFixtures[index];
    reference();
    PcmBitsByFormat sixteen;
    sixteen[fixture.format] = 16;
    auto narrow = openDecoder(kind, path(fixture), sixteen);
    EXPECT_EQ(narrow->info().bitsPerSample, 16u);
    const auto got16 = decodeAll(*narrow);
    const auto got24 = decodeAll(*openDecoder(kind, path(fixture)));
    ASSERT_EQ(got16.size(), got24.size());
    for (size_t i = 0; i < got16.size(); i++) {
        ASSERT_GE(got16[i], -32768);
        ASSERT_LE(got16[i], 32767);
        ASSERT_LE(std::abs(got16[i] * 256 - got24[i]), 256) << "sample " << i;
    }
}

INSTANTIATE_TEST_SUITE_P(Lossy, LossyDecoder, ::testing::ValuesIn(cases()), caseName);

TEST(LossyDecoderPolicy, DecodersOfAFileAgree) {
    reference();
    for (const auto& fixture : kFixtures) {
        std::optional<std::vector<int32_t>> first;
        for (const auto kind : decodersFor(fixture.format)) {
            const auto got = decodeAll(*openDecoder(kind, path(fixture)));
            if (!first) {
                first = got;
                continue;
            }
            const int32_t difference = maxDifference(*first, got);
            EXPECT_EQ(got.size(), first->size()) << decoderName(kind) << " on " << fixture.name;
            EXPECT_LE(difference, kDecoderAgreement) << decoderName(kind) << " on " << fixture.name;
        }
    }
}

// Decoded overs clip to full scale instead of wrapping around.
TEST(LossyDecoderPolicy, OversClipInsteadOfWrapping) {
    const auto loud = signal(1.6);
    std::vector<int32_t> clipped(loud.size());
    const int32_t full = (1 << 23) - 1;
    for (size_t i = 0; i < loud.size(); i++) clipped[i] = std::clamp(loud[i], -full, full);
    const auto aac = testutil::tempDir() / "lossy-loud.m4a";
    const auto mp3 = testutil::tempDir() / "lossy-loud.mp3";
    testaudio::writeAac(aac, clipped, kRate, kBits, kChannels, false);
    testutil::writeMp3(mp3, clipped, kRate, kBits, kChannels, true);
    for (const auto& [file, format] : {std::pair{aac, AudioFormat::Aac}, std::pair{mp3, AudioFormat::Mp3}})
        for (const auto kind : decodersFor(format)) {
            const auto got = decodeAll(*openDecoder(kind, file));
            int32_t jump = 0;
            for (size_t i = kChannels; i < got.size(); i++) jump = std::max(jump, std::abs(got[i] - got[i - kChannels]));
            EXPECT_LT(jump, 1 << 22) << decoderName(kind) << " wraps on " << file.filename();
        }
}

// The Rust Engine path hands each decoder's output to the device unchanged (docs/Plan.md §5.2: lossy source, no
// processing), for files with and without trim metadata.
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
            EXPECT_TRUE(got == expected) << fixture.name << " via " << decoderName(kind);
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
    EXPECT_TRUE(got == expected);
    std::filesystem::remove(out);
}

// Local real-world samples (testdata/, not in Git): every registered decoder decodes the same length, within tolerance.
TEST(LossyDecoderPolicy, RealSamplesAgreeAcrossDecoders) {
    std::filesystem::path testdata;
    for (auto d = std::filesystem::current_path(); !d.empty() && d != d.root_path(); d = d.parent_path())
        if (std::filesystem::exists(d / "testdata")) {
            testdata = d / "testdata";
            break;
        }
    if (testdata.empty()) GTEST_SKIP() << "testdata/ not present";
    for (const char* name : {"sample-6s.mp3", "Sample_BeeMoved_48kHz16bit.m4a"}) {
        const auto file = testdata / name;
        if (!std::filesystem::exists(file)) continue;
        std::optional<std::vector<int32_t>> first;
        for (const auto kind : decodersFor(probeFormat(file))) {
            const auto got = decodeAll(*openDecoder(kind, file));
            if (!first) {
                first = got;
                continue;
            }
            EXPECT_EQ(got.size(), first->size()) << decoderName(kind) << " on " << name;
            EXPECT_LE(maxDifference(*first, got), kDecoderAgreement) << decoderName(kind) << " on " << name;
        }
    }
}
