// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>
#include <windows.h>

#include <atomic>
#include <fstream>
#include <thread>

#include "Server/Library/LibraryIdentity.h"
#include "Server/Library/LibraryScanner.h"
#include "TestUtil.h"

using namespace camel::server;
namespace fs = std::filesystem;

namespace {

class LibraryIdentityTest : public ::testing::Test {
protected:
    fs::path dir, music;
    void SetUp() override {
        dir = testutil::tempDir() / "identity" / ::testing::UnitTest::GetInstance()->current_test_info()->name();
        fs::remove_all(dir);
        music = dir / "music";
        fs::create_directories(music);
    }
    fs::path file(const char* name, const std::string& bytes = "abc") {
        const auto path = music / name;
        std::ofstream(path, std::ios::binary) << bytes;
        return path;
    }
    static int64_t scalar(LibraryStore& store, const char* sql) { return store.openReader().scalar(sql).value(); }
    static int64_t id(LibraryStore& store, const char* name) {
        auto db = store.openReader();
        auto row = db.prepare("SELECT id FROM files WHERE path LIKE ? AND available=1");
        row.bind(1, std::string("%\\") + name);
        return row.step() ? row.integer(0) : 0;
    }
    static void rate(LibraryStore& store, int64_t file) {
        store
            .write([=](sqlite::Database& db) {
                db.prepare("INSERT INTO stats(file_id,rating,favorite,play_count,last_played) VALUES(?,5,1,17,123)")
                    .bind(1, file)
                    .run();
            })
            .get();
    }
    static void distrustIds(LibraryStore& store) {
        store.write([](sqlite::Database& db) { db.exec("UPDATE files SET volume_key=NULL,file_index=x'00',created=1"); }).get();
    }
};

TEST_F(LibraryIdentityTest, LocalRenamePreservesTracksStatsPlaylistQueueAndCueReferences) {
    const auto source = music / "old.flac";
    fs::copy_file(testutil::testFile("16-44.1k"), source);
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(music);
    ASSERT_TRUE(scanner.scan(root).complete);
    const auto original = id(store, "old.flac");
    const auto track = scalar(store, "SELECT id FROM tracks");
    ASSERT_FALSE(library::fileVersion(source).volumeKey.empty());
    rate(store, original);
    store
        .write([=](sqlite::Database& db) {
            db.prepare("INSERT INTO tracks(file_id,cue_index,cue_file_id,title) VALUES(?,1,?,'Virtual')")
                .bind(1, original)
                .bind(2, original)
                .run();
            db.prepare("INSERT INTO stats(file_id,cue_index,rating) VALUES(?,1,4)").bind(1, original).run();
            db.exec("INSERT INTO playlists(id,name,created,modified) VALUES(1,'Kept',0,0)");
            db.prepare("INSERT INTO playlist_items(playlist_id,position,file_id,source_description) VALUES(1,0,?,'Kept')")
                .bind(1, original)
                .run();
            db.prepare("INSERT INTO queue(position,entry_id,file_id,path,source_description) VALUES(0,77,?,'old','Kept')")
                .bind(1, original)
                .run();
        })
        .get();
    fs::rename(source, music / "new.flac");
    const auto report = scanner.scan(root);
    ASSERT_TRUE(report.complete);
    EXPECT_EQ(report.moved, 1);
    EXPECT_EQ(id(store, "new.flac"), original);
    EXPECT_EQ(scalar(store, "SELECT id FROM tracks WHERE cue_index=0"), track);
    EXPECT_EQ(scalar(store, "SELECT play_count FROM stats WHERE cue_index=0"), 17);
    EXPECT_EQ(scalar(store, "SELECT favorite FROM stats WHERE cue_index=0"), 1);
    EXPECT_EQ(scalar(store, "SELECT last_played FROM stats WHERE cue_index=0"), 123);
    EXPECT_EQ(scalar(store, "SELECT rating FROM stats WHERE cue_index=1"), 4);
    EXPECT_EQ(scalar(store, "SELECT file_id FROM playlist_items"), original);
    EXPECT_EQ(scalar(store, "SELECT file_id FROM queue WHERE entry_id=77"), original);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM queue JOIN files ON files.id=queue.file_id WHERE queue.path=files.path"), 1);
    EXPECT_EQ(scalar(store, "SELECT cue_file_id FROM tracks WHERE cue_index=1"), original);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM tracks"), 2);
}

TEST_F(LibraryIdentityTest, MoveAcrossRegisteredRootsDoesNotNeedTheSourceRootToBeRescanned) {
    const auto source = file("old.lrc");
    const auto other = dir / "other";
    fs::create_directory(other);
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(music), destination = scanner.addRoot(other);
    ASSERT_TRUE(scanner.scan(root).complete);
    const auto original = id(store, "old.lrc");
    rate(store, original);
    fs::rename(source, other / "new.lrc");
    EXPECT_EQ(scanner.scan(destination).moved, 1);
    EXPECT_EQ(id(store, "new.lrc"), original);
    EXPECT_TRUE(scanner.scan(root).complete);
    EXPECT_EQ(id(store, "new.lrc"), original);
}

TEST_F(LibraryIdentityTest, MovedFileThatBecomesUnsupportedKeepsHistoryWithoutAStaleWholeTrack) {
    const auto source = music / "old.flac", destination = music / "new.flac";
    fs::copy_file(testutil::testFile("16-44.1k"), source);
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(music);
    ASSERT_TRUE(scanner.scan(root).complete);
    const auto original = id(store, "old.flac");
    rate(store, original);
    fs::rename(source, destination);
    const camel::audio::testaudio::GenSpec spec{"32bit", 44100, 32, 2, 0.05};
    camel::audio::testaudio::writeFlac(destination, spec);
    const auto report = scanner.scan(root);
    ASSERT_TRUE(report.complete);
    EXPECT_EQ(report.moved, 1);
    EXPECT_EQ(id(store, "new.flac"), original);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM tracks"), 0);
    EXPECT_EQ(scalar(store, "SELECT rating FROM stats"), 5);
}

TEST_F(LibraryIdentityTest, ReplacementAndSizeTimeCollisionNeverInheritHistory) {
    const auto source = file("old.lrc");
    const auto stamp = fs::last_write_time(source);
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(music);
    ASSERT_TRUE(scanner.scan(root).complete);
    const auto original = id(store, "old.lrc");
    rate(store, original);
    ASSERT_EQ(scanner.fingerprint(root).hashed, 1);
    fs::rename(source, music / "moved.lrc");
    file("old.lrc", "xyz");
    fs::last_write_time(source, stamp);
    EXPECT_EQ(scanner.scan(root).moved, 1);
    EXPECT_EQ(id(store, "moved.lrc"), original);
    EXPECT_NE(id(store, "old.lrc"), original);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM stats"), 1);
}

TEST_F(LibraryIdentityTest, UniqueRecordedHashRecoversUntrustedOrCrossVolumeIdentity) {
    const auto source = file("old.lrc");
    LibraryStore store(dir / "db");
    bool playing = false;
    LibraryScanner scanner(store, [&] { return playing; });
    const auto root = scanner.addRoot(music);
    ASSERT_TRUE(scanner.scan(root).complete);
    const auto original = id(store, "old.lrc");
    rate(store, original);
    ASSERT_EQ(scanner.fingerprint(root).hashed, 1);
    distrustIds(store);
    fs::copy_file(source, music / "new.lrc");
    fs::remove(source);
    playing = true;
    EXPECT_FALSE(scanner.scan(root).complete);
    EXPECT_EQ(id(store, "new.lrc"), 0);
    playing = false;
    EXPECT_EQ(scanner.scan(root).moved, 1);
    EXPECT_EQ(id(store, "new.lrc"), original);
    EXPECT_EQ(scalar(store, "SELECT rating FROM stats"), 5);
}

TEST_F(LibraryIdentityTest, MissingPriorFingerprintCannotBeInventedAfterTheMove) {
    const auto source = file("old.lrc");
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(music);
    ASSERT_TRUE(scanner.scan(root).complete);
    const auto original = id(store, "old.lrc");
    rate(store, original);
    distrustIds(store);
    fs::rename(source, music / "new.lrc");
    EXPECT_EQ(scanner.scan(root).moved, 0);
    EXPECT_NE(id(store, "new.lrc"), original);
    EXPECT_EQ(scanner.fingerprint(root).hashed, 1);
    EXPECT_EQ(scanner.scan(root).moved, 0);
    EXPECT_EQ(scalar(store, "SELECT available FROM files WHERE id=1"), 0);
    EXPECT_EQ(scalar(store, "SELECT file_id FROM stats"), original);
}

TEST_F(LibraryIdentityTest, EqualSizeAndTimeWithDifferentContentDoesNotMatch) {
    const auto source = file("old.lrc");
    const auto stamp = fs::last_write_time(source);
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(music);
    ASSERT_TRUE(scanner.scan(root).complete);
    const auto original = id(store, "old.lrc");
    rate(store, original);
    ASSERT_EQ(scanner.fingerprint(root).hashed, 1);
    distrustIds(store);
    fs::remove(source);
    fs::last_write_time(file("new.lrc", "xyz"), stamp);
    EXPECT_EQ(scanner.scan(root).moved, 0);
    EXPECT_NE(id(store, "new.lrc"), original);
}

TEST_F(LibraryIdentityTest, MultiplePriorHashesAreAmbiguous) {
    const auto source = file("one.lrc");
    file("two.lrc");
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(music);
    ASSERT_TRUE(scanner.scan(root).complete);
    rate(store, id(store, "one.lrc"));
    ASSERT_EQ(scanner.fingerprint(root).hashed, 2);
    distrustIds(store);
    fs::remove(music / "two.lrc");
    fs::rename(source, music / "new.lrc");
    EXPECT_EQ(scanner.scan(root).moved, 0);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM files WHERE available=1 AND id IN (SELECT file_id FROM stats)"), 0);
}

TEST_F(LibraryIdentityTest, MultipleArrivalsNeverLetTheFirstCopyStealHistory) {
    const auto source = file("old.lrc");
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(music);
    ASSERT_TRUE(scanner.scan(root).complete);
    rate(store, id(store, "old.lrc"));
    ASSERT_EQ(scanner.fingerprint(root).hashed, 1);
    distrustIds(store);
    fs::copy_file(source, music / "one.lrc");
    fs::copy_file(source, music / "two.lrc");
    fs::remove(source);
    EXPECT_EQ(scanner.scan(root).moved, 0);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM files WHERE available=1 AND id IN (SELECT file_id FROM stats)"), 0);
}

TEST_F(LibraryIdentityTest, LiveCopiesAndHardLinksDoNotStealIdentityEvenAfterTheSourceDisappears) {
    const auto source = file("old.lrc");
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(music);
    ASSERT_TRUE(scanner.scan(root).complete);
    const auto original = id(store, "old.lrc");
    rate(store, original);
    ASSERT_EQ(scanner.fingerprint(root).hashed, 1);
    fs::copy_file(source, music / "copy.lrc");
    fs::create_hard_link(source, music / "link.lrc");
    EXPECT_EQ(scanner.scan(root).moved, 0);
    const auto copy = id(store, "copy.lrc"), link = id(store, "link.lrc");
    EXPECT_NE(copy, original);
    EXPECT_NE(link, original);
    fs::remove(source);
    EXPECT_EQ(scanner.scan(root).moved, 0);
    EXPECT_EQ(id(store, "copy.lrc"), copy);
    EXPECT_EQ(id(store, "link.lrc"), link);
}

TEST_F(LibraryIdentityTest, PlaybackSourceGatesOnlyTheSameStorage) {
    const auto source = file("old.lrc");
    fs::path playing = source;
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store, {}, [&] { return playing; });
    const auto root = scanner.addRoot(music);
    ASSERT_TRUE(scanner.scan(root).complete);
    EXPECT_FALSE(scanner.fingerprint(root).complete);
    playing = L"\\\\TACHIKOMA\\Music\\track.flac";
    EXPECT_EQ(scanner.fingerprint(root).hashed, 1);
    file("second.lrc", "second bytes");
    ASSERT_TRUE(scanner.scan(root).complete);
    playing = source;
    EXPECT_FALSE(scanner.fingerprint(root).complete);
    playing.clear();
    EXPECT_EQ(scanner.fingerprint(root).hashed, 1);
    EXPECT_EQ(library::storageKey(source), library::storageKey(music));
    EXPECT_FALSE(library::storageKey(source).empty());
    EXPECT_EQ(library::storageKey(L"\\\\TACHIKOMA\\Music\\album\\track.flac"), L"\\\\tachikoma\\music");
    EXPECT_EQ(library::storageKey(L"\\\\?\\UNC\\TACHIKOMA\\Music"), L"\\\\tachikoma\\music");
    EXPECT_NE(library::storageKey(L"\\\\TACHIKOMA\\Other\\track.flac"), L"\\\\tachikoma\\music");
}

