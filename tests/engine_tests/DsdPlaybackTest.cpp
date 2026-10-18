// Raw DSD playback (docs/Plan.md §5.3, §5.8): DSF read by DSD Nexus reaches a simulated device as native DSD or DoP,
// bit for bit, with format-correct silence and continuous DoP markers; carrier rates are never assumed.
#include <gtest/gtest.h>

#include <fstream>
#include <functional>
#include <optional>
#include <string>

#include "DsdFixture.h"
#include "Engine/FormatRule.h"
#include "Engine/PlaybackEngine.h"
#include "OutputBridge/CaptureOutput.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::audio;
using namespace dsdfixture;

namespace {

constexpr uint32_t kDsd64Frames = kDsd64 / 8; // byte frames per second

struct Played {
    EngineSnapshot snapshot;
    std::optional<OutputPlan> plan;
    std::string bytes;
};

// 0.3 s of DSD64 decodes completely before the device starts, so the capture has no underrun in the middle.
std::filesystem::path dsfFile(const std::string& name, const Bytes& raw, uint32_t channels = 2, uint32_t rate = kDsd64) {
    const auto path = testutil::tempDir() / ("dsd-play-" + name + ".dsf");
    writeFile(path, dsf(raw, {.channels = channels, .rate = rate}));
    return path;
}

Played play(const std::filesystem::path& file, const Settings& settings, std::function<bool(const DeviceFormat&)> accepts,
            uint64_t startFrame = 0) {
    const auto out = testutil::tempDir() / "dsd-play-capture.raw";
    std::filesystem::remove(out);
    CaptureOptions options;
    options.file = out;
    options.accepts = std::move(accepts);
    Played played;
    {
        PlaybackEngine engine(settings, [&](const Settings&) { return createCaptureOutput(options); });
        engine.load(file);
        if (startFrame) engine.seek(startFrame);
        engine.play();
        engine.waitIdle();
        played.plan = engine.snapshot().plan;
        EXPECT_TRUE(engine.waitForState(PlayState::Stopped, std::chrono::seconds(30)));
        played.snapshot = engine.snapshot();
    }
    std::ifstream in(out, std::ios::binary);
    played.bytes.assign(std::istreambuf_iterator<char>(in), {});
    return played;
}

// DoP frames carry raw byte frames pairwise from `start` (the last pair completed with idle pattern), then idle
// pattern; markers alternate across every captured frame.
void expectDop(const Played& played, const Bytes& raw, size_t start = 0) {
    ASSERT_TRUE(played.plan);
    const size_t bps = bytesPerSample(played.plan->format.type);
    const size_t frames = played.bytes.size() / (2 * bps);
    const size_t total = raw.size() / 2;
    const size_t audio = (total - start + 1) / 2;
    ASSERT_GE(frames, audio);
    const auto byte = [&](size_t frame, size_t channel, size_t offset) {
        return uint8_t(played.bytes[(frame * 2 + channel) * bps + offset]);
    };
    for (size_t f = 0; f < frames; f++)
        for (size_t c = 0; c < 2; c++) {
            const uint8_t marker = byte(f, c, bps - 1);
            ASSERT_TRUE(marker == 0x05 || marker == 0xFA) << "frame " << f;
            if (f) ASSERT_NE(marker, byte(f - 1, c, bps - 1)) << "markers repeat at frame " << f;
            if (bps == 4) ASSERT_EQ(byte(f, c, 0), 0) << "frame " << f;
            const size_t first = start + 2 * f, second = first + 1;
            const uint8_t expectFirst = f < audio ? raw[first * 2 + c] : 0x69;
            const uint8_t expectSecond = f < audio && second < total ? raw[second * 2 + c] : 0x69;
            ASSERT_EQ(byte(f, c, bps - 2), expectFirst) << "frame " << f << " channel " << c;
            ASSERT_EQ(byte(f, c, bps - 3), expectSecond) << "frame " << f << " channel " << c;
        }
}

bool dopDevice(const DeviceFormat& f, SampleType type, uint32_t maxRate = 768000) {
    return f.type == type && f.sampleRate <= maxRate;
}

} // namespace

