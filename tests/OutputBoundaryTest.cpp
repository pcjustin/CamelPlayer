#include <gtest/gtest.h>

#include <fstream>
#include <future>
#include <thread>

#include "audio/engine/PlaybackEngine.h"
#include "audio/output/AsioOutput.h"
#include "audio/output/CaptureOutput.h"
#include "audio/output/KsOutput.h"
#include "audio/output/WasapiOutput.h"
#include "util/Log.h"
#include "TestUtil.h"
#include <wil/resource.h>

using namespace camel;
using namespace camel::audio;

namespace {

class SilentSource final : public IRenderSource {
public:
    size_t render(uint8_t*, size_t) noexcept override { return 0; }
};

TEST(OutputBoundary, MissingWasapiDeviceReportsTypedFailure) {
    WasapiSettings settings;
    settings.deviceId = "camelplayer-test-nonexistent-endpoint";
    auto output = createWasapiOutput(settings);
    SilentSource source;
    EXPECT_THROW(output->supports({48000, 2, SampleType::Int16}), Error);
    try {
        output->open({48000, 2, SampleType::Int16}, &source);
        FAIL() << "Missing WASAPI endpoint was opened";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::DeviceNotFound);
    }
    EXPECT_NO_THROW(output->close());
}

TEST(OutputBoundary, InvalidAsioIdReportsTypedFailure) {
    AsioSettings settings;
    settings.driverClsid = "not-a-clsid";
    auto output = createAsioOutput(settings);
    SilentSource source;
    try {
        output->open({48000, 2, SampleType::Int16}, &source);
        FAIL() << "Invalid ASIO driver was opened";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::DeviceNotFound);
    }
    EXPECT_NO_THROW(output->close());
}

TEST(OutputBoundary, MalformedKsIdsAreRejected) {
    for (const char* id : {"missing-separator", "|0", "filter|", "filter|-1", "filter|2junk", "filter|4294967296"}) {
        KsSettings settings;
        settings.deviceId = id;
        try {
            auto output = createKsOutput(settings);
            FAIL() << "Accepted malformed KS ID: " << id;
        } catch (const Error& error) {
            EXPECT_EQ(error.code(), ErrorCode::DeviceNotFound);
        }
    }
}

TEST(OutputBoundary, RepeatedKsConstructionDoesNotLeakHandles) {
    KsSettings settings;
    settings.deviceId = "\\\\?\\camelplayer-nonexistent-filter|0";
    {
        auto warmup = createKsOutput(settings);
    }
    DWORD before = 0, after = 0;
    ASSERT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &before));
    for (int i = 0; i < 10; ++i) {
        auto output = createKsOutput(settings);
    }
    ASSERT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &after));
    EXPECT_EQ(after, before);
}

TEST(OutputBoundary, MissingKsFilterReportsTypedFailure) {
    KsSettings settings;
    settings.deviceId = "\\\\?\\camelplayer-nonexistent-filter|0";
    auto output = createKsOutput(settings);
    SilentSource source;
    EXPECT_THROW(output->supports({48000, 2, SampleType::Int16}), Error);
    try {
        output->open({48000, 2, SampleType::Int16}, &source);
        FAIL() << "Missing KS filter was opened";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::DeviceNotFound);
    }
    EXPECT_NO_THROW(output->close());
}

TEST(OutputBoundary, CaptureCannotCreateFileInMissingDirectory) {
    CaptureOptions options;
    options.file = testutil::tempDir() / "nonexistent-capture-directory" / "output.raw";
    auto output = createCaptureOutput(options);
    SilentSource source;
    EXPECT_THROW(output->open({48000, 2, SampleType::Int16}, &source), Error);
    EXPECT_NO_THROW(output->close());
}

