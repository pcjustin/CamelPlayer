// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// library.db (docs/Plan.md §7.6, M3-R01): schema migrations, identity rules, the single writer, migrations with backup and
// restore, recovery from a damaged file, and refusal of network locations.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
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
    EXPECT_EQ(db.userVersion(), 16);
    EXPECT_EQ(tables(db),
              (std::set<std::string>{"albums", "app_state", "change_log", "covers", "favorite_names", "files", "folders",
                                     "library_revision", "play_events", "playlist_items", "playlists", "queue", "roots", "stats",
                                     "track_tags", "track_values", "tracks", "tracks_fts"}));
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
            db.prepare("INSERT INTO stats (file_id, play_count, rating) VALUES (?, 3, 5)").bind(1, file).run();
            // A rescan replaces the track row; statistics stay with the identity.
            db.exec("DELETE FROM tracks");
            EXPECT_EQ(db.scalar("SELECT play_count FROM stats WHERE file_id = " + std::to_string(file)), 3);
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
    EXPECT_EQ(db.userVersion(), 16);
    EXPECT_EQ(db.scalar("SELECT rating FROM stats JOIN files ON files.id=stats.file_id WHERE files.path='C:/music/old.flac'"), 5);
    EXPECT_EQ(db.scalar("SELECT play_count FROM stats"), 17);
    EXPECT_EQ(db.scalar("SELECT COUNT(*) FROM files WHERE content_hash IS NULL AND created=0 AND volume_key IS NULL"), 1);
}

TEST(LibraryStore, UpgradesV11ByDroppingTheCueIndexAndKeepingEveryRow) {
    const auto dir = freshDir("upgrade-v11");
    {
        LibraryStore store(dir, std::vector<Migration>(libraryMigrations().begin(), libraryMigrations().begin() + 11));
        store
            .write([](Database& db) {
                const auto file = addTrack(db, "C:/music/kept.flac", "Kept");
                const auto other = addTrack(db, "C:/music/other.flac", "Other");
                db.exec("INSERT INTO track_values(track_id,field,position,value) SELECT id,'artist',0,'Kept artist' FROM tracks "
                        "WHERE title='Kept'");
                db.prepare(
                      "INSERT INTO stats(file_id,cue_index,play_count,rating,listened_ms) VALUES(?,0,3,5,1000),(?,0,1,NULL,10)")
                    .bind(1, file)
                    .bind(2, other)
                    .run();
                db.prepare(
                      "INSERT INTO play_events(file_id,cue_index,started,utc_offset,heard_ms,counted) VALUES(?,0,1,0,1000,1)")
                    .bind(1, file)
                    .run();
                db.exec("INSERT INTO playlists(id,name,created,modified,revision) VALUES(1,'Kept',0,0,1)");
                db.prepare(
                      "INSERT INTO playlist_items(playlist_id,position,file_id,cue_index,source_description) VALUES(1,0,?,0,'k')")
                    .bind(1, file)
                    .run();
                db.prepare(
                      "INSERT INTO queue(position,entry_id,file_id,cue_index,path,source_description) VALUES(0,1,?,0,'p','k')")
                    .bind(1, file)
                    .run();
            })
            .get();
    }
    LibraryStore store(dir);
    EXPECT_EQ(store.report().migratedFrom, 11);
    auto db = store.openReader();
    EXPECT_EQ(db.userVersion(), 16);
    for (const char* table : {"tracks", "stats", "play_events", "playlist_items", "queue"})
        EXPECT_EQ(db.scalar(std::string("SELECT COUNT(*) FROM pragma_table_info('") + table + "') WHERE name LIKE 'cue%'"), 0)
            << table;
    EXPECT_EQ(db.scalar("SELECT COUNT(*) FROM sqlite_master WHERE name='library_tracks'"), 0);
    EXPECT_EQ(db.scalar("SELECT COUNT(*) FROM tracks"), 2);
    EXPECT_EQ(db.scalar("SELECT COUNT(*) FROM track_values"), 1); // a rebuilt parent table cascades into no child
    EXPECT_EQ(db.scalar("SELECT rowid FROM tracks_fts WHERE tracks_fts MATCH 'kept'"),
              db.scalar("SELECT id FROM tracks WHERE title='Kept'"));
    EXPECT_EQ(db.scalar("SELECT play_count FROM stats JOIN files f ON f.id=stats.file_id WHERE f.path='C:/music/kept.flac'"), 3);
    EXPECT_EQ(db.scalar("SELECT SUM(listened_ms) FROM stats"), 1010);
    EXPECT_EQ(db.scalar("SELECT COUNT(*) FROM play_events"), 1);
    EXPECT_EQ(db.scalar("SELECT COUNT(*) FROM playlist_items"), 1);
    EXPECT_EQ(db.scalar("SELECT COUNT(*) FROM queue"), 1);
    store
        .write([](Database& w) {
            EXPECT_EQ(w.scalar("PRAGMA foreign_keys"), 1);
            EXPECT_THROW(w.exec("INSERT INTO tracks(file_id) VALUES(1)"), sqlite::Error); // one track per file
            w.exec("UPDATE tracks SET title='Renamed' WHERE title='Kept'");               // the triggers came back
            EXPECT_EQ(w.scalar("SELECT COUNT(*) FROM tracks_fts WHERE tracks_fts MATCH 'renamed'"), 1);
        })
        .get();
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
    EXPECT_EQ(reader.scalar("SELECT COUNT(*) FROM app_state WHERE key<>'name_rules'"), 400);
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
        EXPECT_EQ(store.report().migratedFrom, 16);
        auto db = store.openReader();
        EXPECT_EQ(db.userVersion(), 17);
        EXPECT_TRUE(std::filesystem::exists(dir / "library.db.v16.bak"));
    }
    const auto broken = withExtra({addTable("v2_table"), [](Database& db) {
                                       db.exec("CREATE TABLE half (x INTEGER)");
                                       throw std::runtime_error("broken migration");
                                   }});
    EXPECT_THROW((LibraryStore(dir, broken)), MigrationError);
    {
        LibraryStore store(dir, withExtra({addTable("v2_table")}));
        auto db = store.openReader();
        EXPECT_EQ(db.userVersion(), 17);
        EXPECT_EQ(db.scalar("SELECT COUNT(*) FROM tracks WHERE title = 'Kept'"), 1);
        EXPECT_EQ(db.scalar("SELECT COUNT(*) FROM sqlite_master WHERE name = 'half'"), 0);
    }
    EXPECT_THROW((LibraryStore(dir)), NewerLibraryError); // the extra migration is newer than the production schema
}

