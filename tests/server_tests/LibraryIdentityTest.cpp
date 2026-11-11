// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>
#include <windows.h>

#include <atomic>
#include <fstream>
#include <future>
#include <optional>
#include <thread>

#include "Server/Library/LibraryIdentity.h"
#include "Server/Library/LibraryScanner.h"
#include "Server/Library/LibraryViews.h"
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

TEST_F(LibraryIdentityTest, LocalRenamePreservesTracksStatsPlaylistAndQueue) {
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
    EXPECT_EQ(scalar(store, "SELECT id FROM tracks"), track);
    EXPECT_EQ(scalar(store, "SELECT play_count FROM stats"), 17);
    EXPECT_EQ(scalar(store, "SELECT favorite FROM stats"), 1);
    EXPECT_EQ(scalar(store, "SELECT last_played FROM stats"), 123);
    EXPECT_EQ(scalar(store, "SELECT file_id FROM playlist_items"), original);
    EXPECT_EQ(scalar(store, "SELECT file_id FROM queue WHERE entry_id=77"), original);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM queue JOIN files ON files.id=queue.file_id WHERE queue.path=files.path"), 1);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM tracks"), 1);
}

// The record a rename keeps names the file as it is now, as a playlist exported from it does.
TEST_F(LibraryIdentityTest, ARenamedFileCarriesItsNewName) {
    const auto source = music / "old.flac";
    fs::copy_file(testutil::testFile("16-44.1k"), source);
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(music);
    ASSERT_TRUE(scanner.scan(root).complete);
    const auto original = id(store, "old.flac");
    fs::rename(source, music / "new.flac");
    ASSERT_EQ(scanner.scan(root).moved, 1);
    ASSERT_EQ(id(store, "new.flac"), original);
    auto db = store.openReader();
    auto name = db.prepare("SELECT name FROM files WHERE id=?");
    ASSERT_TRUE(name.bind(1, original).step());
    EXPECT_EQ(name.text(0), "new.flac");
    const auto stored = libraryStoredPath(db, original);
    ASSERT_TRUE(stored);
    EXPECT_TRUE(stored->ends_with("\\new.flac")) << *stored;
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
    ASSERT_EQ(scalar(store, "SELECT COUNT(*) FROM files WHERE content_hash IS NOT NULL"), 1);
    fs::rename(source, music / "moved.lrc");
    file("old.lrc", "xyz");
    fs::last_write_time(source, stamp);
    EXPECT_EQ(scanner.scan(root).moved, 1);
    EXPECT_EQ(id(store, "moved.lrc"), original);
    EXPECT_NE(id(store, "old.lrc"), original);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM stats"), 1);
}

// A tagger writes the file anew and renames it over the original: another file ID and other content at the same path.
TEST_F(LibraryIdentityTest, AFileRewrittenInPlaceKeepsItsHistory) {
    const auto song = music / "song.flac";
    fs::copy_file(testutil::testFile("16-44.1k"), song);
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(music);
    ASSERT_TRUE(scanner.scan(root).complete);
    const auto original = id(store, "song.flac");
    const auto before = library::fileVersion(song);
    rate(store, original);
    store
        .write([=](sqlite::Database& db) {
            db.exec("INSERT INTO playlists(id,name,created,modified) VALUES(1,'Kept',0,0)");
            db.prepare("INSERT INTO playlist_items(playlist_id,position,file_id,source_description) VALUES(1,0,?,'Kept')")
                .bind(1, original)
                .run();
        })
        .get();
    const auto written = music / "song.tmp";
    camel::audio::testaudio::writeFlac(written, {"retagged", 44100, 16, 2, 0.05});
    fs::rename(written, song);
    ASSERT_FALSE(library::fileVersion(song).sameFile(before));
    ASSERT_TRUE(scanner.scan(root).complete);
    EXPECT_EQ(id(store, "song.flac"), original);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM files WHERE available=1"), 1);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM tracks"), 1);
    EXPECT_EQ(scalar(store, "SELECT play_count FROM stats"), 17);
    EXPECT_EQ(scalar(store, "SELECT favorite FROM stats"), 1);
    EXPECT_EQ(scalar(store, "SELECT file_id FROM playlist_items"), original);
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
    ASSERT_EQ(scalar(store, "SELECT COUNT(*) FROM files WHERE content_hash IS NOT NULL"), 1);
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
    // As when the file was scanned during playback, which leaves its fingerprint to a later pass.
    store.write([](sqlite::Database& db) { db.exec("UPDATE files SET content_hash=NULL"); }).get();
    distrustIds(store);
    fs::rename(source, music / "new.lrc");
    EXPECT_EQ(scanner.scan(root).moved, 0);
    EXPECT_NE(id(store, "new.lrc"), original);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM files WHERE content_hash IS NOT NULL AND available=1"), 1);
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
    ASSERT_EQ(scalar(store, "SELECT COUNT(*) FROM files WHERE content_hash IS NOT NULL"), 1);
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
    ASSERT_EQ(scalar(store, "SELECT COUNT(*) FROM files WHERE content_hash IS NOT NULL"), 2);
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
    ASSERT_EQ(scalar(store, "SELECT COUNT(*) FROM files WHERE content_hash IS NOT NULL"), 1);
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
    ASSERT_EQ(scalar(store, "SELECT COUNT(*) FROM files WHERE content_hash IS NOT NULL"), 1);
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