TEST(OutputBoundary, EnginePreservesMissingDeviceErrorFromCapabilityProbe) {
    for (auto backend : {Backend::Wasapi, Backend::Ks}) {
        Settings settings;
        settings.backend = backend;
        settings.wasapi.deviceId = "camelplayer-test-nonexistent-endpoint";
        settings.ks.deviceId = "\\\\?\\camelplayer-nonexistent-filter|0";
        PlaybackEngine engine(settings);
        engine.openAndPlay(testutil::testFile("24-96k"));
        engine.waitIdle();
        EXPECT_EQ(engine.snapshot().error, ErrorCode::DeviceNotFound);
        EXPECT_EQ(engine.snapshot().state, PlayState::Stopped);
        EXPECT_FALSE(engine.snapshot().output);
    }
}

TEST(OutputBoundary, CaptureReportsWriteFailureInsteadOfSilentlyStalling) {
    class PcmSource final : public IRenderSource {
    public:
        size_t render(uint8_t* destination, size_t frames) noexcept override {
            std::fill_n(destination, frames * 4, uint8_t(0));
            return frames;
        }
    } source;
    std::promise<ErrorCode> failure;
    auto result = failure.get_future();
    CaptureOptions options;
    options.file = testutil::tempDir() / "capture-write-failure.raw";
    auto output = createCaptureOutput(options);
    output->setErrorHandler([&](const Error& error) { failure.set_value(error.code()); });
    output->open({48000, 2, SampleType::Int16}, &source);
    {
        wil::unique_hfile lockFile(CreateFileW(options.file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                               OPEN_EXISTING, 0, nullptr));
        ASSERT_TRUE(lockFile);
        OVERLAPPED lock{};
        ASSERT_TRUE(
            LockFileEx(lockFile.get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &lock));
        output->start();
        EXPECT_EQ(result.wait_for(std::chrono::seconds(3)), std::future_status::ready);
        output->close();
    }
    if (result.wait_for(std::chrono::seconds(0)) == std::future_status::ready) EXPECT_EQ(result.get(), ErrorCode::Other);
    std::filesystem::remove(options.file);
}

TEST(Log, ConcurrentWritesAndRetention) {
    const auto directory = testutil::tempDir() / "log-unit-tests";
    std::filesystem::create_directories(directory);
    const auto expired = directory / "expired.log";
    const auto unrelated = directory / "preserve.txt";
    {
        std::ofstream file(expired);
        file << "expired";
    }
    {
        std::ofstream file(unrelated);
        file << "preserve";
    }
    const auto old = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 8);
    std::filesystem::last_write_time(expired, old);
    std::filesystem::last_write_time(unrelated, old);
    log::init(directory, L"unit");
    std::vector<std::thread> writers;
    for (int thread = 0; thread < 4; ++thread)
        writers.emplace_back([thread] {
            for (int line = 0; line < 20; ++line) log::info("writer=%d line=%d", thread, line);
        });
    for (auto& writer : writers) writer.join();
    log::warn("warning-marker");
    log::error("error-marker");
    EXPECT_FALSE(std::filesystem::exists(expired));
    EXPECT_TRUE(std::filesystem::exists(unrelated));
    std::filesystem::path logFile;
    for (const auto& entry : std::filesystem::directory_iterator(directory))
        if (entry.path().extension() == L".log") logFile = entry.path();
    ASSERT_FALSE(logFile.empty());
    std::ifstream input(logFile);
    const std::string text(std::istreambuf_iterator<char>(input), {});
    for (int thread = 0; thread < 4; ++thread)
        for (int line = 0; line < 20; ++line)
            EXPECT_NE(text.find("writer=" + std::to_string(thread) + " line=" + std::to_string(line) + "\n"), std::string::npos);
    EXPECT_NE(text.find("[warn]"), std::string::npos);
    EXPECT_NE(text.find("warning-marker"), std::string::npos);
    EXPECT_NE(text.find("error-marker"), std::string::npos);
    log::init(testutil::tempDir() / "log-after-tests", L"test");
    input.close();
    for (const auto& entry : std::filesystem::directory_iterator(directory)) std::filesystem::remove(entry.path());
    std::filesystem::remove(directory);
}

} // namespace
