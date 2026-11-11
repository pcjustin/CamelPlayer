// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <thread>

#include <wil/resource.h>

#include <future>

#include "Common/DecoderRegistry.h"
#include "Engine/decode/IDecoder.h"
#include "Common/settings/Settings.h"
#include "Common/util/Text.h"
#include "Server/Library/LibraryScanner.h"
#include "Server/Library/LibraryStore.h"
#include "Server/Playback/LocalServer.h"
#include "Tools/testaudio/TestAudio.h"
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

namespace {

int64_t availableLike(LibraryStore& store, const char* name) {
    auto row = store.openReader().prepare("SELECT COUNT(*) FROM files WHERE available=1 AND path LIKE ?");
    row.bind(1, std::string("%") + name);
    row.step();
    return row.integer(0);
}

fs::path storedRoot(LibraryStore& store) {
    auto row = store.openReader().prepare("SELECT path FROM roots");
    row.step();
    return fs::path(toWide(row.text(0)));
}

} // namespace

// Storage that refuses change notifications is scanned at its poll interval, not again at every check that it is there.
TEST(UnwatchedStorage, IsScannedOnlyAtItsPollInterval) {
    const auto dir = freshDir("unwatched");
    fs::create_directories(dir / "music");
    fs::copy_file(testutil::testFile("16-44.1k"), dir / "music" / "one.flac");
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    scanner.addRoot(dir / "music");
    // Every scan of the whole folder records when it ended; this clears it, so a scan shows as a time again.
    const auto forget = [&] { store.write([](sqlite::Database& db) { db.exec("UPDATE roots SET last_scan=NULL"); }).get(); };
    const auto scanned = [&] { return store.openReader().scalar("SELECT COUNT(*) FROM roots WHERE last_scan IS NOT NULL") == 1; };
    auto options = quietMonitor(); // checks the storage every 100 ms
    options.refuseNotifications = true;
    {
        LibraryMonitor monitor(store, options);
        ASSERT_TRUE(waitFor([&] { return filesIn(store) == 1 && scanned() && store.scanProgress().scans == 0; }));
        forget();
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        EXPECT_FALSE(scanned()) << "scanned again without a poll";
    }
    options.pollInterval = std::chrono::milliseconds(500);
    LibraryMonitor polled(store, options);
    ASSERT_TRUE(waitFor([&] { return scanned() && store.scanProgress().scans == 0; }));
    forget();
    EXPECT_TRUE(waitFor([&] { return scanned(); })) << "the poll still scans it";
}

// Scan now finds what a share's missed notifications left out: a file added, one changed and one removed.
TEST(ScanningByHand, ScanNowFindsWhatChangedWithoutNotifications) {
    const auto dir = freshDir("scan-now");
    const auto album = dir / "music" / "album";
    fs::create_directories(album);
    fs::copy_file(testutil::testFile("16-44.1k"), album / "kept.flac");
    fs::copy_file(testutil::testFile("16-44.1k"), album / "gone.flac");
    fs::copy_file(testutil::testFile("16-44.1k"), album / "changed.flac");
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    scanner.addRoot(dir / "music");
    auto options = quietMonitor();
    options.watchChanges = false;
    LibraryMonitor monitor(store, options);
    ASSERT_TRUE(waitFor([&] { return filesIn(store) == 3 && store.scanProgress().scans == 0; }));
    fs::copy_file(testutil::testFile("16-44.1k"), album / "added.flac");
    fs::remove(album / "gone.flac");
    fs::copy_file(testutil::testFile("24-96k"), album / "changed.flac", fs::copy_options::overwrite_existing);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_EQ(filesIn(store), 3); // nothing noticed it
    monitor.scanAll();
    EXPECT_TRUE(waitFor([&] { return availableLike(store, "added.flac") == 1 && availableLike(store, "gone.flac") == 0; }));
    EXPECT_TRUE(waitFor([&] {
        return store.openReader().scalar("SELECT sample_rate FROM tracks JOIN files ON files.id=tracks.file_id "
                                         "WHERE files.path LIKE '%changed.flac'") == 96000;
    }));
    EXPECT_EQ(store.backgroundWorkers(BackgroundWork::Covers, 4), 4U); // the user's scan
}

