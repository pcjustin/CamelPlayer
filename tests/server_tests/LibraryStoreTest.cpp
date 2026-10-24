// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// library.db (docs/Plan.md §7.6, M3-R01): schema migrations, identity rules, the single writer, migrations with backup and
// restore, recovery from a damaged file, and refusal of network locations.
#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "Server/Library/LibraryStore.h"
#include "TestUtil.h"

using namespace camel::server;
using camel::server::sqlite::Database;

namespace {

std::filesystem::path freshDir(const char* name) {
    const auto dir = testutil::tempDir() / "library" / name;
    std::filesystem::remove_all(dir);
    return dir;
}

std::set<std::string> tables(Database& db) {
    std::set<std::string> out;
    auto s = db.prepare("SELECT name FROM sqlite_master WHERE type IN ('table') AND name NOT LIKE 'sqlite_%'"
                        " AND name NOT LIKE 'tracks_fts_%'");
    while (s.step()) out.insert(s.text(0));
    return out;
}

// One root, folder, audio file and whole-file track; returns the file identity.
int64_t addTrack(Database& db, const std::string& path, const std::string& title) {
    db.exec("INSERT OR IGNORE INTO roots (id, path, added) VALUES (1, 'C:/music', 0)");
    db.exec("INSERT OR IGNORE INTO folders (id, root_id, path) VALUES (1, 1, 'C:/music/album')");
    db.prepare("INSERT INTO files (folder_id, root_id, path, kind, size, mtime) VALUES (1, 1, ?, 'audio', 100, 1)")
        .bind(1, path)
        .run();
    const int64_t file = db.lastInsertId();
    db.prepare("INSERT INTO tracks (file_id, title) VALUES (?, ?)").bind(1, file).bind(2, title).run();
    return file;
}

std::vector<Migration> withExtra(std::vector<Migration> extra) {
    std::vector<Migration> all = libraryMigrations();
    all.insert(all.end(), extra.begin(), extra.end());
    return all;
}

Migration addTable(const std::string& name) {
    return [name](Database& db) { db.exec("CREATE TABLE " + name + " (x INTEGER)"); };
}

size_t backupCount(const std::filesystem::path& dir) {
    size_t n = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir))
        if (e.path().extension() == ".bak") n++;
    return n;
}

} // namespace

TEST(LibraryStore, CreatesCurrentSchemaWithWalAndForeignKeys) {
    const auto dir = freshDir("create");
    LibraryStore store(dir);
    EXPECT_TRUE(store.report().created);
    EXPECT_EQ(store.report().migratedFrom, 0);
    auto db = store.openReader();
    EXPECT_EQ(db.userVersion(), 5);
    EXPECT_EQ(tables(db),
              (std::set<std::string>{"albums", "app_state", "change_log", "covers", "files", "folders", "library_revision", "playlist_items",
                                     "playlists", "queue", "roots", "stats", "track_values", "tracks", "tracks_fts"}));
    auto mode = db.prepare("PRAGMA journal_mode");
    ASSERT_TRUE(mode.step());
    EXPECT_EQ(mode.text(0), "wal");
    store
        .write([](Database& w) {
            EXPECT_EQ(w.scalar("PRAGMA foreign_keys"), 1);
            w.exec(
                "INSERT INTO tracks_fts (rowid, title_norm, album_norm, artist_norm) VALUES (1, 'kind of blue', 'x', 'miles')");
            EXPECT_EQ(w.scalar("SELECT rowid FROM tracks_fts WHERE tracks_fts MATCH 'blu'"), 1); // trigram
        })
        .get();
}

