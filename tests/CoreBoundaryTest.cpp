#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <limits>
#include <numeric>

#include "audio/decode/FlacStreamInfo.h"
#include "audio/engine/FormatRule.h"
#include "audio/engine/SamplePacker.h"
#include "audio/engine/SpscRing.h"
#include "audio/output/WaveFormat.h"
#include "audio/verify/FlacMd5.h"
#include "settings/Settings.h"
#include "util/Md5.h"
#include "util/Text.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::audio;

namespace {

std::filesystem::path caseFile(const char* suffix) {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    return testutil::tempDir() / (std::string(info->test_suite_name()) + "-" + info->name() + suffix);
}

void writeBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    ASSERT_TRUE(out.good());
}

std::vector<uint8_t> streamInfo() {
    std::ifstream in(testutil::testFile("24-96k"), std::ios::binary);
    std::vector<uint8_t> bytes(42);
    in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    return bytes;
}

TEST(AudioTypes, DescriptionsPreserveRateAndContainer) {
    EXPECT_EQ(describe({96000, 2, SampleType::Int24In32}), "96 kHz / 24-in-32");
    EXPECT_EQ(describe({44100, 2, SampleType::Int16}), "44.1 kHz / 16");
    EXPECT_EQ(describe({48000, 2, SampleType::Float32}), "48 kHz / 32 float");
    EXPECT_DOUBLE_EQ(framesToSeconds(44100, 44100), 1.0);
    EXPECT_DOUBLE_EQ(framesToSeconds(123, 0), 0.0);
    EXPECT_EQ(secondsToFrames(-1, 48000), 0u);
    EXPECT_EQ(secondsToFrames(0.5, 44100), 22050u);
    EXPECT_EQ(secondsToFrames(0.51 / 48000, 48000), 1u);
}

TEST(AudioTypes, ErrorRetainsCategoryAndDetail) {
    const Error error(ErrorCode::DeviceLost, "device unplugged");
    EXPECT_EQ(error.code(), ErrorCode::DeviceLost);
    EXPECT_STREQ(error.what(), "device unplugged");
    for (auto code : {ErrorCode::FileOpen, ErrorCode::UnsupportedStream, ErrorCode::DecodeFailed, ErrorCode::SeekFailed,
                      ErrorCode::DeviceNotFound, ErrorCode::DeviceInUse, ErrorCode::DeviceLost, ErrorCode::FormatNotSupported,
                      ErrorCode::DriverError, ErrorCode::Other})
        EXPECT_STRNE(errorCodeName(code), "?");
}

TEST(AudioTypes, InvalidTimeValuesDoNotOverflowFrameCount) {
    EXPECT_EQ(secondsToFrames(std::numeric_limits<double>::quiet_NaN(), 48000), 0u);
    EXPECT_EQ(secondsToFrames(std::numeric_limits<double>::infinity(), 48000), 0u);
    EXPECT_EQ(secondsToFrames(1, 0), 0u);
    EXPECT_EQ(secondsToFrames(std::numeric_limits<double>::max(), 48000), std::numeric_limits<uint64_t>::max());
}

TEST(SamplePackerBoundary, RejectsInvalidDepthsAndFloatingPointCandidates) {
    for (auto type : {SampleType::Int16, SampleType::Int24, SampleType::Int24In32, SampleType::Int32, SampleType::Float32}) {
        EXPECT_FALSE(canCarryBitPerfect(type, 0));
        EXPECT_FALSE(canCarryBitPerfect(type, 33));
        EXPECT_FALSE(canCarryBitPerfect(type, validBits(type) + 1));
    }
    EXPECT_TRUE(bitPerfectCandidates(0).empty());
    EXPECT_TRUE(bitPerfectCandidates(33).empty());
    EXPECT_EQ(bitPerfectCandidates(24), (std::vector{SampleType::Int24, SampleType::Int24In32, SampleType::Int32}));
}