TEST(BackgroundPace, WorkOnThePlayingStorageReadsOnlyWhilePlaybackIsAhead) {
    const fs::path share = L"\\\\TACHIKOMA\\Music";
    EXPECT_EQ(backgroundPace({}, share), BackgroundPace::Full);
    EXPECT_EQ(backgroundPace({{share / "a.flac"}, false}, share), BackgroundPace::Wait);
    EXPECT_EQ(backgroundPace({{share / "a.flac"}, true}, share), BackgroundPace::One);
    EXPECT_EQ(backgroundPace({{L"C:\\Music\\a.flac"}, false}, share), BackgroundPace::One);
    // The prepared next file counts like the playing one.
    EXPECT_EQ(backgroundPace({{L"C:\\Music\\a.flac", share / "b.flac"}, false}, share), BackgroundPace::Wait);
}

TEST_F(LibraryIdentityTest, AScanWaitingForPlaybackCompletesOnceItStops) {
    file("a.lrc");
    file("b.lrc", "b");
    LibraryStore store(dir / "db");
    std::atomic<bool> playing = true;
    LibraryScanner scanner(store, {}, [&] { return playing ? PlaybackReads{{music / "playing.flac"}, false} : PlaybackReads{}; });
    const auto root = scanner.addRoot(music);
    auto scan = std::async(std::launch::async, [&] { return scanner.scan(root); });
    EXPECT_EQ(scan.wait_for(std::chrono::milliseconds(500)), std::future_status::timeout);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM files"), 0);
    playing = false;
    EXPECT_TRUE(scan.get().complete);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM files WHERE available=1"), 2);
}

TEST_F(LibraryIdentityTest, PlaybackSourceGatesOnlyTheSameStorage) {
    const auto source = file("old.lrc");
    PlaybackReads playing{{source}, true};
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store, {}, [&] { return playing; });
    const auto root = scanner.addRoot(music);
    ASSERT_TRUE(scanner.scan(root).complete);
    playing.ahead = false;
    EXPECT_FALSE(scanner.fingerprint(root).complete);
    playing.files = {L"\\\\TACHIKOMA\\Music\\track.flac"};
    EXPECT_EQ(scanner.fingerprint(root).hashed, 1);
    playing = {{source}, true};
    file("second.lrc", "second bytes");
    ASSERT_TRUE(scanner.scan(root).complete);
    playing.ahead = false;
    EXPECT_FALSE(scanner.fingerprint(root).complete);
    playing = {};
    EXPECT_EQ(scanner.fingerprint(root).hashed, 1);
    EXPECT_EQ(library::storageKey(source), library::storageKey(music));
    EXPECT_FALSE(library::storageKey(source).empty());
    EXPECT_EQ(library::storageKey(L"\\\\TACHIKOMA\\Music\\album\\track.flac"), L"\\\\tachikoma\\music");
    EXPECT_EQ(library::storageKey(L"\\\\?\\UNC\\TACHIKOMA\\Music"), L"\\\\tachikoma\\music");
    EXPECT_NE(library::storageKey(L"\\\\TACHIKOMA\\Other\\track.flac"), L"\\\\tachikoma\\music");
}