TEST(LibraryStore, IdentityRulesHold) {
    LibraryStore store(freshDir("identity"));
    store
        .write([](Database& db) {
            const int64_t file = addTrack(db, "C:/music/album/01.flac", "One");
            db.prepare("INSERT INTO stats (file_id, cue_index, play_count, rating) VALUES (?, 0, 3, 5)").bind(1, file).run();
            // A rescan replaces the track row; statistics stay with the identity.
            db.exec("DELETE FROM tracks");
            EXPECT_EQ(db.scalar("SELECT play_count FROM stats WHERE cue_index = 0"), 3);
            // One available file per path; an unavailable identity may keep the old path.
            EXPECT_THROW(addTrack(db, "C:/music/album/01.flac", "Again"), sqlite::Error);
            db.prepare("UPDATE files SET available = 0 WHERE id = ?").bind(1, file).run();
            EXPECT_NO_THROW(addTrack(db, "C:/music/album/01.flac", "New identity"));
            EXPECT_EQ(db.scalar("SELECT COUNT(*) FROM files WHERE path = 'C:/music/album/01.flac'"), 2);
            // Rows cannot point at identities that do not exist, and referenced identities cannot vanish.
            EXPECT_THROW(db.exec("INSERT INTO tracks (file_id) VALUES (999)"), sqlite::Error);
            EXPECT_THROW(db.prepare("DELETE FROM files WHERE id = ?").bind(1, file).run(), sqlite::Error);
            EXPECT_THROW(db.exec("INSERT INTO stats (file_id, rating) VALUES (1, 6)"), sqlite::Error);
        })
        .get();
}

TEST(LibraryStore, UpgradesV1WithoutInventingFingerprintsOrChangingHistory) {
    const auto dir = freshDir("upgrade-v1");
    {
        LibraryStore store(dir, {libraryMigrations().front()});
        store
            .write([](Database& db) {
                const auto id = addTrack(db, "C:/music/old.flac", "Kept");
                db.prepare("INSERT INTO stats(file_id,rating,play_count) VALUES(?,5,17)").bind(1, id).run();
            })
            .get();
    }
    LibraryStore store(dir);
    EXPECT_EQ(store.report().migratedFrom, 1);
    EXPECT_TRUE(std::filesystem::exists(dir / "library.db.v1.bak"));
    auto db = store.openReader();
    EXPECT_EQ(db.userVersion(), 5);
    EXPECT_EQ(db.scalar("SELECT rating FROM stats JOIN files ON files.id=stats.file_id WHERE files.path='C:/music/old.flac'"), 5);
    EXPECT_EQ(db.scalar("SELECT play_count FROM stats"), 17);
    EXPECT_EQ(db.scalar("SELECT COUNT(*) FROM files WHERE content_hash IS NULL AND created=0 AND volume_key IS NULL"), 1);
}

TEST(LibraryStore, UpgradesV2WithCueTimelineAndVisibleTrackView) {
    const auto dir = freshDir("upgrade-v2");
    {
        LibraryStore store(dir, {libraryMigrations()[0], libraryMigrations()[1]});
        store.write([](Database& db) { addTrack(db, "C:/music/old.flac", "Kept"); }).get();
    }
    LibraryStore store(dir);
    EXPECT_EQ(store.report().migratedFrom, 2);
    auto db = store.openReader();
    EXPECT_EQ(db.userVersion(), 5);
    EXPECT_EQ(db.scalar("SELECT cue_pregap_75ths + cue_postgap_75ths FROM tracks"), 0);
    EXPECT_EQ(db.scalar("SELECT COUNT(*) FROM library_tracks"), 1);
}

TEST(LibraryStore, WriterRunsEachJobInOneTransaction) {
    LibraryStore store(freshDir("writer"));
    auto failed = store.write([](Database& db) {
        db.exec("INSERT INTO app_state (key, value) VALUES ('kept', 'no')");
        throw std::runtime_error("job failed");
    });
    EXPECT_THROW(failed.get(), std::runtime_error);
    std::vector<std::thread> threads;
    std::vector<std::future<void>> jobs(8);
    for (int t = 0; t < 8; t++)
        threads.emplace_back([&, t] {
            jobs[size_t(t)] = store.write([t](Database& db) {
                for (int i = 0; i < 50; i++)
                    db.prepare("INSERT INTO app_state (key, value) VALUES (?, 'v')").bind(1, std::to_string(t * 100 + i)).run();
            });
        });
    for (auto& t : threads) t.join();
    for (auto& j : jobs) j.get();
    auto reader = store.openReader();
    EXPECT_EQ(reader.scalar("SELECT COUNT(*) FROM app_state"), 400);
    EXPECT_EQ(reader.scalar("SELECT COUNT(*) FROM app_state WHERE key = 'kept'"), 0);
    EXPECT_THROW(reader.exec("INSERT INTO app_state (key, value) VALUES ('ro', 'x')"), sqlite::Error);
}