TEST(DsdPlayback, NegotiatesNativeThenDopAndNeverPcm) {
    const SourceInfo dsd64{kDsd64Frames, 2, 1, 1000};
    const auto native = negotiateDsd(dsd64, true, std::nullopt, [](const DeviceFormat&) { return true; });
    ASSERT_TRUE(native);
    EXPECT_EQ(native->dsd, DsdPath::Native);
    EXPECT_EQ(native->format, (DeviceFormat{kDsd64Frames, 2, SampleType::Dsd8}));
    EXPECT_EQ(native->state, PipelineState::BitPerfect);

    const auto noNative = [](const DeviceFormat& f) { return f.type != SampleType::Dsd8; };
    const auto dop = negotiateDsd(dsd64, true, std::nullopt, noNative);
    ASSERT_TRUE(dop);
    EXPECT_EQ(dop->dsd, DsdPath::Dop);
    EXPECT_EQ(dop->format, (DeviceFormat{176400, 2, SampleType::Int24}));
    EXPECT_EQ(negotiateDsd(dsd64, false, SampleType::Int24In32, noNative)->format.type, SampleType::Int24In32);
    EXPECT_FALSE(negotiateDsd(dsd64, false, SampleType::Int16, noNative));
    EXPECT_FALSE(negotiateDsd(dsd64, true, std::nullopt, [](const DeviceFormat& f) {
        return f.type == SampleType::Int16 || f.type == SampleType::Float32;
    }));

    const auto mono = negotiateDsd({kDsd64Frames, 1, 1, 1000}, false, std::nullopt, noNative);
    ASSERT_TRUE(mono);
    EXPECT_TRUE(mono->duplicate);
    EXPECT_FALSE(negotiateDsd({kDsd64Frames, 6, 1, 1000}, false, std::nullopt, noNative));
    EXPECT_FALSE(negotiateDsd({44100, 2, 16, 1000}, false, std::nullopt, noNative));
}

TEST(DsdPlayback, CarrierRatesComeFromTheDeviceNotAssumptions) {
    const auto upTo768k = [](const DeviceFormat& f) { return dopDevice(f, SampleType::Int24In32); };
    const auto dsd256 = negotiateDsd({4 * kDsd64Frames, 2, 1, 1000}, false, std::nullopt, upTo768k);
    ASSERT_TRUE(dsd256);
    EXPECT_EQ(dsd256->format.sampleRate, 705600u);
    // DSD512 DoP needs a 1411.2 kHz carrier: refused on a 768 kHz device, used only where the device reports it.
    EXPECT_FALSE(negotiateDsd({8 * kDsd64Frames, 2, 1, 1000}, false, std::nullopt, upTo768k));
    const auto dsd512 = negotiateDsd({8 * kDsd64Frames, 2, 1, 1000}, false, std::nullopt,
                                     [](const DeviceFormat& f) { return dopDevice(f, SampleType::Int24In32, 1411200); });
    ASSERT_TRUE(dsd512);
    EXPECT_EQ(dsd512->format.sampleRate, 1411200u);
}

TEST(DsdPlayback, DopCarriesRawDsdWithContinuousMarkersInEveryContainer) {
    const Bytes raw = rawDsd(size_t(kDsd64Frames * 3 / 10) + 1, 2, 21); // odd: the last DoP frame is padded
    const auto file = dsfFile("dop", raw);
    for (const auto type : {SampleType::Int24, SampleType::Int24In32, SampleType::Int32}) {
        const auto played = play(file, Settings{}, [type](const DeviceFormat& f) { return dopDevice(f, type); });
        ASSERT_FALSE(played.snapshot.error) << played.snapshot.errorDetail;
        ASSERT_TRUE(played.plan);
        EXPECT_EQ(played.plan->dsd, DsdPath::Dop);
        EXPECT_EQ(played.plan->format, (DeviceFormat{176400, 2, type}));
        EXPECT_EQ(played.plan->state, PipelineState::BitPerfect);
        EXPECT_EQ(played.snapshot.decoder, DecoderKind::DsdNexus);
        expectDop(played, raw);
    }
}

