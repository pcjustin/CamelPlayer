// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <limits>
#include <numeric>

#include "Engine/decode/FlacStreamInfo.h"
#include "Engine/FormatRule.h"
#include "Engine/SamplePacker.h"
#include "OutputBridge/WaveFormat.h"
#include "Engine/verify/FlacMd5.h"
#include "Common/settings/Settings.h"
#include "Common/util/Md5.h"
#include "Common/util/Text.h"
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
        for (auto bits : {0u, 8u, 20u, 32u, 33u}) EXPECT_FALSE(canCarryBitPerfect(type, bits)) << bits;
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
    for (const auto& source : {SourceInfo{0, 2, 16}, SourceInfo{48000, 0, 16}, SourceInfo{48000, 6, 24}, SourceInfo{48000, 2, 0},
                               SourceInfo{48000, 2, 33}})
        EXPECT_FALSE(negotiateOutput(source, false, std::nullopt, false, supports));
    EXPECT_FALSE(negotiateOutput(SourceInfo{48000, 9, 24}, false, std::nullopt, true, supports));
    EXPECT_EQ(probes, 0);
}

TEST(FormatRuleBoundary, PreservesRateAndStopsAtFirstSupportedContainer) {
    std::vector<SampleType> probes;
    auto plan = negotiateOutput({88200, 2, 16}, false, std::nullopt, false, [&](const DeviceFormat& candidate) {
        EXPECT_EQ(candidate.sampleRate, 88200u);
        EXPECT_EQ(candidate.channels, 2u);
        probes.push_back(candidate.type);
        return candidate.type == SampleType::Int24;
    });
    ASSERT_TRUE(plan);
    EXPECT_EQ(plan->format.type, SampleType::Int24);
    EXPECT_EQ(probes, (std::vector{SampleType::Int16, SampleType::Int24}));
    const auto widened = negotiateOutput({48000, 2, 16}, false, SampleType::Float32, false, [](const auto&) { return true; });
    ASSERT_TRUE(widened);
    EXPECT_TRUE(widened->widen);
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

DecoderByFormat flacChoice(DecoderKind decoder) {
    DecoderByFormat choices;
    choices[AudioFormat::Flac] = decoder;
    return choices;
}

TEST(SettingsMigration, GlobalDecoderChoiceBecomesTheFlacChoiceAndNewSettingsUseSndfile) {
    for (const int schema : {1, 2})
        for (const auto decoder : audio::decodersFor(AudioFormat::Flac)) {
            const auto json =
                R"({"schema":)" + std::to_string(schema) + R"(,"backend":"ks","decoder":")" + audio::decoderId(decoder) + R"("})";
            const auto migrated = fromJson(json);
            EXPECT_EQ(migrated.backend, Backend::Ks);
            EXPECT_EQ(migrated.decoderByFormat, flacChoice(decoder));
            const auto saved = toJson(migrated);
            EXPECT_NE(saved.find(R"("schema": 3)"), std::string::npos);
            EXPECT_EQ(saved.find(R"("decoder")"), std::string::npos);
        }

    EXPECT_EQ(Settings{}.decoderByFormat, DecoderByFormat{});
    EXPECT_EQ(decoderFor(Settings{}.decoderByFormat, AudioFormat::Flac), DecoderKind::Sndfile);
    EXPECT_EQ(fromJson(R"({"schema":2})").decoderByFormat, DecoderByFormat{});
    EXPECT_EQ(fromJson(R"({"schema":3,"decoder":"drflac"})").decoderByFormat, DecoderByFormat{});
}

TEST(SettingsBoundary, RegistryDefaultsFollowThePlan) {
    const DecoderByFormat none;
    EXPECT_EQ(decoderFor(none, AudioFormat::Flac), DecoderKind::Sndfile);
    EXPECT_EQ(decoderFor(none, AudioFormat::Alac), DecoderKind::Symphonia);
    EXPECT_EQ(decoderFor(none, AudioFormat::Wav), DecoderKind::Sndfile);
    EXPECT_EQ(decoderFor(none, AudioFormat::Aiff), DecoderKind::Sndfile);
    for (const auto format : kAllFormats) {
        const auto decoders = decodersFor(format);
        if (decoders.empty()) continue;
        EXPECT_TRUE(supports(format, defaultDecoder(format))) << formatName(format);
    }
    EXPECT_EQ(decoderFor(none, AudioFormat::Aac), DecoderKind::Symphonia);
    EXPECT_EQ(decoderFor(none, AudioFormat::Mp3), DecoderKind::Mpg123);
    EXPECT_EQ(decoderFor(none, AudioFormat::Dsd), DecoderKind::DsdNexus);
    EXPECT_EQ(decodersFor(AudioFormat::Dsd).size(), 1u);
}

TEST(SettingsBoundary, ConversionIsOffByDefaultWithHighQualityResampling) {
    EXPECT_FALSE(Settings{}.convertWhenUnsupported);
    EXPECT_EQ(Settings{}.resampleQuality, ResampleQuality::High);
    Settings settings;
    settings.convertWhenUnsupported = true;
    for (const auto quality : {ResampleQuality::Quick, ResampleQuality::High, ResampleQuality::VeryHigh}) {
        settings.resampleQuality = quality;
        const auto back = fromJson(toJson(settings));
        EXPECT_TRUE(back.convertWhenUnsupported);
        EXPECT_EQ(back.resampleQuality, quality);
    }
    const auto invalid = fromJson(R"({"convertWhenUnsupported":"yes","resampleQuality":"best"})");
    EXPECT_FALSE(invalid.convertWhenUnsupported);
    EXPECT_EQ(invalid.resampleQuality, ResampleQuality::High);
}

TEST(SettingsBoundary, LossyOutputDepthDefaultsTo24AndAccepts16) {
    EXPECT_EQ(pcmBitsFor(PcmBitsByFormat{}, AudioFormat::Aac), 24u);
    EXPECT_EQ(pcmBitsFor(PcmBitsByFormat{}, AudioFormat::Mp3), 24u);
    const auto settings = fromJson(R"({"schema":3,"pcmBitsByFormat":{"aac":16,"mp3":24,"flac":16,"wav":24,"ogg":16}})");
    PcmBitsByFormat expected;
    expected[AudioFormat::Aac] = 16;
    expected[AudioFormat::Mp3] = 24;
    EXPECT_EQ(settings.pcmBitsByFormat, expected);
    EXPECT_EQ(fromJson(toJson(settings)).pcmBitsByFormat, expected);
    for (const char* json : {R"({"pcmBitsByFormat":{"aac":20}})", R"({"pcmBitsByFormat":{"mp3":"16"}})",
                             R"({"pcmBitsByFormat":{"mp3":16.5}})", R"({"pcmBitsByFormat":[16]})"})
        EXPECT_EQ(fromJson(json).pcmBitsByFormat, PcmBitsByFormat{}) << json;
}

TEST(SettingsBoundary, DecoderChoicesKeepOnlyIntegratedPairs) {
    const auto settings = fromJson(R"({"schema":3,"decoderByFormat":{"flac":"symphonia","alac":"ffmpeg",
        "ogg":"ffmpeg","mp3":"sndfile","wav":"drflac","aiff":7}})");
    auto expected = flacChoice(DecoderKind::Symphonia);
    expected[AudioFormat::Alac] = DecoderKind::FFmpeg;
    EXPECT_EQ(settings.decoderByFormat, expected);
    EXPECT_EQ(fromJson(R"({"schema":3,"decoderByFormat":{"flac":"mpg123"}})").decoderByFormat, DecoderByFormat{});
    EXPECT_EQ(fromJson(R"({"schema":3,"decoderByFormat":["flac"]})").decoderByFormat, DecoderByFormat{});
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
    EXPECT_EQ(decoderFor(settings.decoderByFormat, AudioFormat::Flac), DecoderKind::DrFlac);
    std::ifstream in(file, std::ios::binary);
    std::stringstream contents;
    contents << in.rdbuf();
    in.close();
    EXPECT_NE(contents.str().find(R"("schema": 3)"), std::string::npos);
    EXPECT_NE(contents.str().find(R"("flac": "drflac")"), std::string::npos);
    std::filesystem::remove(file);
}

