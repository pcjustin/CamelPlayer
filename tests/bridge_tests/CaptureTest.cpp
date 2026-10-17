// Capture comparison (docs/Plan.md §12.1, §16.7 item 2): the bytes the engine hands to the "device"
// must unpack to exactly the libFLAC reference decode, for every decoder and container choice.
#include <gtest/gtest.h>

#include <fstream>
#include <tuple>

#include "Engine/PlaybackEngine.h"
#include "Engine/SamplePacker.h"
#include "OutputBridge/CaptureOutput.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::audio;

namespace {

struct Case {
    DecoderKind decoder;
    const char* file;
    std::vector<SampleType> accept; // simulated device capabilities
    SampleType expected;
    bool fullMatrix = false;
};

std::vector<uint8_t> readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in) return {};
    const auto size = in.tellg();
    if (size < 0) return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) return {};
    return bytes;
}

void runCase(const Case& c, uint64_t seekFrame = 0) {
    auto file = c.fullMatrix ? testutil::acceptanceFile(c.file) : testutil::testFile(c.file);
    auto ref = testaudio::referenceDecode(file);
    ASSERT_TRUE(ref.error.empty()) << ref.error;
    if (ref.info.md5) ASSERT_TRUE(ref.md5Ok);
    if (testutil::knownUnsupported(c.decoder, ref.info.sampleRate)) GTEST_SKIP() << "Decoder does not support this format";
    auto out = file.parent_path() / (std::string("capture-") + decoderId(c.decoder) + "-" + c.file + ".raw");
    Settings s;
    s.decoderByFormat[AudioFormat::Flac] = c.decoder;
    CaptureOptions co;
    co.file = out;
    auto accept = c.accept;
    co.accepts = [accept](const DeviceFormat& f) { return std::find(accept.begin(), accept.end(), f.type) != accept.end(); };
    {
        PlaybackEngine engine(s, [&](const Settings&) { return createCaptureOutput(co); });
        engine.openAndPlay(file);
        if (seekFrame) engine.seek(seekFrame);
        engine.waitIdle();
        ASSERT_TRUE(engine.waitForState(PlayState::Stopped, std::chrono::seconds(60)));
        auto snap = engine.snapshot();
        ASSERT_FALSE(snap.error.has_value()) << snap.errorDetail;
    }
    auto bytes = readFile(out);
    ASSERT_EQ(bytes.size() % bytesPerSample(c.expected), 0u);
    const size_t count = bytes.size() / bytesPerSample(c.expected);
    std::vector<int32_t> got(count);
    unpackSamples(bytes.data(), count, ref.info.bitsPerSample, c.expected, got.data());
    if (!seekFrame) {
        EXPECT_EQ(got.size(), ref.samples.size());
        EXPECT_TRUE(got == ref.samples) << decoderName(c.decoder) << " " << c.file << " as " << sampleTypeName(c.expected);
    } else {
        // Seek happened after an unknown amount was captured; the tail from the seek point must match exactly.
        const size_t tail = ref.samples.size() - size_t(seekFrame) * 2;
        ASSERT_GE(got.size(), tail);
        EXPECT_TRUE(std::equal(got.end() - tail, got.end(), ref.samples.begin() + seekFrame * 2));
    }
    std::filesystem::remove(out);
}

} // namespace

class CaptureEveryDecoder : public ::testing::TestWithParam<DecoderKind> {};

TEST_P(CaptureEveryDecoder, NativeContainers) {
    const std::vector<SampleType> all = {SampleType::Int16, SampleType::Int24, SampleType::Int24In32, SampleType::Int32};
    runCase({GetParam(), "16-44.1k", all, SampleType::Int16});
    runCase({GetParam(), "24-96k", all, SampleType::Int24});
}

TEST_P(CaptureEveryDecoder, ZeroPaddedContainers) {
    // DAVE on usbaudio2 only takes 32-bit containers; Cirrus takes 24-in-32 (docs/Plan.md §15.2)
    runCase({GetParam(), "16-44.1k", {SampleType::Int32}, SampleType::Int32});
    runCase({GetParam(), "24-96k", {SampleType::Int24In32, SampleType::Int32}, SampleType::Int24In32});
}

TEST_P(CaptureEveryDecoder, SeekKeepsBitPerfectTail) {
    runCase({GetParam(), "24-96k", {SampleType::Int32}, SampleType::Int32}, 100000);
}

class CaptureAcceptance : public ::testing::TestWithParam<std::tuple<DecoderKind, const char*>> {};

TEST_P(CaptureAcceptance, FullFile) {
    const auto [decoder, file] = GetParam();
    auto info = testaudio::referenceDecode(testutil::acceptanceFile(file)).info;
    const auto native = info.bitsPerSample == 16   ? SampleType::Int16
                        : info.bitsPerSample == 24 ? SampleType::Int24
                                                   : SampleType::Int32;
    runCase({decoder, file, {native}, native, true});
}

TEST_P(CaptureAcceptance, PaddedAndSeek) {
    const auto [decoder, file] = GetParam();
    auto info = testaudio::referenceDecode(testutil::acceptanceFile(file)).info;
    runCase({decoder, file, {SampleType::Int32}, SampleType::Int32, true});
    runCase({decoder, file, {SampleType::Int32}, SampleType::Int32, true}, info.sampleRate * 3ull + 17);
}

INSTANTIATE_TEST_SUITE_P(M0_18, CaptureAcceptance,
                         ::testing::Combine(::testing::ValuesIn(kAllDecoders),
                                            ::testing::Values("16-44.1k", "16-48k", "24-96k", "24-192k", "24-352.8k", "24-384k",
                                                              "24-768k", "no-md5-24-96k")));

INSTANTIATE_TEST_SUITE_P(Flac, CaptureEveryDecoder, ::testing::ValuesIn(kAllDecoders),
                         [](const ::testing::TestParamInfo<DecoderKind>& i) { return std::string(decoderId(i.param)); });

TEST(Capture, UnsupportedFormatIsReportedNotPlayed) {
    Settings s;
    CaptureOptions co;
    co.file = testutil::tempDir() / "never.raw";
    co.accepts = [](const DeviceFormat& f) { return f.type == SampleType::Int16; }; // 16-bit only device
    PlaybackEngine engine(s, [&](const Settings&) { return createCaptureOutput(co); });
    engine.openAndPlay(testutil::testFile("24-96k"));
    engine.waitIdle();
    auto snap = engine.snapshot();
    EXPECT_EQ(snap.state, PlayState::Stopped);
    ASSERT_TRUE(snap.error.has_value());
    EXPECT_EQ(*snap.error, ErrorCode::FormatNotSupported);
}