TEST(LibraryStore, UpgradesV13ByDatingFilesByTheirCreationOnDisk) {
    const auto dir = freshDir("upgrade-v13");
    {
        LibraryStore store(dir, std::vector<Migration>(libraryMigrations().begin(), libraryMigrations().begin() + 13));
        store
            .write([](Database& db) {
                const auto file = addTrack(db, "C:/music/album/01.flac", "Created");
                addTrack(db, "C:/music/album/02.flac", "Unknown");
                // 2020-01-01T00:00:00Z as a FILETIME
                db.prepare("UPDATE files SET created=132223104000000000 WHERE id=?").bind(1, file).run();
            })
            .get();
    }
    const auto before =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    LibraryStore store(dir);
    auto db = store.openReader();
    EXPECT_EQ(db.scalar("SELECT added FROM files WHERE path='C:/music/album/01.flac'"), 1577836800);
    EXPECT_GE(db.scalar("SELECT added FROM files WHERE path='C:/music/album/02.flac'").value_or(0), before - 1);
    EXPECT_EQ(db.scalar("SELECT COUNT(*) FROM stats WHERE favorite_at IS NOT NULL OR album_favorite_at IS NOT NULL"), 0);
}

TEST(LibraryStore, UpgradesV14ByKeepingTagsAndSplittingTheNamesTheyCredit) {
    const auto dir = freshDir("upgrade-v14");
    {
        LibraryStore store(dir, std::vector<Migration>(libraryMigrations().begin(), libraryMigrations().begin() + 14));
        store
            .write([](Database& db) {
                addTrack(db, "C:/music/album/01.flac", "Duet");
                addTrack(db, "C:/music/album/02.flac", "Solo");
                addTrack(db, "C:/music/album/03.flac", "Band");
                const auto value = [&](int64_t track, const char* field, int64_t position, const char* text, const char* sort) {
                    db.prepare("INSERT INTO track_values(track_id,field,position,value,sort_source) VALUES(?,?,?,?,?)")
                        .bind(1, track)
                        .bind(2, std::string_view(field))
                        .bind(3, position)
                        .bind(4, std::string_view(text))
                        .bind(5, std::string_view(sort))
                        .run();
                };
                value(1, "artist", 0, "Aimer with chelly", "Aimer");
                value(1, "composer", 0, "ACAね、ぬゆり", "");
                value(1, "genre", 0, "J-Pop; Rock", "");
                value(2, "artist", 0, "Aimer", "Aimer sort");
                value(3, "artist", 0, "Bob Marley & The Wailers", "");
            })
            .get();
    }
    LibraryStore store(dir);
    auto db = store.openReader();
    const auto names = [&](int64_t track, const char* field) {
        std::vector<std::string> out;
        auto rows = db.prepare("SELECT value FROM track_values WHERE track_id=? AND field=? ORDER BY position");
        rows.bind(1, track).bind(2, std::string_view(field));
        while (rows.step()) out.push_back(rows.text(0));
        return out;
    };
    EXPECT_EQ(names(1, "artist"), (std::vector<std::string>{"Aimer", "chelly"}));
    EXPECT_EQ(names(1, "composer"), (std::vector<std::string>{"ACAね", "ぬゆり"}));
    EXPECT_EQ(names(1, "genre"), (std::vector<std::string>{"J-Pop; Rock"})); // genres are not names
    EXPECT_EQ(names(3, "artist"), (std::vector<std::string>{"Bob Marley & The Wailers"}));
    EXPECT_EQ(db.scalar("SELECT COUNT(*) FROM track_tags WHERE track_id=1 AND value='Aimer with chelly'"), 1);
    EXPECT_EQ(db.scalar("SELECT COUNT(*) FROM app_state WHERE key='name_rules'"), 1);
    // A split name sorts by itself; a tag that is one name keeps its sort tag.
    auto sort = db.prepare("SELECT sort_source FROM track_values WHERE track_id=? AND field='artist' AND position=0");
    EXPECT_TRUE(sort.bind(1, int64_t{1}).step());
    EXPECT_EQ(sort.text(0), "");
    sort.reset();
    EXPECT_TRUE(sort.bind(1, int64_t{2}).step());
    EXPECT_EQ(sort.text(0), "Aimer sort");
}