TEST(SamplePackerBoundary, KnownLittleEndianBytesAndSignedExtremes) {
    const std::array<int32_t, 5> pcm{0, 1, -1, -32768, 32767};
    const std::array<uint8_t, 10> expected{0, 0, 1, 0, 255, 255, 0, 128, 255, 127};
    std::array<uint8_t, 12> bytes;
    bytes.fill(0xA5);
    packSamples(pcm.data(), pcm.size(), 16, SampleType::Int16, bytes.data() + 1);
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(), bytes.begin() + 1));
    EXPECT_EQ(bytes.front(), 0xA5);
    EXPECT_EQ(bytes.back(), 0xA5);
    std::array<int32_t, 5> decoded{};
    unpackSamples(expected.data(), decoded.size(), 16, SampleType::Int16, decoded.data());
    EXPECT_EQ(decoded, pcm);
    const std::array<uint8_t, 15> expected24{0, 0, 0, 0, 1, 0, 0, 255, 255, 0, 0, 128, 0, 255, 127};
    std::array<uint8_t, 15> packed24{};
    packSamples(pcm.data(), pcm.size(), 16, SampleType::Int24, packed24.data());
    EXPECT_EQ(packed24, expected24);
}

TEST(SamplePackerBoundary, ZeroCountDoesNotAccessNullPointers) {
    for (auto type : {SampleType::Int16, SampleType::Int24, SampleType::Int24In32, SampleType::Int32}) {
        EXPECT_NO_THROW(packSamples(nullptr, 0, 16, type, nullptr));
        EXPECT_NO_THROW(unpackSamples(nullptr, 0, 16, type, nullptr));
    }
}

TEST(FormatRuleBoundary, InvalidSourceNeverProbesDevice) {
    int probes = 0;
    auto supports = [&](const DeviceFormat&) {
        ++probes;
        return true;
    };
    for (const auto& source : {SourceInfo{0, 2, 16}, SourceInfo{48000, 0, 16}, SourceInfo{48000, 1, 16}, SourceInfo{48000, 6, 24},
                               SourceInfo{48000, 2, 0}, SourceInfo{48000, 2, 33}})
        EXPECT_FALSE(chooseOutputFormat(source, std::nullopt, supports));
    EXPECT_EQ(probes, 0);
}

TEST(FormatRuleBoundary, PreservesRateAndStopsAtFirstSupportedContainer) {
    std::vector<SampleType> probes;
    auto format = chooseOutputFormat({88200, 2, 16}, std::nullopt, [&](const DeviceFormat& candidate) {
        EXPECT_EQ(candidate.sampleRate, 88200u);
        EXPECT_EQ(candidate.channels, 2u);
        probes.push_back(candidate.type);
        return candidate.type == SampleType::Int24;
    });
    ASSERT_TRUE(format);
    EXPECT_EQ(format->type, SampleType::Int24);
    EXPECT_EQ(probes, (std::vector{SampleType::Int16, SampleType::Int24}));
    EXPECT_FALSE(chooseOutputFormat({48000, 2, 16}, SampleType::Float32, [](const auto&) { return true; }));
}

TEST(SpscRingBoundary, EmptyAndZeroCapacityAreSafe) {
    SpscRing ring;
    EXPECT_EQ(ring.capacity(), 0u);
    EXPECT_EQ(ring.readable(), 0u);
    EXPECT_EQ(ring.writable(), 0u);
    EXPECT_EQ(ring.write(nullptr, 5), 0u);
    EXPECT_EQ(ring.read(nullptr, 5), 0u);
    ring.clear();
    ring.reset(1);
    const uint8_t byte = 42;
    uint8_t got = 0;
    EXPECT_EQ(ring.write(&byte, 1), 1u);
    EXPECT_EQ(ring.write(&byte, 1), 0u);
    EXPECT_EQ(ring.read(&got, 1), 1u);
    EXPECT_EQ(got, byte);
}

TEST(SpscRingBoundary, PartialWritesReadsClearAndResetPreserveFifo) {
    SpscRing ring(7);
    std::array<uint8_t, 10> bytes{};
    std::iota(bytes.begin(), bytes.end(), uint8_t(1));
    std::array<uint8_t, 10> got{};
    EXPECT_EQ(ring.write(bytes.data(), bytes.size()), 7u);
    EXPECT_EQ(ring.writable(), 0u);
    EXPECT_EQ(ring.read(got.data(), 3), 3u);
    EXPECT_EQ(ring.write(bytes.data() + 7, 3), 3u);
    EXPECT_EQ(ring.read(got.data() + 3, 7), 7u);
    EXPECT_EQ(got, bytes);
    ring.write(bytes.data(), 5);
    ring.clear();
    EXPECT_EQ(ring.readable(), 0u);
    EXPECT_EQ(ring.writable(), 7u);
    ring.reset(2);
    EXPECT_EQ(ring.capacity(), 2u);
    EXPECT_EQ(ring.readable(), 0u);
    EXPECT_EQ(ring.write(bytes.data(), 3), 2u);
}