TEST_F(LibraryIdentityTest, AFingerprintPassWithNothingLeftIsCompleteWhenPlaybackFallsBehind) {
    const auto source = file("a.lrc");
    LibraryStore store(dir / "db");
    std::atomic<bool> playing = false;
    std::atomic<int> calls = 0;
    LibraryScanner scanner(store, {}, [&] { return playing ? PlaybackReads{{source}, calls++ == 0} : PlaybackReads{}; });
    const auto root = scanner.addRoot(music);
    ASSERT_TRUE(scanner.scan(root).complete);
    ASSERT_TRUE(scanner.fingerprint(root).complete);
    playing = true;
    EXPECT_TRUE(scanner.fingerprint(root).complete);
}

TEST_F(LibraryIdentityTest, ARemovedRootLeavesNoRowsAndComesBackAsNew) {
    fs::copy_file(testutil::testFile("16-44.1k"), music / "gone.flac");
    file("gone.lrc");
    const auto other = dir / "other";
    fs::create_directories(other);
    fs::copy_file(testutil::testFile("16-44.1k"), other / "kept.flac");
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(music);
    const auto otherRoot = scanner.addRoot(other);
    ASSERT_TRUE(scanner.scan(root).complete);
    ASSERT_TRUE(scanner.scan(otherRoot).complete);
    const auto gone = id(store, "gone.flac");
    const auto kept = id(store, "kept.flac");
    rate(store, gone);
    rate(store, kept);
    store
        .write([=](sqlite::Database& db) {
            db.exec("INSERT INTO playlists(id,name,created,modified) VALUES(1,'Mixed',0,0);"
                    "INSERT INTO favorite_names(field,name,added) VALUES('artist','Kept by name',1)");
            db.prepare("INSERT INTO playlist_items(playlist_id,position,file_id,source_description) "
                       "VALUES(1,0,?1,'a'),(1,1,?2,'b'),(1,2,?1,'c')")
                .bind(1, gone)
                .bind(2, kept)
                .run();
            db.prepare("INSERT INTO queue(position,entry_id,file_id,path,source_description) VALUES(0,77,?,'gone','Gone')")
                .bind(1, gone)
                .run();
            db.prepare("INSERT INTO play_events(file_id,started,utc_offset,heard_ms,counted) VALUES(?1,1,0,1,1),(?2,1,0,1,1)")
                .bind(1, gone)
                .bind(2, kept)
                .run();
        })
        .get();
    const auto contents = libraryRootContents(store, root);
    EXPECT_EQ(contents.tracks, 1);
    EXPECT_EQ(contents.plays, 17);
    EXPECT_EQ(contents.favorites, 1);
    EXPECT_EQ(contents.playlistEntries, 2);

    scanner.removeRoot(root).get();
    EXPECT_THROW(scanner.removeRoot(root), std::invalid_argument);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM roots"), 1);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM files WHERE root_id NOT IN (SELECT id FROM roots)"), 0);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM folders WHERE root_id NOT IN (SELECT id FROM roots)"), 0);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM tracks"), 1);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM albums"), 1);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM track_tags WHERE track_id NOT IN (SELECT id FROM tracks)"), 0);
    EXPECT_EQ(scalar(store, "SELECT file_id FROM stats"), kept);
    EXPECT_EQ(scalar(store, "SELECT file_id FROM play_events"), kept);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM queue"), 0);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM playlist_items"), 1);
    EXPECT_EQ(scalar(store, "SELECT position FROM playlist_items"), 0);
    EXPECT_EQ(scalar(store, "SELECT revision FROM playlists"), 1);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM favorite_names"), 1);
    const auto rootState = "SELECT COUNT(*) FROM app_state WHERE key LIKE 'library_%:" + std::to_string(root) + "'";
    EXPECT_EQ(scalar(store, rootState.c_str()), 0);
    EXPECT_EQ(scalar(store, "SELECT value FROM app_state WHERE key='library_deletions'"), 1);
    EXPECT_FALSE(store.openReader().prepare("PRAGMA foreign_key_check").step());
    EXPECT_THROW(scanner.scan(root), std::invalid_argument);
    EXPECT_FALSE(scanner.fingerprint(root).complete);

    const auto again = scanner.addRoot(music);
    EXPECT_NE(again, root);
    ASSERT_TRUE(scanner.scan(again).complete);
    EXPECT_NE(id(store, "gone.flac"), gone);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM tracks"), 2);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM stats"), 1);
}