TEST(LibraryStore, KeepsTheNewestThreeBackups) {
    const auto dir = freshDir("rotate");
    {
        LibraryStore store(dir);
    }
    std::vector<Migration> extra;
    for (int v = 16; v <= 20; v++) {
        extra.push_back(addTable("t" + std::to_string(v)));
        LibraryStore store(dir, withExtra(extra));
    }
    EXPECT_EQ(backupCount(dir), size_t(LibraryStore::kKeptBackups));
    for (int v : {18, 19, 20}) EXPECT_TRUE(std::filesystem::exists(dir / ("library.db.v" + std::to_string(v) + ".bak"))) << v;
}

TEST(LibraryStore, ADamagedFileIsKeptAsideAndReplacedByTheNewestBackup) {
    const auto dir = freshDir("damaged");
    {
        LibraryStore store(dir);
        store.write([](Database& db) { addTrack(db, "C:/music/album/01.flac", "From backup"); }).get();
    }
    {
        LibraryStore store(dir, withExtra({addTable("v2_table")}));
    } // writes library.db.v16.bak
    std::filesystem::remove(dir / "library.db-wal");
    std::ofstream(dir / "library.db", std::ios::binary | std::ios::trunc) << std::string(8192, 'x');
    {
        LibraryStore store(dir, withExtra({addTable("v2_table")}));
        EXPECT_TRUE(store.report().damaged);
        EXPECT_TRUE(store.report().restoredBackup);
        EXPECT_TRUE(std::filesystem::exists(dir / "library.db.bad"));
        auto db = store.openReader();
        EXPECT_EQ(db.scalar("SELECT COUNT(*) FROM tracks WHERE title = 'From backup'"), 1);
        EXPECT_EQ(db.userVersion(), 17); // the backup was migrated again
    }
    for (const auto& e : std::filesystem::directory_iterator(dir))
        if (e.path().extension() == ".bak") std::filesystem::remove(e.path());
    std::ofstream(dir / "library.db", std::ios::binary | std::ios::trunc) << std::string(8192, 'y');
    LibraryStore store(dir);
    EXPECT_TRUE(store.report().damaged);
    EXPECT_FALSE(store.report().restoredBackup);
    EXPECT_EQ(store.openReader().scalar("SELECT COUNT(*) FROM tracks"), 0);
}