TEST(WaveFormat, IntegerAndFloatingPointLayouts) {
    for (auto type : {SampleType::Int16, SampleType::Int24, SampleType::Int24In32, SampleType::Int32, SampleType::Float32}) {
        const auto wave = makeWaveFormat({96000, 2, type});
        EXPECT_EQ(wave.Format.wFormatTag, WAVE_FORMAT_EXTENSIBLE);
        EXPECT_EQ(wave.Format.nChannels, 2);
        EXPECT_EQ(wave.Format.nSamplesPerSec, 96000u);
        EXPECT_EQ(wave.Format.nBlockAlign, 2 * bytesPerSample(type));
        EXPECT_EQ(wave.Format.nAvgBytesPerSec, 96000 * 2 * bytesPerSample(type));
        EXPECT_EQ(wave.Format.wBitsPerSample, 8 * bytesPerSample(type));
        EXPECT_EQ(wave.Samples.wValidBitsPerSample, validBits(type));
        EXPECT_EQ(wave.Format.cbSize, 22);
        EXPECT_EQ(wave.dwChannelMask, SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
        EXPECT_TRUE(IsEqualGUID(wave.SubFormat,
                                type == SampleType::Float32 ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM));
    }
}

TEST(SettingsBoundary, UnknownIdsDoNotOverwriteOutputs) {
    Backend backend = Backend::Ks;
    EXPECT_FALSE(backendFromId("WASAPI", backend));
    EXPECT_EQ(backend, Backend::Ks);
    DecoderKind decoder = DecoderKind::DrFlac;
    EXPECT_FALSE(decoderFromId("unknown", decoder));
    EXPECT_EQ(decoder, DecoderKind::DrFlac);
    EXPECT_FALSE(sampleTypeFromId("invalid"));
    for (auto type : {SampleType::Int16, SampleType::Int24, SampleType::Int24In32, SampleType::Int32, SampleType::Float32})
        EXPECT_EQ(sampleTypeFromId(sampleTypeId(type)), type);
    for (auto value : {Backend::Wasapi, Backend::Asio, Backend::Ks}) {
        EXPECT_TRUE(backendFromId(backendId(value), backend));
        EXPECT_EQ(backend, value);
    }
}

TEST(SettingsBoundary, WrongTypesAndUnknownEnumsUseDefaults) {
    const auto settings = fromJson(R"({"backend":"other","decoder":5,"language":false,"lastFile":[],
        "wasapi":{"device":1,"eventDriven":"yes","period":"other","customPeriodMs":[],"sampleType":"other"},
        "asio":{"bufferSize":"512","outLeft":null,"outRight":true},"ks":{"mode":"other","packetCount":"4"}})");
    EXPECT_EQ(toJson(settings), toJson(Settings{}));
}

TEST(SettingsMigration, V1DecoderChoiceSurvivesUpgradeAndNewSettingsUseSndfile) {
    for (const auto decoder : audio::kAllDecoders) {
        const auto json = std::string(R"({"schema":1,"backend":"ks","decoder":")") + audio::decoderId(decoder) + R"("})";
        const auto migrated = fromJson(json);
        EXPECT_EQ(migrated.backend, Backend::Ks);
        EXPECT_EQ(migrated.decoder, decoder);
        EXPECT_NE(toJson(migrated).find(R"("schema": 2)"), std::string::npos);
    }

    const auto fresh = Settings{};
    EXPECT_EQ(fresh.decoder, DecoderKind::Sndfile);
    EXPECT_EQ(fromJson(R"({"schema":2})").decoder, DecoderKind::Sndfile);
}

TEST(SettingsMigration, RejectsFutureSchemaVersions) {
    EXPECT_THROW(fromJson(R"({"schema":99})"), std::invalid_argument);
    EXPECT_THROW(fromJson(R"({"schema":"2"})"), std::invalid_argument);
}

TEST(SettingsMigration, LoadingV1PersistsTheMigratedSchema) {
    const auto file = caseFile(".json");
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << R"({"schema":1,"decoder":"drflac"})";
    }
    const auto settings = loadSettings(file);
    EXPECT_EQ(settings.decoder, DecoderKind::DrFlac);
    std::ifstream in(file, std::ios::binary);
    std::stringstream contents;
    contents << in.rdbuf();
    in.close();
    EXPECT_NE(contents.str().find(R"("schema": 2)"), std::string::npos);
    std::filesystem::remove(file);
}

