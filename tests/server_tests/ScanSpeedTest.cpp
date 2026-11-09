// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <thread>

#include "Common/settings/Settings.h"
#include "Server/Library/LibraryScanner.h"
#include "Server/Library/LibraryStore.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::server;
namespace fs = std::filesystem;

namespace {

fs::path freshDir(const char* name) {
    const auto dir = testutil::tempDir() / "scan-speed" / name;
    fs::remove_all(dir);
    fs::create_directories(dir / "music");
    return dir;
}

bool waitFor(const std::function<bool()>& check) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    do {
        if (check()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < end);
    return false;
}

LibraryMonitor::Options quietMonitor() {
    LibraryMonitor::Options options;
    options.pollInterval = std::chrono::milliseconds(0);
    options.networkPollInterval = std::chrono::milliseconds(0);
    options.offlineInterval = std::chrono::milliseconds(100);
    options.coalesce = std::chrono::milliseconds(100);
    return options;
}

int64_t filesIn(LibraryStore& store) {
    return store.openReader().scalar("SELECT COUNT(*) FROM files WHERE available=1").value();
}

} // namespace

TEST(ScanSpeed, IsLowByDefaultAndRoundTrips) {
    EXPECT_EQ(Settings{}.scanSpeed, ScanSpeed::Low);
    EXPECT_EQ(fromJson("{}").scanSpeed, ScanSpeed::Low); // an install that never set it
    EXPECT_EQ(fromJson(R"({"scanSpeed":"fastest"})").scanSpeed, ScanSpeed::Low);
    for (const auto speed : {ScanSpeed::Low, ScanSpeed::Medium, ScanSpeed::High}) {
        Settings settings;
        settings.scanSpeed = speed;
        EXPECT_EQ(fromJson(toJson(settings)).scanSpeed, speed);
        EXPECT_EQ(resetPlayback(settings).scanSpeed, speed);
        EXPECT_EQ(resetInterface(settings).scanSpeed, speed);
    }
    // ReplayGain analysis had a speed of its own, which the scan speed replaced.
    EXPECT_EQ(fromJson(R"({"replayGain":{"mode":"track","analysis":"fast"}})").replayGain.mode, ReplayGainMode::Track);
}

TEST(ScanSpeed, WorkerCountsFollowTheSetting) {
    LibraryStore store(freshDir("counts"));
    EXPECT_EQ(store.backgroundWorkers(BackgroundWork::Scan, 16), 1U); // Low until set
    store.setScanSpeed(ScanSpeed::Medium);
    EXPECT_EQ(store.backgroundWorkers(BackgroundWork::Scan, 16), 8U);
    EXPECT_EQ(store.backgroundWorkers(BackgroundWork::Fingerprints, 4), 2U);
    EXPECT_EQ(store.backgroundWorkers(BackgroundWork::Loudness, 3), 1U);
    EXPECT_EQ(store.backgroundWorkers(BackgroundWork::Covers, 1), 1U);
    store.setScanSpeed(ScanSpeed::High);
    EXPECT_EQ(store.backgroundWorkers(BackgroundWork::Scan, 16), 16U);
    EXPECT_EQ(store.backgroundWorkers(BackgroundWork::Loudness, 12), 12U);
    store.setScanSpeed(ScanSpeed::Low);
    for (const auto kind : {BackgroundWork::Scan, BackgroundWork::Fingerprints, BackgroundWork::Covers, BackgroundWork::Loudness})
        EXPECT_EQ(store.backgroundWorkers(kind, 16), 1U);
}

TEST(ScanSpeed, AUsersScanAndTheWorkFollowingItRunAtHigh) {
    LibraryStore store(freshDir("user"));
    store.userScanStarted();
    for (const auto kind : {BackgroundWork::Scan, BackgroundWork::Fingerprints, BackgroundWork::Covers, BackgroundWork::Loudness})
        EXPECT_EQ(store.backgroundWorkers(kind, 16), 16U);
    store.caughtUp(BackgroundWork::Fingerprints); // before the scan ends it has more to find
    store.userScanEnded();
    EXPECT_EQ(store.backgroundWorkers(BackgroundWork::Scan, 16), 1U);
    EXPECT_EQ(store.backgroundWorkers(BackgroundWork::Fingerprints, 16), 16U);
    store.caughtUp(BackgroundWork::Fingerprints);
    EXPECT_EQ(store.backgroundWorkers(BackgroundWork::Fingerprints, 16), 1U);
    EXPECT_EQ(store.backgroundWorkers(BackgroundWork::Covers, 4), 4U);
    EXPECT_EQ(store.backgroundWorkers(BackgroundWork::Loudness, 8), 8U);
    store.caughtUp(BackgroundWork::Covers);
    store.caughtUp(BackgroundWork::Loudness);
    EXPECT_EQ(store.backgroundWorkers(BackgroundWork::Covers, 4), 1U);
    EXPECT_EQ(store.backgroundWorkers(BackgroundWork::Loudness, 8), 1U);
}

TEST(ScanSpeed, AChangeReachesWorkAlreadyRunning) {
    LibraryStore store(freshDir("live"));
    store.setScanSpeed(ScanSpeed::High);
    std::atomic<bool> stop = false;
    std::atomic<unsigned> seen = 0;
    // A running worker asks before each item, as the scan, fingerprints, covers and loudness analysis do.
    std::thread worker([&] {
        while (!stop) {
            seen = store.backgroundWorkers(BackgroundWork::Scan, 16);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    ASSERT_TRUE(waitFor([&] { return seen == 16U; }));
    store.setScanSpeed(ScanSpeed::Low);
    EXPECT_TRUE(waitFor([&] { return seen == 1U; }));
    stop = true;
    worker.join();
}

// The follow-up work's mark stays until that work catches up; covers, which only the Server's cover warmer catches up,
// keep it here, so it shows a scan the user asked for ran.
TEST(ScanSpeed, RescanningAndAddingAFolderAreTheUsersScans) {
    const auto dir = freshDir("monitor");
    fs::create_directories(dir / "music" / "album");
    fs::copy_file(testutil::testFile("16-44.1k"), dir / "music" / "album" / "one.flac");
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    scanner.addRoot(dir / "music");
    LibraryMonitor monitor(store, quietMonitor());
    ASSERT_TRUE(waitFor([&] { return filesIn(store) == 1 && store.scanProgress().scans == 0; }));
    EXPECT_EQ(store.backgroundWorkers(BackgroundWork::Covers, 4), 1U); // the scan at start is background work
    monitor.rescanAll();
    EXPECT_TRUE(waitFor([&] { return store.backgroundWorkers(BackgroundWork::Covers, 4) == 4U; }));
    // The scan's own count goes back to Low once it ends.
    EXPECT_TRUE(waitFor([&] { return store.backgroundWorkers(BackgroundWork::Scan, 16) == 1U; }));
    store.caughtUp(BackgroundWork::Covers);

    fs::create_directories(dir / "more" / "album");
    fs::copy_file(testutil::testFile("16-44.1k"), dir / "more" / "album" / "two.flac");
    monitor.scanAdded(scanner.addRoot(dir / "more"));
    EXPECT_TRUE(waitFor([&] { return store.backgroundWorkers(BackgroundWork::Covers, 4) == 4U; }));
    EXPECT_TRUE(waitFor([&] { return filesIn(store) == 2; }));
}