TEST_F(LibraryIdentityTest, RemovingRootRetainsHistoryAndReadditionRequiresVerifiedIdentity) {
    const auto source = file("old.lrc");
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(music);
    ASSERT_TRUE(scanner.scan(root).complete);
    const auto original = id(store, "old.lrc");
    rate(store, original);
    ASSERT_EQ(scanner.fingerprint(root).hashed, 1);
    store.write([=](sqlite::Database& db) {
        db.exec("INSERT INTO playlists(id,name,created,modified) VALUES(1,'Kept',0,0)");
        db.prepare("INSERT INTO playlist_items(playlist_id,position,file_id,source_description) VALUES(1,0,?,'Kept')")
            .bind(1, original).run();
        db.prepare("INSERT INTO queue(position,entry_id,file_id,path,source_description) VALUES(0,77,?,'old','Kept')")
            .bind(1, original).run();
    }).get();
    scanner.removeRoot(root);
    scanner.removeRoot(root);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM files WHERE available=1"), 0);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM roots WHERE removed IS NOT NULL AND online=0"), 1);
    EXPECT_EQ(scalar(store, "SELECT rating FROM stats"), 5);
    EXPECT_EQ(scalar(store, "SELECT file_id FROM playlist_items"), original);
    EXPECT_EQ(scalar(store, "SELECT file_id FROM queue"), original);
    EXPECT_THROW(scanner.scan(root), std::invalid_argument);
    EXPECT_FALSE(scanner.fingerprint(root).complete);
    ASSERT_EQ(scanner.addRoot(music), root);
    ASSERT_TRUE(scanner.scan(root).complete);
    EXPECT_EQ(id(store, "old.lrc"), original);
    scanner.removeRoot(root);
    fs::remove(source);
    file("old.lrc", "replacement with different content");
    ASSERT_EQ(scanner.addRoot(music), root);
    ASSERT_TRUE(scanner.scan(root).complete);
    EXPECT_NE(id(store, "old.lrc"), original);
    EXPECT_EQ(scalar(store, "SELECT file_id FROM stats"), original);
}