TEST(SettingsBoundary, AllFieldsAndModesSurvivePersistence) {
    const auto file = caseFile(".json");
    for (auto mode : {KsMode::Auto, KsMode::WaveRtEvent, KsMode::WaveRtPoll, KsMode::Standard}) {
        Settings settings;
        settings.backend = Backend::Asio;
        settings.decoder = DecoderKind::Sndfile;
        settings.language = "zh-TW";
        settings.lastFile = L"C:\\music\\album.flac";
        settings.wasapi = {"endpoint", false, WasapiPeriod::Custom, 12.5, SampleType::Int24In32};
        settings.asio = {"driver", 1024, 2, 3};
        settings.ks = {"filter|1", mode, 30, 8, 15, SampleType::Int32};
        saveSettings(settings, file);
        EXPECT_EQ(toJson(loadSettings(file)), toJson(settings));
        settings.language = "en-US";
        saveSettings(settings, file);
        EXPECT_EQ(loadSettings(file).language, "en-US");
        EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(file).concat(L".tmp")));
    }
    std::filesystem::remove(file);
}

TEST(SettingsBoundary, InvalidRootIsRejected) {
    for (const char* value : {"null", "[]", "123", "true", "\"text\""}) EXPECT_THROW(fromJson(value), std::exception);
}

TEST(SettingsBoundary, OutOfRangeNumericValuesUseDefaults) {
    const auto settings = fromJson(R"({"asio":{"bufferSize":1e100,"outLeft":-1,"outRight":1e100},
        "wasapi":{"customPeriodMs":-1},"ks":{"bufferMs":1e100,"packetCount":1e100,"packetMs":-1}})");
    EXPECT_EQ(settings.asio, Settings{}.asio);
    EXPECT_EQ(settings.wasapi, Settings{}.wasapi);
    EXPECT_EQ(settings.ks, Settings{}.ks);
    EXPECT_EQ(fromJson(R"({"asio":{"outLeft":1.5},"ks":{"packetCount":3.7}})").asio.outLeft, 0);
    EXPECT_EQ(fromJson(R"({"ks":{"packetCount":3.7}})").ks.packetCount, 4);
}

TEST(SettingsBoundary, MissingFileUsesDefaultsAndWriteFailureIsReported) {
    const auto missing = caseFile("-missing") / "settings.json";
    EXPECT_EQ(toJson(loadSettings(missing)), toJson(Settings{}));
    EXPECT_THROW(saveSettings({}, missing), std::runtime_error);
}

TEST(SettingsBoundary, FailedReplacementPreservesDestination) {
    const auto directory = caseFile("-destination");
    std::filesystem::create_directories(directory);
    EXPECT_THROW(saveSettings({}, directory), std::runtime_error);
    EXPECT_TRUE(std::filesystem::is_directory(directory));
    std::filesystem::remove(directory);
    std::filesystem::remove(std::filesystem::path(directory).concat(L".tmp"));
}

TEST(FlacStreamInfoBoundary, MissingAndTruncatedFilesAreRejectedWithoutMutatingOutput) {
    SourceInfo result{123, 1, 8, 42};
    EXPECT_FALSE(readFlacStreamInfo(caseFile("-missing"), result));
    const auto file = caseFile(".flac");
    const auto valid = streamInfo();
    for (size_t size : {0u, 3u, 9u, 10u, 41u}) {
        writeBytes(file, {valid.begin(), valid.begin() + size});
        EXPECT_FALSE(readFlacStreamInfo(file, result));
        EXPECT_EQ(result.sampleRate, 123u);
        EXPECT_EQ(result.totalFrames, 42u);
    }
    std::filesystem::remove(file);
}

TEST(FlacStreamInfoBoundary, RejectsInvalidMetadataLengthAndParameters) {
    const auto file = caseFile(".flac");
    const auto valid = streamInfo();
    for (uint8_t length : {uint8_t(0), uint8_t(33), uint8_t(35)}) {
        auto bytes = valid;
        bytes[7] = length;
        writeBytes(file, bytes);
        SourceInfo result;
        EXPECT_FALSE(readFlacStreamInfo(file, result)) << int(length);
    }
    auto bytes = valid;
    bytes[18] = bytes[19] = 0;
    bytes[20] &= 0x0F;
    writeBytes(file, bytes);
    SourceInfo result;
    EXPECT_FALSE(readFlacStreamInfo(file, result));
    bytes = valid;
    bytes[4] = 1;
    writeBytes(file, bytes);
    EXPECT_FALSE(readFlacStreamInfo(file, result));
    std::filesystem::remove(file);
}