TEST_F(LibraryIdentityTest, TheDatabaseShrinksWhenALargeRootIsRemoved) {
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(music);
    store
        .write([root](sqlite::Database& db) {
            auto insert = db.prepare("INSERT INTO files(root_id,path,kind,size,mtime,name) VALUES(?,?,'lrc',1,1,?)");
            const std::string name(200, 'x');
            for (int i = 0; i < 20000; ++i) {
                insert.reset();
                insert.bind(1, root).bind(2, "c:\\music\\" + std::to_string(i)).bind(3, name).run();
            }
        })
        .get();
    store.compact().get();
    const auto before = fs::file_size(dir / "db" / "library.db");
    scanner.removeRoot(root).get();
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM files"), 0);
    EXPECT_LT(fs::file_size(dir / "db" / "library.db"), before / 4);
}

TEST_F(LibraryIdentityTest, FoldersAnEarlierVersionRemovedAreDeletedOnce) {
    file("old.lrc");
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(music);
    ASSERT_TRUE(scanner.scan(root).complete);
    rate(store, id(store, "old.lrc"));
    store.write([](sqlite::Database& db) { db.exec("UPDATE roots SET removed=1,online=0; UPDATE files SET available=0"); }).get();
    scanner.deleteRemovedRoots().get();
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM roots"), 0);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM files"), 0);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM stats"), 0);
    scanner.deleteRemovedRoots().get();
    EXPECT_EQ(scalar(store, "SELECT value FROM app_state WHERE key='library_deletions'"), 1);
}

TEST_F(LibraryIdentityTest, AFileMissingForThirtyDaysGoesAfterACompleteScanOfItsReachableFolder) {
    const auto gone = file("gone.lrc", "gone");
    file("kept.lrc", "kept");
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(music);
    ASSERT_TRUE(scanner.scan(root).complete);
    rate(store, id(store, "gone.lrc"));
    fs::remove(gone);
    ASSERT_TRUE(scanner.scan(root).complete);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM files WHERE available=0 AND missing_since IS NOT NULL"), 1);
    const auto age = [&](int days) {
        store
            .write([days](sqlite::Database& db) {
                db.prepare("UPDATE files SET missing_since=missing_since-?").bind(1, int64_t(days) * 86400).run();
            })
            .get();
    };
    age(29);
    ASSERT_TRUE(scanner.scan(root).complete);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM stats"), 1);
    age(2);
    fs::rename(music, dir / "away");
    EXPECT_FALSE(scanner.scan(root).complete);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM files"), 2);
    fs::rename(dir / "away", music);
    const auto report = scanner.scan(root);
    ASSERT_TRUE(report.complete);
    EXPECT_EQ(report.deleted, 1u);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM files"), 1);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM stats"), 0);
    EXPECT_NE(id(store, "kept.lrc"), 0);
}