TEST_F(LibraryIdentityTest, RemovingRootCancelsRunningScanWithoutCancellingOtherRoots) {
    file("old.lrc");
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(music);
    ASSERT_TRUE(scanner.scan(root).complete);
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    std::atomic<bool> first = true;
    LibraryScanner slow(store, [&] {
        if (first.exchange(false)) { entered.set_value(); released.wait(); }
        return false;
    });
    CancellationToken parent;
    auto scan = std::async(std::launch::async, [&] { return slow.scan(root, parent); });
    const bool running = entered.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    if (running) scanner.removeRoot(root);
    release.set_value();
    ASSERT_TRUE(running);
    EXPECT_FALSE(scan.get().complete);
    EXPECT_FALSE(parent.cancelled());
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM files WHERE available=1"), 0);
    fs::create_directory(dir / "other");
    const auto other = scanner.addRoot(dir / "other");
    EXPECT_TRUE(scanner.scan(other, parent).complete);
}

TEST_F(LibraryIdentityTest, FingerprintsYieldCancelValidateVersionsAndInvalidateOnEdits) {
    const auto source = file("old.lrc");
    bool playing = true;
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store, [&] { return playing; });
    const auto root = scanner.addRoot(music);
    ASSERT_TRUE(scanner.scan(root).complete);
    EXPECT_FALSE(scanner.fingerprint(root).complete);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM files WHERE content_hash IS NOT NULL"), 0);
    playing = false;
    CancellationToken cancelled;
    cancelled.cancel();
    EXPECT_FALSE(scanner.fingerprint(root, cancelled).complete);
    const auto expected = library::fileVersion(source);
    const auto digest = library::fileDigest(source, expected, {}, {});
    ASSERT_TRUE(digest);
    const std::vector<uint8_t> sha256{0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
                                      0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
                                      0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    EXPECT_EQ(*digest, sha256);
    EXPECT_EQ(scanner.fingerprint(root).hashed, 1);
    file("old.lrc", "xyz");
    fs::last_write_time(source, fs::last_write_time(source) + std::chrono::seconds(2));
    EXPECT_THROW(library::fileDigest(source, expected, {}, {}), std::runtime_error);
    EXPECT_TRUE(scanner.scan(root).complete);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM files WHERE content_hash IS NOT NULL"), 0);
    EXPECT_EQ(scanner.fingerprint(root).hashed, 1);
}