TEST(FlacStreamInfoBoundary, ReadsId3PrefixAndClearsAbsentMd5) {
    auto bytes = streamInfo();
    std::vector<uint8_t> prefixed{'I', 'D', '3', 4, 0, 0, 0, 0, 0, 3, 1, 2, 3};
    prefixed.insert(prefixed.end(), bytes.begin(), bytes.end());
    const auto file = caseFile(".flac");
    writeBytes(file, prefixed);
    SourceInfo result;
    ASSERT_TRUE(readFlacStreamInfo(file, result));
    EXPECT_EQ(result.sampleRate, 96000u);
    EXPECT_EQ(result.totalFrames, 192000u);
    EXPECT_TRUE(result.md5);
    std::fill(bytes.begin() + 26, bytes.end(), 0);
    writeBytes(file, bytes);
    ASSERT_TRUE(readFlacStreamInfo(file, result));
    EXPECT_FALSE(result.md5);
    std::filesystem::remove(file);
}

TEST(Md5, KnownVectorsAndIncrementalUpdates) {
    struct Vector {
        const char* input;
        const char* expected;
    };
    for (const auto& vector : {Vector{"", "d41d8cd98f00b204e9800998ecf8427e"}, Vector{"abc", "900150983cd24fb0d6963f7d28e17f72"},
                               Vector{"message digest", "f96b697d7cb7938d525a2f31aaf161d0"}}) {
        Md5 md5;
        md5.update(nullptr, 0);
        for (const char* p = vector.input; *p; ++p) md5.update(p, 1);
        EXPECT_EQ(toHex(md5.finish()), vector.expected);
    }
}

TEST(Md5, FinishedHashRejectsFurtherUpdatesAndFinalization) {
    Md5 hash;
    hash.finish();
    EXPECT_THROW(hash.update("a", 1), std::runtime_error);
    EXPECT_THROW(hash.finish(), std::runtime_error);
}

TEST(FlacMd5, InvalidDepthIsRejected) {
    for (auto bits : {0u, 33u, std::numeric_limits<uint32_t>::max()})
        EXPECT_THROW({ FlacMd5 invalid(bits); }, std::invalid_argument);
}

TEST(FlacMd5, SignedPcmMatchesIndependentLittleEndianBytes) {
    const std::array<int32_t, 5> pcm{0, 1, -1, -32768, 32767};
    const std::vector<uint8_t> pcm16{0, 0, 1, 0, 255, 255, 0, 128, 255, 127};
    const std::vector<uint8_t> pcm24{0, 0, 0, 1, 0, 0, 255, 255, 255, 0, 128, 255, 255, 127, 0};
    for (auto bits : {16u, 24u}) {
        const auto& bytes = bits == 16 ? pcm16 : pcm24;
        Md5 expected;
        expected.update(bytes.data(), bytes.size());
        FlacMd5 actual(bits);
        actual.update(pcm.data(), 2);
        actual.update(pcm.data() + 2, 3);
        EXPECT_EQ(actual.finish(), expected.finish());
    }
}

TEST(Text, UnicodeAndEmbeddedNullRoundTrip) {
    const std::wstring wide = L"Camel \u97F3\u6A02 \U0001F42A";
    EXPECT_EQ(toWide(toUtf8(wide)), wide);
    const std::string embedded("a\0b", 3);
    EXPECT_EQ(toUtf8(toWide(embedded)), embedded);
    EXPECT_TRUE(toWide("").empty());
    EXPECT_TRUE(toUtf8(L"").empty());
}

TEST(Text, LongPathsNormalizeRelativeDriveAndUncPaths) {
    EXPECT_EQ(longPath(L"C:\\music\\..\\album.flac"), L"\\\\?\\C:\\album.flac");
    EXPECT_EQ(longPath(L"\\\\server\\share\\album.flac"), L"\\\\?\\UNC\\server\\share\\album.flac");
    EXPECT_EQ(longPath(L"\\\\?\\C:\\album.flac"), L"\\\\?\\C:\\album.flac");
    EXPECT_EQ(longPath(L"album.flac"), L"\\\\?\\" + (std::filesystem::current_path() / L"album.flac").wstring());
}

} // namespace