// Scan this folder finds the changes in that folder and below it, and leaves the rest of the library folder alone.
// The files differ, so the library does not take one for another moved.
TEST(ScanningByHand, ScanThisFolderScansOnlyThatFolder) {
    const auto dir = freshDir("scan-folder");
    fs::create_directories(dir / "music" / "album" / "disc 2");
    fs::create_directories(dir / "music" / "other");
    fs::copy_file(testutil::testFile("16-44.1k"), dir / "music" / "album" / "one.flac");
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(dir / "music");
    auto options = quietMonitor();
    options.watchChanges = false;
    LibraryMonitor monitor(store, options);
    ASSERT_TRUE(waitFor([&] { return filesIn(store) == 1 && store.scanProgress().scans == 0; }));
    fs::copy_file(testutil::testFile("24-96k"), dir / "music" / "album" / "disc 2" / "below.flac");
    fs::copy_file(testutil::testFile("no-md5"), dir / "music" / "other" / "elsewhere.flac");
    fs::remove(dir / "music" / "album" / "one.flac");
    monitor.scanFolder(root, storedRoot(store) / "album");
    EXPECT_TRUE(waitFor([&] { return availableLike(store, "below.flac") == 1 && availableLike(store, "one.flac") == 0; }));
    ASSERT_TRUE(waitFor([&] { return store.scanProgress().scans == 0; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(availableLike(store, "elsewhere.flac"), 0); // "other" was not scanned
    monitor.scanFolder(root, storedRoot(store) / "other");
    EXPECT_TRUE(waitFor([&] { return availableLike(store, "elsewhere.flac") == 1; }));
}

// Scan now and Scan this folder start nothing while a scan runs; once it ends they start.
TEST(ScanningByHand, ASecondRequestWhileAScanRunsStartsNothing) {
    const auto dir = freshDir("second-request");
    using namespace camel::audio;
    testaudio::writeWav(dir / "music" / "slow.wav", testaudio::makeSignal({"short", 44100, 16, 2, 0.1}), 44100, 16, 2);
    openDecoder(DecoderKind::Sndfile, testutil::testFile("16-44.1k")); // fills the registry before its opener is replaced
    const auto id = decoderId(DecoderKind::Sndfile);
    const auto opener = decoderRegistry().find(id).value().open;
    auto restore = wil::scope_exit([&] { decoderRegistry().setOpener(id, opener); });
    std::promise<void> release, started;
    auto gate = release.get_future().share();
    std::atomic<bool> first = true;
    decoderRegistry().setOpener(id, [&, opener](const fs::path& path, std::string_view format, uint32_t bits) {
        if (path.filename() == "slow.wav" && first.exchange(false)) {
            started.set_value();
            gate.wait();
        }
        return opener(path, format, bits);
    });
    int64_t folder = 0;
    {
        LibraryStore store(dir / "db");
        LibraryScanner scanner(store);
        scanner.addRoot(dir / "music");
    }
    LocalServer server(Settings{}, dir / "db" / "settings.json");
    auto unblock = wil::scope_exit([&] {
        if (first.load()) return;
        try {
            release.set_value();
        } catch (const std::future_error&) {
        }
    });
    ASSERT_EQ(started.get_future().wait_for(std::chrono::seconds(10)), std::future_status::ready); // the scan at start
    folder = sqlite::Database(dir / "db" / "library.db", sqlite::Database::Mode::ReadOnly)
                 .scalar("SELECT id FROM folders WHERE parent_id IS NULL")
                 .value();
    EXPECT_EQ(server.scanLibraryNow(std::nullopt), LibraryScanStart::Running);
    EXPECT_EQ(server.scanLibraryNow(folder), LibraryScanStart::Running);
    release.set_value();
    ASSERT_TRUE(waitFor([&] { return server.libraryScanProgress().scans == 0; }));
    EXPECT_EQ(server.scanLibraryNow(folder), LibraryScanStart::Started);
    ASSERT_TRUE(waitFor([&] { return server.libraryScanProgress().scans == 0; }));
    EXPECT_EQ(server.scanLibraryNow(std::nullopt), LibraryScanStart::Started);
}
