#include <gtest/gtest.h>

#include <fstream>
#include <limits>

#include "Engine/testing/TestAudio.h"
#include "Common/util/CrashDump.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::audio;

namespace {

TEST(TestAudio, SignalIsDeterministicAndBoundedForSupportedDepths) {
    for (auto bits : {16u, 24u}) {
        testaudio::GenSpec spec{"unit-signal", 48000, bits, 2, 0.01};
        const auto first = testaudio::makeSignal(spec);
        EXPECT_EQ(first, testaudio::makeSignal(spec));
        EXPECT_EQ(first.size(), 960u);
        const int32_t minimum = -(int32_t(1) << (bits - 1));
        const int32_t maximum = (int32_t(1) << (bits - 1)) - 1;
        for (auto sample : first) {
            ASSERT_GE(sample, minimum);
            ASSERT_LE(sample, maximum);
        }
        spec.seconds = 0;
        EXPECT_TRUE(testaudio::makeSignal(spec).empty());
    }
}

TEST(TestAudio, InvalidSignalParametersAreRejected) {
    const testaudio::GenSpec valid{"invalid-signal", 48000, 16, 2, 0.01};
    for (auto bits : {0u, 3u, 33u}) {
        auto spec = valid;
        spec.bits = bits;
        EXPECT_THROW(testaudio::makeSignal(spec), std::invalid_argument);
    }
    for (auto channels : {0u, 9u}) {
        auto spec = valid;
        spec.channels = channels;
        EXPECT_THROW(testaudio::makeSignal(spec), std::invalid_argument);
    }
    for (auto seconds : {-1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        auto spec = valid;
        spec.seconds = seconds;
        EXPECT_THROW(testaudio::makeSignal(spec), std::invalid_argument);
    }
    auto spec = valid;
    spec.sampleRate = 0;
    EXPECT_THROW(testaudio::makeSignal(spec), std::invalid_argument);
}

TEST(TestAudio, GeneratedFlacHasExactSamplesAndVerifiedMd5) {
    for (auto bits : {16u, 24u}) {
        const auto file = testutil::tempDir() / ("generated-unit-" + std::to_string(bits) + ".flac");
        testaudio::GenSpec spec{"unit", 48000, bits, 2, 0.01};
        testaudio::writeFlac(file, spec);
        auto reference = testaudio::referenceDecode(file);
        ASSERT_TRUE(reference.error.empty()) << reference.error;
        EXPECT_TRUE(reference.md5Checked);
        EXPECT_TRUE(reference.md5Ok);
        EXPECT_EQ(reference.samples, testaudio::makeSignal(spec));
        spec.md5 = false;
        testaudio::writeFlac(file, spec);
        reference = testaudio::referenceDecode(file);
        EXPECT_FALSE(reference.info.md5);
        EXPECT_FALSE(reference.md5Checked);
        EXPECT_FALSE(reference.md5Ok);
        EXPECT_EQ(reference.samples, testaudio::makeSignal(spec));
        std::filesystem::remove(file);
    }
}

TEST(TestAudio, InvalidCorruptionFractionsAndMissingInputsAreRejected) {
    const auto source = testutil::testFile("24-96k");
    const auto destination = testutil::tempDir() / "corruption-boundary.flac";
    for (double fraction : {-0.1, 1.1, std::numeric_limits<double>::quiet_NaN()}) {
        EXPECT_THROW(testaudio::writeTruncated(source, destination, fraction), std::invalid_argument);
        EXPECT_THROW(testaudio::writeCorrupted(source, destination, fraction), std::invalid_argument);
    }
    EXPECT_THROW(testaudio::writeTruncated(source.parent_path() / "missing-unit-input", destination), std::runtime_error);
    testaudio::writeTruncated(source, destination, 0);
    EXPECT_EQ(std::filesystem::file_size(destination), 0u);
    EXPECT_FALSE(testaudio::referenceDecode(destination).error.empty());
    std::filesystem::remove(destination);
}

TEST(CrashDump, UnhandledChildExceptionCreatesDump) {
    constexpr wchar_t marker[] = L"CAMELPLAYER_CRASHDUMP_TEST_CHILD";
    wchar_t childDirectory[32768];
    const DWORD length = GetEnvironmentVariableW(marker, childDirectory, DWORD(std::size(childDirectory)));
    if (length && length < std::size(childDirectory)) {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        installCrashDumps(childDirectory);
        RaiseException(0xE0424242, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        FAIL() << "Unhandled exception unexpectedly returned";
        return;
    }
    const auto directory = testutil::tempDir() / "crashdump-unit-test";
    std::filesystem::create_directories(directory);
    for (const auto& entry : std::filesystem::directory_iterator(directory)) std::filesystem::remove(entry.path());
    ASSERT_TRUE(SetEnvironmentVariableW(marker, directory.c_str()));
    wchar_t executable[32768];
    ASSERT_NE(GetModuleFileNameW(nullptr, executable, DWORD(std::size(executable))), 0u);
    std::wstring command = L"\"" + std::wstring(executable) +
                           L"\" --gtest_filter=CrashDump.UnhandledChildExceptionCreatesDump --gtest_catch_exceptions=0";
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                                        &startup, &process);
    SetEnvironmentVariableW(marker, nullptr);
    ASSERT_TRUE(created);
    const DWORD waited = WaitForSingleObject(process.hProcess, 15000);
    if (waited != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
    }
    DWORD exitCode = 0;
    EXPECT_TRUE(GetExitCodeProcess(process.hProcess, &exitCode));
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    ASSERT_EQ(waited, WAIT_OBJECT_0);
    EXPECT_EQ(exitCode, 0xE0424242u);
    size_t dumps = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() != L".dmp") continue;
        ++dumps;
        std::ifstream dump(entry.path(), std::ios::binary);
        char signature[4]{};
        dump.read(signature, sizeof signature);
        EXPECT_EQ(std::string(signature, sizeof signature), "MDMP");
        EXPECT_GT(std::filesystem::file_size(entry.path()), 32u);
    }
    EXPECT_EQ(dumps, 1u);
    for (const auto& entry : std::filesystem::directory_iterator(directory)) std::filesystem::remove(entry.path());
    std::filesystem::remove(directory);
}

} // namespace