TEST(LibraryStore, MigrationBacksUpFirstAndAFailureKeepsThePreviousDatabase) {
    const auto dir = freshDir("migrate");
    {
        LibraryStore store(dir);
        store.write([](Database& db) { addTrack(db, "C:/music/album/01.flac", "Kept"); }).get();
    }
    {
        LibraryStore store(dir, withExtra({addTable("v2_table")}));
        EXPECT_EQ(store.report().migratedFrom, 5);
        auto db = store.openReader();
        EXPECT_EQ(db.userVersion(), 6);
        EXPECT_TRUE(std::filesystem::exists(dir / "library.db.v5.bak"));
    }
    const auto broken = withExtra({addTable("v2_table"), [](Database& db) {
                                       db.exec("CREATE TABLE half (x INTEGER)");
                                       throw std::runtime_error("broken migration");
                                   }});
    EXPECT_THROW((LibraryStore(dir, broken)), MigrationError);
    {
        LibraryStore store(dir, withExtra({addTable("v2_table")}));
        auto db = store.openReader();
        EXPECT_EQ(db.userVersion(), 6);
        EXPECT_EQ(db.scalar("SELECT COUNT(*) FROM tracks WHERE title = 'Kept'"), 1);
        EXPECT_EQ(db.scalar("SELECT COUNT(*) FROM sqlite_master WHERE name = 'half'"), 0);
    }
    EXPECT_THROW((LibraryStore(dir)), MigrationError); // the extra migration is newer than the production schema
}

TEST(LibraryStore, KeepsTheNewestThreeBackups) {
    const auto dir = freshDir("rotate");
    {
        LibraryStore store(dir);
    }
    std::vector<Migration> extra;
    for (int v = 5; v <= 9; v++) {
        extra.push_back(addTable("t" + std::to_string(v)));
        LibraryStore store(dir, withExtra(extra));
    }
    EXPECT_EQ(backupCount(dir), size_t(LibraryStore::kKeptBackups));
    for (int v : {7, 8, 9}) EXPECT_TRUE(std::filesystem::exists(dir / ("library.db.v" + std::to_string(v) + ".bak"))) << v;
}

TEST(LibraryStore, ADamagedFileIsKeptAsideAndReplacedByTheNewestBackup) {
    const auto dir = freshDir("damaged");
    {
        LibraryStore store(dir);
        store.write([](Database& db) { addTrack(db, "C:/music/album/01.flac", "From backup"); }).get();
    }
    {
        LibraryStore store(dir, withExtra({addTable("v2_table")}));
    } // writes library.db.v5.bak
    std::filesystem::remove(dir / "library.db-wal");
    std::ofstream(dir / "library.db", std::ios::binary | std::ios::trunc) << std::string(8192, 'x');
    {
        LibraryStore store(dir, withExtra({addTable("v2_table")}));
        EXPECT_TRUE(store.report().damaged);
        EXPECT_TRUE(store.report().restoredBackup);
        EXPECT_TRUE(std::filesystem::exists(dir / "library.db.bad"));
        auto db = store.openReader();
        EXPECT_EQ(db.scalar("SELECT COUNT(*) FROM tracks WHERE title = 'From backup'"), 1);
        EXPECT_EQ(db.userVersion(), 6); // the backup was migrated again
    }
    for (const auto& e : std::filesystem::directory_iterator(dir))
        if (e.path().extension() == ".bak") std::filesystem::remove(e.path());
    std::ofstream(dir / "library.db", std::ios::binary | std::ios::trunc) << std::string(8192, 'y');
    LibraryStore store(dir);
    EXPECT_TRUE(store.report().damaged);
    EXPECT_FALSE(store.report().restoredBackup);
    EXPECT_EQ(store.openReader().scalar("SELECT COUNT(*) FROM tracks"), 0);
}

TEST(LibraryStore, RefusesNetworkLocations) {
    EXPECT_TRUE(isNetworkPath(L"\\\\nas\\music\\camelplayer"));
    EXPECT_TRUE(isNetworkPath(L"\\\\?\\UNC\\nas\\music\\camelplayer"));
    EXPECT_FALSE(isNetworkPath(testutil::tempDir()));
    EXPECT_THROW(LibraryStore(L"\\\\nas\\music\\camelplayer"), std::runtime_error);
}