TEST_F(LibraryIdentityTest, HashingStopsBetweenChunksWithoutCommittingAPartialDigest) {
    const auto source = file("large.lrc", std::string(3 << 20, 'x'));
    const auto expected = library::fileVersion(source);
    CancellationToken token;
    int checks = 0;
    EXPECT_FALSE(library::fileDigest(source, expected, token, [&] { return ++checks >= 3; }));
    checks = 0;
    EXPECT_FALSE(library::fileDigest(source, expected, token, [&] {
        if (++checks == 3) token.cancel();
        return false;
    }));
    EXPECT_THROW(library::fileDigest(source, expected, {},
                                     [&] {
                                         if (++checks == 5) {
                                             file("large.lrc", "changed");
                                             fs::last_write_time(source, fs::last_write_time(source) + std::chrono::seconds(2));
                                         }
                                         return false;
                                     }),
                 std::runtime_error);
}

TEST_F(LibraryIdentityTest, PartialScanDefersMoveResolutionUntilACompleteRetry) {
    const auto source = file("old.lrc");
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(music);
    ASSERT_TRUE(scanner.scan(root).complete);
    const auto original = id(store, "old.lrc");
    rate(store, original);
    fs::rename(source, music / "new.lrc");
    const auto blocked = music / "blocked.flac";
    fs::copy_file(testutil::testFile("16-44.1k"), blocked);
    HANDLE handle = CreateFileW(blocked.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    const auto partial = scanner.scan(root);
    CloseHandle(handle);
    EXPECT_FALSE(partial.complete);
    EXPECT_EQ(partial.moved, 0);
    EXPECT_NE(id(store, "new.lrc"), original);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM files WHERE identity_state=1"), 1);
    const auto provisional = id(store, "new.lrc");
    store
        .write([=](sqlite::Database& db) {
            db.prepare("INSERT INTO queue(position,entry_id,file_id,path,source_description) VALUES(0,99,?,'stale','Kept')")
                .bind(1, provisional)
                .run();
        })
        .get();
    EXPECT_EQ(scanner.scan(root).moved, 1);
    EXPECT_EQ(id(store, "new.lrc"), original);
    EXPECT_EQ(scalar(store, "SELECT file_id FROM queue WHERE entry_id=99"), original);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM queue JOIN files ON files.id=queue.file_id WHERE queue.path=files.path"), 1);
}

TEST_F(LibraryIdentityTest, NewUserHistoryOnAProvisionalIdentityPreventsMerging) {
    const auto source = file("old.lrc");
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(music);
    ASSERT_TRUE(scanner.scan(root).complete);
    const auto original = id(store, "old.lrc");
    rate(store, original);
    fs::rename(source, music / "new.lrc");
    const auto blocked = music / "blocked.flac";
    fs::copy_file(testutil::testFile("16-44.1k"), blocked);
    HANDLE handle = CreateFileW(blocked.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    const auto partial = scanner.scan(root);
    CloseHandle(handle);
    ASSERT_FALSE(partial.complete);
    const auto provisional = id(store, "new.lrc");
    rate(store, provisional);
    EXPECT_EQ(scanner.scan(root).moved, 0);
    EXPECT_EQ(id(store, "new.lrc"), provisional);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM stats"), 2);
}

TEST_F(LibraryIdentityTest, UnreadableSourceRootDoesNotTransferHistory) {
    const auto source = file("old.lrc");
    const auto other = dir / "other";
    fs::create_directory(other);
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(music), destination = scanner.addRoot(other);
    ASSERT_TRUE(scanner.scan(root).complete);
    const auto original = id(store, "old.lrc");
    rate(store, original);
    fs::rename(source, other / "new.lrc");
    fs::rename(music, dir / "offline");
    std::ofstream(music) << "not a directory";
    EXPECT_EQ(scanner.scan(destination).moved, 0);
    EXPECT_NE(id(store, "new.lrc"), original);
    EXPECT_EQ(scalar(store, "SELECT file_id FROM stats"), original);
}

TEST_F(LibraryIdentityTest, LegacySchemaUpgradePreservesVerifiedCurrentPathIdentity) {
    const auto source = file("old.lrc");
    const auto legacy = library::fileVersion(source).legacyIndex;
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(music);
    ASSERT_TRUE(scanner.scan(root).complete);
    const auto original = id(store, "old.lrc");
    rate(store, original);
    store
        .write([&](sqlite::Database& db) {
            db.prepare("UPDATE files SET file_index=?,created=0,volume_key=NULL").bind(1, legacy).run();
        })
        .get();
    EXPECT_TRUE(scanner.scan(root).complete);
    EXPECT_EQ(id(store, "old.lrc"), original);
    EXPECT_EQ(scalar(store, "SELECT length(file_index) FROM files WHERE available=1"), 16);
    EXPECT_EQ(scalar(store, "SELECT rating FROM stats"), 5);
}

TEST_F(LibraryIdentityTest, MonitorFingerprintsInTheBackgroundAndHonorsPlaybackGate) {
    file("old.lrc");
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    scanner.addRoot(music);
    std::atomic<bool> playing{true};
    LibraryMonitor::Options options;
    options.playbackActive = [&] { return playing.load(); };
    LibraryMonitor monitor(store, options);
    auto wait = [&](const std::function<bool()>& check) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < end) {
            if (check()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    };
    ASSERT_TRUE(wait([&] { return scalar(store, "SELECT COUNT(*) FROM files WHERE available=1") == 1; }));
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM files WHERE content_hash IS NOT NULL"), 0);
    playing = false;
    EXPECT_TRUE(wait([&] { return scalar(store, "SELECT COUNT(*) FROM files WHERE content_hash IS NOT NULL") == 1; }));
}

TEST_F(LibraryIdentityTest, MonitorRetriesDeferredMoveMatchingAfterPlaybackWithoutPolling) {
    const auto source = file("old.lrc");
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(music);
    ASSERT_TRUE(scanner.scan(root).complete);
    const auto original = id(store, "old.lrc");
    rate(store, original);
    ASSERT_EQ(scanner.fingerprint(root).hashed, 1);
    distrustIds(store);
    fs::rename(source, music / "new.lrc");
    std::atomic<bool> playing = true;
    LibraryScanner gated(store, [&] { return playing.load(); });
    const auto deferred = gated.scan(root);
    ASSERT_TRUE(deferred.deferred);
    ASSERT_FALSE(deferred.complete);
    EXPECT_EQ(id(store, "old.lrc"), original);
    LibraryMonitor::Options options;
    options.pollInterval = std::chrono::milliseconds(0);
    options.playbackActive = [&] { return playing.load(); };
    store.write([](sqlite::Database& db) { db.exec("UPDATE roots SET online=0"); }).get();
    LibraryMonitor monitor(store, options);
    const auto scanned = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!scalar(store, "SELECT online FROM roots") && std::chrono::steady_clock::now() < scanned)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ASSERT_EQ(scalar(store, "SELECT online FROM roots"), 1);
    EXPECT_EQ(id(store, "new.lrc"), 0);
    playing = false;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (id(store, "new.lrc") != original && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(id(store, "new.lrc"), original);
    EXPECT_EQ(scalar(store, "SELECT rating FROM stats"), 5);
}

} // namespace