TEST(SettingsBoundary, ResettingOneGroupKeepsTheOther) {
    Settings settings;
    settings.backend = Backend::Asio;
    settings.asio.driverClsid = "{DAVE}";
    settings.asio.bufferSize = 2048;
    settings.wasapi.deviceId = "endpoint";
    settings.convertWhenUnsupported = true;
    settings.decoderByFormat[AudioFormat::Flac] = DecoderKind::DrFlac;
    settings.language = "zh-TW";
    settings.closeAction = CloseAction::Exit;
    settings.trayNoticeShown = settings.firstRunDone = true;
    settings.lastFile = L"C:\\music\\a.flac";

    const auto playback = resetPlayback(settings);
    EXPECT_EQ(playback.backend, Backend::Wasapi);
    EXPECT_EQ(playback.asio, AsioSettings{});
    EXPECT_EQ(playback.wasapi, WasapiSettings{});
    EXPECT_FALSE(playback.convertWhenUnsupported);
    EXPECT_FALSE(playback.decoderByFormat[AudioFormat::Flac]);
    EXPECT_EQ(playback.language, "zh-TW");
    EXPECT_EQ(playback.closeAction, CloseAction::Exit);
    EXPECT_TRUE(playback.trayNoticeShown && playback.firstRunDone);
    EXPECT_EQ(playback.lastFile, settings.lastFile);

    const auto ui = resetInterface(settings);
    EXPECT_EQ(ui.language, "");
    EXPECT_EQ(ui.closeAction, CloseAction::Tray);
    EXPECT_FALSE(ui.trayNoticeShown || ui.firstRunDone);
    EXPECT_EQ(ui.backend, Backend::Asio);
    EXPECT_EQ(ui.asio, settings.asio);
    EXPECT_TRUE(ui.convertWhenUnsupported);
}