TEST(DsdPlayback, NativeDsdIsTheRawStreamFollowedByIdlePattern) {
    const Bytes raw = rawDsd(size_t(kDsd64Frames * 3 / 10), 2, 22);
    Settings settings;
    settings.backend = Backend::Asio;
    ASSERT_EQ(settings.asio.dsdOutput, DsdOutput::Native);
    const auto played = play(dsfFile("native", raw), settings, [](const DeviceFormat& f) { return f.type == SampleType::Dsd8; });
    ASSERT_FALSE(played.snapshot.error) << played.snapshot.errorDetail;
    ASSERT_TRUE(played.plan);
    EXPECT_EQ(played.plan->dsd, DsdPath::Native);
    ASSERT_GE(played.bytes.size(), raw.size());
    EXPECT_TRUE(std::equal(raw.begin(), raw.end(), reinterpret_cast<const uint8_t*>(played.bytes.data())));
    EXPECT_TRUE(std::all_of(played.bytes.begin() + ptrdiff_t(raw.size()), played.bytes.end(), [](char b) { return b == 0x69; }));
}

TEST(DsdPlayback, NativeRequestFallsBackToDopNeverToPcm) {
    const Bytes raw = rawDsd(5000, 2, 23);
    Settings settings;
    settings.backend = Backend::Asio;
    const auto played = play(dsfFile("fallback", raw), settings,
                             [](const DeviceFormat& f) { return f.type == SampleType::Int32 && f.sampleRate == 176400; });
    ASSERT_FALSE(played.snapshot.error) << played.snapshot.errorDetail;
    ASSERT_TRUE(played.plan);
    EXPECT_EQ(played.plan->dsd, DsdPath::Dop);
    expectDop(played, raw);

    settings.asio.dsdOutput = DsdOutput::Dop;
    const auto refused =
        play(dsfFile("fallback", raw), settings, [](const DeviceFormat& f) { return f.type == SampleType::Dsd8; });
    EXPECT_EQ(refused.snapshot.error, ErrorCode::FormatNotSupported);
}

TEST(DsdPlayback, SeekStartsAtTheRequestedByteFrame) {
    const Bytes raw = rawDsd(3 * 4096 + 500, 2, 24);
    const auto played =
        play(dsfFile("seek", raw), Settings{}, [](const DeviceFormat& f) { return dopDevice(f, SampleType::Int24In32); }, 4097);
    ASSERT_FALSE(played.snapshot.error) << played.snapshot.errorDetail;
    expectDop(played, raw, 4097);
}

TEST(DsdPlayback, MonoIsDuplicatedAndSurroundIsSkippedWithANotice) {
    const Bytes mono = rawDsd(3000, 1, 25);
    const auto played =
        play(dsfFile("mono", mono, 1), Settings{}, [](const DeviceFormat& f) { return dopDevice(f, SampleType::Int24); });
    ASSERT_FALSE(played.snapshot.error) << played.snapshot.errorDetail;
    Bytes stereo;
    for (const uint8_t b : mono) stereo.insert(stereo.end(), {b, b});
    expectDop(played, stereo);

    const auto surround = play(dsfFile("surround", rawDsd(3000, 6, 26), 6), Settings{},
                               [](const DeviceFormat& f) { return dopDevice(f, SampleType::Int24); });
    EXPECT_EQ(surround.snapshot.error, ErrorCode::FormatNotSupported);
}

TEST(DsdPlayback, Dsd512DopIsRefusedOnA768kDevice) {
    const auto played = play(dsfFile("dsd512", rawDsd(20000, 2, 27), 2, 8 * kDsd64), Settings{},
                             [](const DeviceFormat& f) { return dopDevice(f, SampleType::Int24In32); });
    EXPECT_EQ(played.snapshot.error, ErrorCode::FormatNotSupported);
    EXPECT_NE(played.snapshot.errorDetail.find("DSD512"), std::string::npos) << played.snapshot.errorDetail;
    EXPECT_NE(played.snapshot.errorDetail.find("1411.2 kHz"), std::string::npos) << played.snapshot.errorDetail;
}