TEST_F(LibraryIdentityTest, AFileThatComesBackIsNoLongerMissing) {
    file("back.lrc");
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    ASSERT_TRUE(scanner.scan(scanner.addRoot(music)).complete);
    store.write([](sqlite::Database& db) { db.exec("UPDATE files SET available=0,missing_since=1"); }).get();
    store.write([](sqlite::Database& db) { db.exec("UPDATE files SET available=1"); }).get();
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM files WHERE missing_since IS NULL"), 1);
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
        if (first.exchange(false)) {
            entered.set_value();
            released.wait();
        }
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
    // Playback of a file beside it, ahead or not.
    std::optional<bool> ahead = true;
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store, {}, [&] { return ahead ? PlaybackReads{{music / "playing.flac"}, *ahead} : PlaybackReads{}; });
    const auto root = scanner.addRoot(music);
    ASSERT_TRUE(scanner.scan(root).complete);
    ahead = false;
    EXPECT_FALSE(scanner.fingerprint(root).complete);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM files WHERE content_hash IS NOT NULL"), 0);
    ahead.reset();
    CancellationToken cancelled;
    cancelled.cancel();
    EXPECT_FALSE(scanner.fingerprint(root, cancelled).complete);
    const auto expected = library::fileVersion(source);
    const auto digest = library::fileDigest(source, expected, {}, {}, library::Fingerprint::Full);
    ASSERT_TRUE(digest);
    const std::vector<uint8_t> sha256{0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
                                      0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
                                      0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    EXPECT_EQ(*digest, sha256);
    EXPECT_EQ(scanner.fingerprint(root).hashed, 1);
    file("old.lrc", "xyz");
    fs::last_write_time(source, fs::last_write_time(source) + std::chrono::seconds(2));
    EXPECT_THROW(library::fileDigest(source, expected, {}, {}), std::runtime_error);
    ahead = true;
    EXPECT_TRUE(scanner.scan(root).complete);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM files WHERE content_hash IS NOT NULL"), 0);
    ahead.reset();
    EXPECT_EQ(scanner.fingerprint(root).hashed, 1);
}

TEST_F(LibraryIdentityTest, FingerprintsPendingFilesAfterAnIncompleteScan) {
    file("old.lrc", "abc");
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(music);
    ASSERT_TRUE(scanner.scan(root).complete);
    store.write([](sqlite::Database& db) { db.exec("UPDATE files SET identity_state=1,content_hash=NULL WHERE available=1"); })
        .get();

    const auto report = scanner.fingerprint(root);
    EXPECT_TRUE(report.complete);
    EXPECT_EQ(report.hashed, 1);
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM files WHERE content_hash IS NOT NULL"), 1);
    EXPECT_EQ(scalar(store, "SELECT identity_state FROM files WHERE available=1"), 1);
}

TEST_F(LibraryIdentityTest, HashingStopsBetweenChunksWithoutCommittingAPartialDigest) {
    const auto source = file("large.lrc", std::string(3 << 20, 'x'));
    const auto expected = library::fileVersion(source);
    CancellationToken token;
    int checks = 0;
    constexpr auto full = library::Fingerprint::Full;
    EXPECT_FALSE(library::fileDigest(source, expected, token, [&] { return ++checks >= 3; }, full));
    checks = 0;
    EXPECT_FALSE(library::fileDigest(
        source, expected, token,
        [&] {
            if (++checks == 3) token.cancel();
            return false;
        },
        full));
    EXPECT_THROW(library::fileDigest(
                     source, expected, {},
                     [&] {
                         if (++checks == 5) {
                             file("large.lrc", "changed");
                             fs::last_write_time(source, fs::last_write_time(source) + std::chrono::seconds(2));
                         }
                         return false;
                     },
                     full),
                 std::runtime_error);
}

TEST_F(LibraryIdentityTest, SampledFingerprintReadsOnlyTheHeadAndTail) {
    std::string bytes(3 << 20, 'x');
    const auto source = file("large.lrc", bytes);
    const auto sampled = [&] {
        IO_COUNTERS before{}, after{};
        GetProcessIoCounters(GetCurrentProcess(), &before);
        auto digest = library::fileDigest(source, library::fileVersion(source), {}, {});
        GetProcessIoCounters(GetCurrentProcess(), &after);
        EXPECT_LE(after.ReadTransferCount - before.ReadTransferCount, 192u * 1024);
        return digest.value();
    };
    const auto original = sampled();
    ASSERT_EQ(original.size(), 33u);
    EXPECT_EQ(original[0], 1);
    bytes[bytes.size() / 2] = 'y';
    file("large.lrc", bytes);
    EXPECT_EQ(sampled(), original); // the unread middle
    bytes[bytes.size() / 2] = 'x';
    bytes.front() = 'y';
    file("large.lrc", bytes);
    EXPECT_NE(sampled(), original);
    bytes.front() = 'x';
    bytes.back() = 'y';
    file("large.lrc", bytes);
    EXPECT_NE(sampled(), original);
    bytes.back() = 'x';
    bytes.push_back('x');
    file("large.lrc", bytes);
    EXPECT_NE(sampled(), original);
}