TEST(SettingsBoundary, TheFirstRunGuideIsDueUntilFinishedOrSkipped) {
    EXPECT_FALSE(Settings{}.firstRunDone);
    Settings settings;
    settings.firstRunDone = true;
    EXPECT_TRUE(fromJson(toJson(settings)).firstRunDone);
    EXPECT_FALSE(fromJson(R"({"firstRunDone":"yes"})").firstRunDone);
}

TEST(SettingsBoundary, ClosingTheWindowMinimizesToTheTrayUntilSetToExit) {
    EXPECT_EQ(Settings{}.closeAction, CloseAction::Tray);
    EXPECT_FALSE(Settings{}.trayNoticeShown);
    Settings settings;
    settings.closeAction = CloseAction::Exit;
    settings.trayNoticeShown = true;
    const auto back = fromJson(toJson(settings));
    EXPECT_EQ(back.closeAction, CloseAction::Exit);
    EXPECT_TRUE(back.trayNoticeShown);
    const auto invalid = fromJson(R"({"closeAction":"hide","trayNoticeShown":"yes"})");
    EXPECT_EQ(invalid.closeAction, CloseAction::Tray);
    EXPECT_FALSE(invalid.trayNoticeShown);
}

TEST(SettingsBoundary, AllFieldsAndModesSurvivePersistence) {
    const auto file = caseFile(".json");
    for (auto mode : {KsMode::Auto, KsMode::WaveRtEvent, KsMode::WaveRtPoll, KsMode::Standard}) {
        Settings settings;
        settings.backend = Backend::Asio;
        settings.decoderByFormat[AudioFormat::Flac] = DecoderKind::Sndfile;
        settings.language = "zh-TW";
        settings.lastFile = L"C:\\music\\album.flac";
        settings.wasapi = {"endpoint", true, ResampleQuality::Quick, false, WasapiPeriod::Custom, 12.5, SampleType::Int24In32};
        settings.asio = {"driver", 1024, 2, 3, DsdOutput::Dop};
        settings.ks = {"filter|1", mode, 30, 8, 15, SampleType::Int32, DsdOutput::Pcm};
        settings.wasapi.fixedRate = 96000;
        settings.wasapi.pollBufferMs = 50;
        settings.wasapi.mmcssTask = MmcssTask::Playback;
        settings.wasapi.mmcssPriority = MmcssPriority::Critical;
        settings.wasapi.timing = {20, 500, 0};
        settings.asio.fixedRate = 352800;
        settings.asio.timing = {1, 2000, 3600};
        settings.ks.mmcssTask = MmcssTask::Disabled;
        settings.ks.mmcssPriority = MmcssPriority::Normal;
        settings.ks.timing = {60, 0, 30};
        saveSettings(settings, file);
        EXPECT_EQ(toJson(loadSettings(file)), toJson(settings));
        settings.language = "en-US";
        saveSettings(settings, file);
        EXPECT_EQ(loadSettings(file).language, "en-US");
        EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(file).concat(L".tmp")));
    }
    std::filesystem::remove(file);
}

