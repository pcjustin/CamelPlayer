// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

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

TEST(CrashDump, StartupKeepsTheNewestFiveDumpsAndOtherFiles) {
    const auto directory = testutil::tempDir() / "crashdump-retention";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    for (int day = 1; day <= 7; ++day)
        std::ofstream(directory / ("crash-2026100" + std::to_string(day) + "-120000.dmp")) << "MDMP";
    std::ofstream(directory / "notes.txt") << "kept";
    installCrashDumps(directory);
    std::vector<std::string> left;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) left.push_back(entry.path().filename().string());
    std::sort(left.begin(), left.end());
    EXPECT_EQ(left,
              (std::vector<std::string>{"crash-20261003-120000.dmp", "crash-20261004-120000.dmp", "crash-20261005-120000.dmp",
                                        "crash-20261006-120000.dmp", "crash-20261007-120000.dmp", "notes.txt"}));
    std::filesystem::remove_all(directory);
}

std::vector<std::wstring> dumpNames(const std::filesystem::path& directory) {
    std::vector<std::wstring> names;
    for (const auto& entry : std::filesystem::directory_iterator(directory))
        if (entry.path().extension() == L".dmp" && std::filesystem::file_size(entry.path()) > 32)
            names.push_back(entry.path().filename().wstring());
    return names;
}

TEST(CrashDump, AbortInChildCreatesDump) {
    constexpr wchar_t marker[] = L"CAMELPLAYER_ABORTDUMP_TEST_CHILD";
    wchar_t childDirectory[32768];
    const DWORD length = GetEnvironmentVariableW(marker, childDirectory, DWORD(std::size(childDirectory)));
    if (length && length < std::size(childDirectory)) {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        installCrashDumps(childDirectory);
        std::abort();
    }
    const auto directory = testutil::tempDir() / "abortdump-unit-test";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    ASSERT_TRUE(SetEnvironmentVariableW(marker, directory.c_str()));
    wchar_t executable[32768];
    ASSERT_NE(GetModuleFileNameW(nullptr, executable, DWORD(std::size(executable))), 0u);
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --gtest_filter=CrashDump.AbortInChildCreatesDump";
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                                        &startup, &process);
    SetEnvironmentVariableW(marker, nullptr);
    ASSERT_TRUE(created);
    const DWORD waited = WaitForSingleObject(process.hProcess, 15000);
    if (waited != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 1);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    ASSERT_EQ(waited, WAIT_OBJECT_0);
    const auto names = dumpNames(directory);
    ASSERT_EQ(names.size(), 1u);
    EXPECT_TRUE(names.front().ends_with(L"-abort.dmp"));
    std::filesystem::remove_all(directory);
}

TEST(CrashDump, UnansweredPingWritesOneHangDump) {
    const auto directory = testutil::tempDir() / "hangdump-unit-test";
    std::filesystem::remove_all(directory);
    installCrashDumps(directory);
    std::vector<std::function<void()>> pings;
    std::mutex mutex;
    {
        HangWatch watch(
            [&](std::function<void()> ping) {
                std::lock_guard lock(mutex);
                pings.push_back(std::move(ping));
                return true;
            },
            std::chrono::seconds(2));
        std::this_thread::sleep_for(std::chrono::milliseconds(4500));
    }
    EXPECT_EQ(pings.size(), 1u);
    const auto names = dumpNames(directory);
    ASSERT_EQ(names.size(), 1u);
    EXPECT_TRUE(names.front().ends_with(L"-hang.dmp"));
    std::filesystem::remove_all(directory);
}

TEST(CrashDump, AnsweredPingsWriteNoDump) {
    const auto directory = testutil::tempDir() / "hangdump-answered";
    std::filesystem::remove_all(directory);
    installCrashDumps(directory);
    size_t pings = 0;
    {
        HangWatch watch(
            [&](const std::function<void()>& ping) {
                ++pings;
                ping();
                return true;
            },
            std::chrono::seconds(2));
        std::this_thread::sleep_for(std::chrono::milliseconds(3500));
    }
    EXPECT_GE(pings, 3u);
    EXPECT_TRUE(dumpNames(directory).empty());
    std::filesystem::remove_all(directory);
}

TEST(CrashDump, NoDumpsOrAnUnusableDirectoryNeverBlockStartup) {
    const auto directory = testutil::tempDir() / "crashdump-empty";
    std::filesystem::remove_all(directory);
    EXPECT_NO_THROW(installCrashDumps(directory)); // created empty
    EXPECT_TRUE(std::filesystem::is_directory(directory));
    std::filesystem::remove_all(directory);
    const auto file = testutil::tempDir() / "crashdump-not-a-directory";
    std::ofstream(file) << "x";
    EXPECT_NO_THROW(installCrashDumps(file / "crashdumps")); // cannot be created under a file
    EXPECT_NO_THROW(pruneCrashDumps(file, kKeptCrashDumps));
    std::filesystem::remove(file);
}

} // namespace