namespace {

std::vector<std::filesystem::path> dailyFiles(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> out;
    for (const auto& e : std::filesystem::directory_iterator(dir))
        if (e.path().filename().wstring().starts_with(L"library.db.daily-") && e.path().extension() == ".bak")
            out.push_back(e.path().filename());
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

TEST(LibraryStore, TakesACheckedDailyBackupThatADamagedFileIsReplacedByFirst) {
    const auto dir = freshDir("daily");
    {
        LibraryStore store(dir);
        store.write([](Database& db) { addTrack(db, "C:/music/album/01.flac", "Daily"); }).get();
        EXPECT_TRUE(store.backupIfDue());
        EXPECT_FALSE(store.backupIfDue()) << "one a day";
    }
    auto daily = dailyFiles(dir);
    ASSERT_EQ(daily.size(), 1u);
    const auto today = daily.front();
    const auto old = std::filesystem::file_time_type::clock::now() - std::chrono::hours(48);
    for (const auto* name : {L"library.db.daily-20200101.bak", L"library.db.daily-20200102.bak"}) {
        std::filesystem::copy_file(dir / today, dir / name);
        std::filesystem::last_write_time(dir / name, old);
    }
    std::filesystem::last_write_time(dir / today, old);
    {
        LibraryStore store(dir);
        store.write([](Database& db) { addTrack(db, "C:/music/album/02.flac", "Later"); }).get();
        EXPECT_TRUE(store.backupIfDue()) << "the newest is two days old";
    }
    daily = dailyFiles(dir);
    EXPECT_EQ(daily, std::vector<std::filesystem::path>{today}) << "only the newest is kept";

    std::filesystem::remove(dir / "library.db-wal");
    std::ofstream(dir / "library.db", std::ios::binary | std::ios::trunc) << std::string(8192, 'x');
    LibraryStore store(dir);
    EXPECT_TRUE(store.report().damaged);
    EXPECT_TRUE(store.report().restoredBackup);
    EXPECT_EQ(store.openReader().scalar("SELECT COUNT(*) FROM tracks WHERE title IN ('Daily', 'Later')"), 2);
}

TEST(LibraryStore, PagesAreCheckedOnlyAfterTheDatabaseWasNotClosed) {
    const auto dir = freshDir("unclosed");
    {
        LibraryStore store(dir);
    }
    {
        LibraryStore store(dir);
        EXPECT_FALSE(store.report().checked);
        EXPECT_TRUE(std::filesystem::exists(dir / "library.db.open"));
    }
    EXPECT_FALSE(std::filesystem::exists(dir / "library.db.open"));
    std::ofstream(dir / "library.db.open"); // as a crash or a forced end leaves it
    LibraryStore store(dir);
    EXPECT_TRUE(store.report().checked);
    EXPECT_FALSE(store.report().damaged);
}

TEST(LibraryStore, RefusesNetworkLocations) {
    EXPECT_TRUE(isNetworkPath(L"\\\\nas\\music\\camelplayer"));
    EXPECT_TRUE(isNetworkPath(L"\\\\?\\UNC\\nas\\music\\camelplayer"));
    EXPECT_FALSE(isNetworkPath(testutil::tempDir()));
    EXPECT_THROW(LibraryStore(L"\\\\nas\\music\\camelplayer"), std::runtime_error);
}