TEST(SettingsBoundary, OutputParametersDefaultAndRejectInvalidValues) {
    const Settings defaults;
    for (const PlaybackTiming* t : {&defaults.wasapi.timing, &defaults.asio.timing, &defaults.ks.timing})
        EXPECT_EQ(*t, (PlaybackTiming{10, 0, 60}));
    EXPECT_EQ(defaults.wasapi.fixedRate, 0u);
    EXPECT_EQ(defaults.wasapi.pollBufferMs, 200.0);
    EXPECT_EQ(defaults.wasapi.mmcssTask, MmcssTask::ProAudio);
    EXPECT_EQ(defaults.wasapi.mmcssPriority, MmcssPriority::High);
    EXPECT_EQ(defaults.ks.mmcssPriority, MmcssPriority::High);
    const auto parsed = fromJson(R"({"backend":"ks",
        "wasapi":{"fixedRate":12345,"pollBufferMs":5,"mmcssTask":"games","mmcssPriority":"low",
                  "predecodeSeconds":0,"formatChangeSilenceMs":2001,"releaseAfterPauseSeconds":-1},
        "asio":{"fixedRate":"48000","predecodeSeconds":2.5},
        "ks":{"fixedRate":768000,"mmcssTask":"playback","mmcssPriority":"critical","releaseAfterPauseSeconds":0}})");
    EXPECT_EQ(parsed.wasapi.fixedRate, 0u);
    EXPECT_EQ(parsed.wasapi.pollBufferMs, 200.0);
    EXPECT_EQ(parsed.wasapi.mmcssTask, MmcssTask::ProAudio);
    EXPECT_EQ(parsed.wasapi.mmcssPriority, MmcssPriority::High);
    EXPECT_EQ(parsed.wasapi.timing, (PlaybackTiming{10, 0, 60}));
    EXPECT_EQ(parsed.asio.fixedRate, 0u);
    EXPECT_EQ(parsed.asio.timing.predecodeSeconds, 10u);
    EXPECT_EQ(parsed.ks.fixedRate, 768000u);
    EXPECT_EQ(parsed.ks.mmcssTask, MmcssTask::Playback);
    EXPECT_EQ(parsed.ks.mmcssPriority, MmcssPriority::Critical);
    EXPECT_EQ(parsed.ks.timing.releaseAfterPauseSeconds, 0u);
    EXPECT_EQ(fixedRate(parsed), 768000u);
    EXPECT_EQ(&timing(parsed), &parsed.ks.timing);
}

TEST(SettingsBoundary, DsdOutputDefaultsToNativeOnAsioAndDopOnKs) {
    const Settings defaults;
    EXPECT_EQ(defaults.asio.dsdOutput, DsdOutput::Native);
    EXPECT_EQ(defaults.ks.dsdOutput, DsdOutput::Dop);
    EXPECT_EQ(defaults.wasapi.dsdOutput, DsdOutput::Dop);
    EXPECT_FALSE(defaults.convertDsdWhenUnsupported);
    for (const uint32_t rate : {defaults.wasapi.dsdPcmRate, defaults.asio.dsdPcmRate, defaults.ks.dsdPcmRate})
        EXPECT_EQ(rate, 176400u);
    const auto parsed = fromJson(R"({"convertDsdWhenUnsupported":true,
        "asio":{"dsdOutput":"dop","dsdPcmRate":88200},"ks":{"dsdOutput":"native","dsdPcmRate":352800},
        "wasapi":{"dsdOutput":"pcm"}})");
    EXPECT_TRUE(parsed.convertDsdWhenUnsupported);
    EXPECT_EQ(parsed.asio.dsdOutput, DsdOutput::Dop);
    EXPECT_EQ(parsed.asio.dsdPcmRate, 88200u);
    EXPECT_EQ(parsed.ks.dsdOutput, DsdOutput::Dop); // KS has no native DSD
    EXPECT_EQ(parsed.ks.dsdPcmRate, 352800u);
    EXPECT_EQ(parsed.wasapi.dsdOutput, DsdOutput::Pcm);
    EXPECT_EQ(fromJson(toJson(parsed)).asio, parsed.asio);
    const auto unknown = fromJson(R"({"convertDsdWhenUnsupported":"yes","asio":{"dsdOutput":"both","dsdPcmRate":96000},
        "ks":{"dsdOutput":7},"wasapi":{"dsdOutput":"native"}})");
    EXPECT_FALSE(unknown.convertDsdWhenUnsupported);
    EXPECT_EQ(unknown.asio.dsdOutput, DsdOutput::Native);
    EXPECT_EQ(unknown.asio.dsdPcmRate, 176400u);
    EXPECT_EQ(unknown.ks.dsdOutput, DsdOutput::Dop);
    EXPECT_EQ(unknown.wasapi.dsdOutput, DsdOutput::Dop); // WASAPI has no native DSD
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
    for (auto bits : {0u, 8u, 20u, 32u, 33u, std::numeric_limits<uint32_t>::max()})
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