TEST_F(LibraryIdentityTest, AFullFingerprintRecordedEarlierStillMatchesAMove) {
    const auto source = file("old.lrc");
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(music);
    ASSERT_TRUE(scanner.scan(root).complete);
    const auto original = id(store, "old.lrc");
    rate(store, original);
    const auto full = library::fileDigest(source, library::fileVersion(source), {}, {}, library::Fingerprint::Full).value();
    store.write([&](sqlite::Database& db) { db.prepare("UPDATE files SET content_hash=?").bind(1, full).run(); }).get();
    distrustIds(store);
    fs::copy_file(source, music / "new.lrc");
    fs::remove(source);
    EXPECT_EQ(scanner.scan(root).moved, 1);
    EXPECT_EQ(id(store, "new.lrc"), original);
    EXPECT_EQ(scalar(store, "SELECT rating FROM stats"), 5);
}

TEST_F(LibraryIdentityTest, PresentFilesTradeAFullFingerprintForASampledOne) {
    const auto source = file("old.lrc");
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    const auto root = scanner.addRoot(music);
    ASSERT_TRUE(scanner.scan(root).complete);
    const auto full = library::fileDigest(source, library::fileVersion(source), {}, {}, library::Fingerprint::Full).value();
    store.write([&](sqlite::Database& db) { db.prepare("UPDATE files SET content_hash=?").bind(1, full).run(); }).get();
    EXPECT_EQ(scanner.fingerprint(root).hashed, 1);
    EXPECT_EQ(scalar(store, "SELECT length(content_hash) FROM files"), 33);
    EXPECT_EQ(scanner.fingerprint(root).hashed, 0);
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
    // A folder that cannot be listed leaves the scan incomplete.
    const auto held = music / "held";
    fs::create_directory(held);
    fs::copy_file(testutil::testFile("16-44.1k"), held / "blocked.flac");
    HANDLE handle = CreateFileW(held.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
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
    // A folder that cannot be listed leaves the scan incomplete.
    const auto held = music / "held";
    fs::create_directory(held);
    fs::copy_file(testutil::testFile("16-44.1k"), held / "blocked.flac");
    HANDLE handle = CreateFileW(held.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
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

TEST_F(LibraryIdentityTest, MonitorScansOnlyWhilePlaybackIsAheadAndFingerprintsAfter) {
    file("old.lrc");
    LibraryStore store(dir / "db");
    LibraryScanner scanner(store);
    scanner.addRoot(music);
    // A file beside it plays, first with the engine short of audio, then ahead, then it stops.
    std::atomic<int> playing{1};
    LibraryMonitor::Options options;
    options.playback = [&] {
        return playing.load() ? PlaybackReads{{music / "playing.flac"}, playing.load() == 2} : PlaybackReads{};
    };
    LibraryMonitor monitor(store, options);
    auto wait = [&](const std::function<bool()>& check) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < end) {
            if (check()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    };
    std::this_thread::sleep_for(std::chrono::seconds(1));
    EXPECT_EQ(scalar(store, "SELECT COUNT(*) FROM files"), 0);
    playing = 2;
    ASSERT_TRUE(wait([&] { return scalar(store, "SELECT COUNT(*) FROM files WHERE available=1") == 1; }));
    playing = 0;
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
    ASSERT_EQ(scalar(store, "SELECT COUNT(*) FROM files WHERE content_hash IS NOT NULL"), 1);
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
    options.coversShown = [&] { return playing.load(); };
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
