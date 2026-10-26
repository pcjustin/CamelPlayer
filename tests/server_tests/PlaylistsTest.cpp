// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include <gtest/gtest.h>

#include <windows.h>
#include <psapi.h>

#include <chrono>
#include <filesystem>
#include <fstream>

#include "Common/util/Text.h"
#include "Server/Library/Playlists.h"
#include "TestUtil.h"

using namespace camel::server;
using Kind = PlaylistSource::Kind;

namespace {

PlaylistSource folder(int64_t id) { return {Kind::Folder, id, {}, {}}; }
PlaylistSource album(int64_t id) { return {Kind::Album, id, {}, {}}; }
PlaylistSource track(int64_t file) { return {Kind::Track, 0, {file}, {}}; }
PlaylistSource artist(std::string name) { return {Kind::Artist, 0, {}, std::move(name)}; }

class PlaylistsTest : public ::testing::Test {
protected:
    std::filesystem::path dir;
    std::unique_ptr<LibraryStore> store;

    // c:\m (1) ─┬─ album (2, album 1: disc 2 track 1 = file 3, disc 1 track 2 = file 2, disc 1 track 1 = file 1)
    //           │   └─ cd2 (3: file 4 without a track number, file 5 unavailable)
    //           └─ live (4, album 2: file 6, track 2 by two artists)
    void SetUp() override {
        dir = testutil::tempDir() / "playlists" /
              (std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()) + "-" +
               std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        std::filesystem::create_directories(dir);
        store = std::make_unique<LibraryStore>(dir);
        store
            ->write([](sqlite::Database& db) {
                db.exec(R"sql(
INSERT INTO roots(id,path,added) VALUES(1,'c:\m',0);
INSERT INTO folders(id,root_id,parent_id,path) VALUES(1,1,NULL,'c:\m'),(2,1,1,'c:\m\album'),(3,1,2,'c:\m\album\cd2'),(4,1,1,'c:\m\live');
INSERT INTO files(id,folder_id,root_id,path,kind,size,mtime,available) VALUES
 (1,2,1,'c:\m\album\01.flac','audio',1,1,1),(2,2,1,'c:\m\album\02.flac','audio',1,1,1),
 (3,3,1,'c:\m\album\cd2\01.flac','audio',1,1,1),(4,3,1,'c:\m\album\cd2\bonus.flac','audio',1,1,1),
 (5,3,1,'c:\m\album\cd2\gone.flac','audio',1,1,0),(6,4,1,'c:\m\live\encore.flac','audio',1,1,1);
INSERT INTO albums(id,folder_id,grouping_key,title,album_artist,added) VALUES(1,2,'a','Stellar','Suisei',0),(2,4,'b','Live','Pekora',0);
INSERT INTO tracks(id,file_id,album_id,title,artist,album,album_artist,year,disc_number,track_number,sample_rate,total_frames,start_frame,end_frame) VALUES
 (1,1,1,'Comet','Suisei','Stellar','Suisei',2021,1,1,1000,60000,0,NULL),
 (2,2,1,'Bluerose','Suisei','Stellar','Suisei',2021,1,2,1000,30000,0,NULL),
 (3,3,1,'Stellar','Suisei','Stellar','Suisei',2021,2,1,1000,90000,0,NULL),
 (4,4,1,'Bonus','Suisei','Stellar','Suisei',2021,2,0,1000,10000,0,NULL),
 (5,5,1,'Gone','Suisei','Stellar','Suisei',2021,2,2,1000,10000,0,NULL),
 (6,6,2,'Encore','Pekora & Miko','Live','Pekora',2022,1,2,1000,60000,0,NULL);
INSERT INTO track_values(track_id,field,position,value) VALUES(6,'artist',0,'Pekora'),(6,'artist',1,'Miko');
)sql");
            })
            .get();
    }

    std::vector<LibraryTrackKey> tracks(int64_t playlist) {
        std::vector<LibraryTrackKey> out;
        for (const auto& item : playlistItems(*store, playlist, 0, 500).items) out.push_back(item.track.value_or(LibraryTrackKey{}));
        return out;
    }
    std::vector<int64_t> ids(int64_t playlist) {
        std::vector<int64_t> out;
        for (const auto& item : playlistItems(*store, playlist, 0, 500).items) out.push_back(item.id);
        return out;
    }
    int64_t revision(int64_t playlist) { return playlistItems(*store, playlist, 0, 1).revision; }
};

TEST_F(PlaylistsTest, AFolderIncludesSubfoldersInAlbumDiscAndTrackOrder) {
    const auto change = createPlaylist(*store, "", {folder(2)});
    EXPECT_EQ(change.name, "album");
    EXPECT_EQ(change.added, 4);
    EXPECT_EQ(change.skippedUnavailable, 1);
    EXPECT_EQ(tracks(change.playlist),
              (std::vector<LibraryTrackKey>{{1}, {2}, {4}, {3}})); // disc 2 lists the unnumbered bonus first
}

TEST_F(PlaylistsTest, OverlappingSourcesKeepTheFirstPositionAndExplicitTracksKeepTheirOrder) {
    const auto change = createPlaylist(*store, "Mix", {track(3), track(1), album(1), artist("Miko")});
    EXPECT_EQ(tracks(change.playlist), (std::vector<LibraryTrackKey>{{3}, {1}, {2}, {4}, {6}}));
    EXPECT_EQ(change.skippedDuplicates, 2);
    EXPECT_EQ(change.skippedUnavailable, 1);
    const auto byColumn = createPlaylist(*store, "Suisei", {artist("Suisei")}); // no multivalue rows: the artist column
    EXPECT_EQ(byColumn.added, 4);
}

TEST_F(PlaylistsTest, PreviewReportsCountDurationAndSkippedTracks) {
    const auto preview = previewSelection(*store, {album(1), track(1)});
    EXPECT_EQ(preview.tracks, 4);
    EXPECT_EQ(preview.durationMs, 190000);
    EXPECT_EQ(preview.unavailable, 1);
    EXPECT_EQ(preview.duplicates, 1);
    EXPECT_TRUE(listPlaylists(*store).empty());
}

TEST_F(PlaylistsTest, NamesDeriveFromSourcesAndCollisionsGetASuffix) {
    EXPECT_EQ(createPlaylist(*store, "", {album(1), album(2)}).name, "Stellar +1");
    EXPECT_EQ(createPlaylist(*store, "", {album(1)}).name, "Stellar");
    EXPECT_EQ(createPlaylist(*store, "", {album(1)}).name, "Stellar (2)");
    const auto third = createPlaylist(*store, "Other", {album(2)});
    PlaylistEdit rename{PlaylistEdit::Kind::Rename, third.playlist, third.revision, "Stellar"};
    EXPECT_EQ(editPlaylist(*store, rename).name, "Stellar (3)");
    const auto names = listPlaylists(*store);
    ASSERT_EQ(names.size(), 4u);
    EXPECT_EQ(names[0].name, "Stellar");
    EXPECT_EQ(names[0].tracks, 4);
}

TEST_F(PlaylistsTest, AddingSkipsTracksAlreadyPresentAndRefusesAStaleRevision) {
    const auto created = createPlaylist(*store, "List", {track(1)});
    const auto added = addToPlaylist(*store, created.playlist, created.revision, {album(1)});
    EXPECT_EQ(added.added, 3);
    EXPECT_EQ(added.skippedExisting, 1);
    EXPECT_EQ(added.name, "List");
    EXPECT_EQ(tracks(created.playlist), (std::vector<LibraryTrackKey>{{1}, {2}, {4}, {3}}));
    EXPECT_THROW(addToPlaylist(*store, created.playlist, created.revision, {track(6)}), std::runtime_error);
    EXPECT_THROW(addToPlaylist(*store, 999, std::nullopt, {track(6)}), std::invalid_argument);
    EXPECT_EQ(addToPlaylist(*store, created.playlist, std::nullopt, {track(6)}).added, 1);
}

TEST_F(PlaylistsTest, SavedEntriesKeepOrderDuplicatesAndEntriesWithoutAnIdentity) {
    const auto saved = savePlaylist(*store, "Queue",
                                    {{LibraryTrackKey{2}, "c:\\m\\album\\02.flac"},
                                     {std::nullopt, "d:\\elsewhere\\x.flac"},
                                     {LibraryTrackKey{2}, "c:\\m\\album\\02.flac"},
                                     {LibraryTrackKey{5}, "c:\\m\\album\\cd2\\gone.flac"}});
    EXPECT_EQ(saved.added, 4);
    const auto page = playlistItems(*store, saved.playlist, 0, 500);
    ASSERT_EQ(page.items.size(), 4u);
    EXPECT_TRUE(page.items[0].available);
    EXPECT_FALSE(page.items[1].track);
    EXPECT_EQ(page.items[1].source, "d:\\elsewhere\\x.flac");
    EXPECT_FALSE(page.items[1].available);
    EXPECT_EQ(page.items[2].track, page.items[0].track);
    EXPECT_FALSE(page.items[3].available);
}

TEST_F(PlaylistsTest, RemoveMoveSortAndDeleteRenumberPositions) {
    const auto created = createPlaylist(*store, "Edit", {album(1)});
    auto items = ids(created.playlist); // tracks 1, 2, 4, 3
    PlaylistEdit remove{PlaylistEdit::Kind::Remove, created.playlist, std::nullopt, {}, {items[1]}};
    editPlaylist(*store, remove);
    EXPECT_EQ(tracks(created.playlist), (std::vector<LibraryTrackKey>{{1}, {4}, {3}}));
    PlaylistEdit move{PlaylistEdit::Kind::Move, created.playlist, std::nullopt, {}, {items[3], items[0]}, 0};
    editPlaylist(*store, move);
    EXPECT_EQ(tracks(created.playlist), (std::vector<LibraryTrackKey>{{1}, {3}, {4}})); // moved keep their order
    move.items = {items[0]};
    move.position = 99;
    editPlaylist(*store, move);
    EXPECT_EQ(tracks(created.playlist), (std::vector<LibraryTrackKey>{{3}, {4}, {1}}));
    const auto page = playlistItems(*store, created.playlist, 0, 500);
    for (int64_t i = 0; i < int64_t(page.items.size()); ++i) EXPECT_EQ(page.items[size_t(i)].position, i);

    addToPlaylist(*store, created.playlist, std::nullopt, {track(6)});
    savePlaylist(*store, "unused", {});
    PlaylistEdit sort{PlaylistEdit::Kind::Sort, created.playlist};
    sort.key = PlaylistSortKey::Title;
    editPlaylist(*store, sort);
    EXPECT_EQ(tracks(created.playlist), (std::vector<LibraryTrackKey>{{4}, {1}, {6}, {3}}));
    sort.key = PlaylistSortKey::Duration;
    sort.descending = true;
    editPlaylist(*store, sort);
    EXPECT_EQ(tracks(created.playlist), (std::vector<LibraryTrackKey>{{3}, {1}, {6}, {4}})); // ties keep order
    sort.key = PlaylistSortKey::TrackNumber;
    sort.descending = false;
    editPlaylist(*store, sort);
    EXPECT_EQ(tracks(created.playlist), (std::vector<LibraryTrackKey>{{1}, {6}, {4}, {3}}));

    PlaylistEdit erase{PlaylistEdit::Kind::Delete, created.playlist, revision(created.playlist)};
    editPlaylist(*store, erase);
    EXPECT_EQ(listPlaylists(*store).size(), 1u);
    EXPECT_THROW(playlistItems(*store, created.playlist, 0, 10), std::invalid_argument);
    EXPECT_EQ(store->openReader().scalar("SELECT COUNT(*) FROM playlist_items WHERE playlist_id=" +
                                         std::to_string(created.playlist)),
              0);
}

TEST_F(PlaylistsTest, QuickUndoRevertsOnlyAnUnchangedPlaylist) {
    const auto created = createPlaylist(*store, "Undo", {track(1)});
    const auto added = addToPlaylist(*store, created.playlist, std::nullopt, {album(2)});
    EXPECT_FALSE(undoPlaylistChange(*store, created.undo)); // the add changed it since
    EXPECT_TRUE(undoPlaylistChange(*store, added.undo));
    EXPECT_EQ(tracks(created.playlist), (std::vector<LibraryTrackKey>{{1}}));
    EXPECT_FALSE(undoPlaylistChange(*store, added.undo));

    const auto fresh = createPlaylist(*store, "Fresh", {album(1)});
    EXPECT_TRUE(undoPlaylistChange(*store, fresh.undo));
    EXPECT_EQ(listPlaylists(*store).size(), 1u);
    const auto renamed = createPlaylist(*store, "Renamed", {album(1)});
    editPlaylist(*store, {PlaylistEdit::Kind::Rename, renamed.playlist, std::nullopt, "Kept"});
    EXPECT_FALSE(undoPlaylistChange(*store, renamed.undo));
    EXPECT_EQ(listPlaylists(*store).size(), 2u);
}

TEST_F(PlaylistsTest, EntriesSurviveRootRemovalAsUnavailableAndReturnWithIt) {
    const auto created = createPlaylist(*store, "Root", {album(1)});
    store->write([](sqlite::Database& db) { db.exec("UPDATE roots SET removed=1"); }).get();
    const auto removed = playlistItems(*store, created.playlist, 0, 500);
    ASSERT_EQ(removed.items.size(), 4u);
    for (const auto& item : removed.items) EXPECT_FALSE(item.available);
    EXPECT_EQ(removed.items[0].title, "Comet");
    EXPECT_EQ(createPlaylist(*store, "Nothing", {album(1)}).added, 0);
    store->write([](sqlite::Database& db) { db.exec("UPDATE roots SET removed=NULL"); }).get();
    for (const auto& item : playlistItems(*store, created.playlist, 0, 500).items) EXPECT_TRUE(item.available);
}

TEST_F(PlaylistsTest, AYearAndAvailableFilePathsBecomeTracks) {
    const auto year = createPlaylist(*store, "", {{Kind::Year, 2021, {}, {}}});
    EXPECT_EQ(year.name, "2021");
    EXPECT_EQ(tracks(year.playlist), (std::vector<LibraryTrackKey>{{1}, {2}, {4}, {3}}));
    EXPECT_EQ(year.skippedUnavailable, 1);
    const auto paths = createPlaylist(*store, "", {{Kind::Path, 0, {}, "c:\\m\\album\\02.flac"},
                                                    {Kind::Path, 0, {}, "d:\\outside.flac"},
                                                    {Kind::Path, 0, {}, "c:\\m\\album\\cd2\\gone.flac"},
                                                    {Kind::Path, 0, {}, "c:\\m\\album\\01.flac"}});
    EXPECT_EQ(paths.name, "Bluerose +3");
    EXPECT_EQ(tracks(paths.playlist), (std::vector<LibraryTrackKey>{{2}, {1}}));
    EXPECT_EQ(paths.skippedUnavailable, 2);
}

TEST_F(PlaylistsTest, ItemsPageByPositionAndListTheFilesCurrentPath) {
    const auto created = createPlaylist(*store, "Album", {album(1)});
    store->write([](sqlite::Database& db) { db.exec(R"(UPDATE files SET path='c:\m\moved\01.flac' WHERE id=1)"); }).get();
    const auto first = playlistItems(*store, created.playlist, 0, 2);
    const auto rest = playlistItems(*store, created.playlist, 2, 2);
    ASSERT_EQ(first.items.size(), 2u);
    ASSERT_EQ(rest.items.size(), 2u);
    EXPECT_EQ(first.items[0].path, "c:\\m\\moved\\01.flac");
    EXPECT_EQ(first.items[0].source, "c:\\m\\album\\01.flac");
    EXPECT_EQ(rest.items[0].position, 2);
}

TEST_F(PlaylistsTest, AnM3u8ImportResolvesPathsAndKeepsOrderDuplicatesAndOutsideEntries) {
    const auto music = dir / L"音樂";
    std::filesystem::create_directories(music);
    const auto local = (music / L"夜に駆ける.flac").wstring();
    store
        ->write([&](sqlite::Database& db) {
            db.prepare("INSERT INTO files(id,folder_id,root_id,path,kind,size,mtime,available) VALUES(7,2,1,?,'audio',1,1,1)")
                .bind(1, camel::toUtf8(local))
                .run();
            db.exec("INSERT INTO tracks(id,file_id,album_id,title,sample_rate,total_frames,start_frame) "
                    "VALUES(10,7,1,'Yoru',1000,1000,0)");
        })
        .get();
    const auto file = dir / L"Mix 一.m3u8";
    std::ofstream(file, std::ios::binary) << "\xEF\xBB\xBF#EXTM3U\r\n#EXTINF:60,Suisei - Comet\r\nC:\\M\\ALBUM\\01.flac\r\n"
                                             "\xE9\x9F\xB3\xE6\xA8\x82/\xE5\xA4\x9C\xE3\x81\xAB\xE9\xA7\x86\xE3\x81\x91\xE3\x82\x8B.flac\n"
                                             "  c:\\m\\album\\01.flac  \n\nhttp://radio.example/stream\n"
                                             "c:\\m\\album\\cd2\\gone.flac\nd:\\elsewhere\\x.flac\n";
    const auto change = importPlaylistFile(*store, file);
    EXPECT_EQ(change.name, "Mix \xE4\xB8\x80");
    EXPECT_EQ(change.added, 5);
    EXPECT_EQ(change.skippedUnavailable, 1);
    const auto page = playlistItems(*store, change.playlist, 0, 500);
    ASSERT_EQ(page.items.size(), 5u);
    EXPECT_EQ(page.items[0].track, (LibraryTrackKey{1}));
    EXPECT_EQ(page.items[1].track, (LibraryTrackKey{7}));
    EXPECT_TRUE(page.items[1].available);
    EXPECT_EQ(page.items[2].track, (LibraryTrackKey{1}));
    EXPECT_EQ(page.items[3].track, (LibraryTrackKey{5})); // known but unavailable: returns with its file
    EXPECT_FALSE(page.items[3].available);
    EXPECT_FALSE(page.items[4].track);
    EXPECT_EQ(page.items[4].path, "d:\\elsewhere\\x.flac");
}

TEST_F(PlaylistsTest, AnM3uWithoutUtf8IsReadInTheSystemCodePage) {
    const auto file = dir / "legacy.m3u";
    std::ofstream(file, std::ios::binary) << "c:\\m\\album\\02.flac\nd:\\caf\xE9.flac\n";
    const auto page = playlistItems(*store, importPlaylistFile(*store, file).playlist, 0, 500);
    ASSERT_EQ(page.items.size(), 2u);
    EXPECT_EQ(page.items[0].track, (LibraryTrackKey{2}));
    wchar_t expected[16] = {};
    MultiByteToWideChar(CP_ACP, 0, "d:\\caf\xE9.flac", -1, expected, 16);
    EXPECT_EQ(page.items[1].path, camel::toUtf8(expected));
}

TEST_F(PlaylistsTest, AnM3u8ExportWritesRelativePathsOnTheSameDriveAndRoundTrips) {
    const auto music = dir / L"音樂";
    std::filesystem::create_directories(music);
    const auto inside = (music / L"a.flac").wstring();
    store
        ->write([&](sqlite::Database& db) {
            db.prepare("INSERT INTO files(id,folder_id,root_id,path,kind,size,mtime,available) VALUES(7,2,1,?,'audio',1,1,1)")
                .bind(1, camel::toUtf8(inside))
                .run();
            db.exec("INSERT INTO tracks(id,file_id,album_id,title,artist,sample_rate,total_frames,start_frame) "
                    "VALUES(10,7,1,'A','B',1000,2400,0)");
        })
        .get();
    const auto saved = savePlaylist(*store, "Out",
                                    {{LibraryTrackKey{7}, camel::toUtf8(inside)},
                                     {LibraryTrackKey{7}, camel::toUtf8(inside)},
                                     {std::nullopt, "\\\\nas\\share\\x.flac"}});
    const auto file = dir / "lists" / "out.m3u8";
    std::filesystem::create_directories(file.parent_path());
    exportPlaylistFile(*store, saved.playlist, file);
    std::ifstream in(file, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(text, "#EXTM3U\r\n#EXTINF:2,B - A\r\n..\\\xE9\x9F\xB3\xE6\xA8\x82\\a.flac\r\n#EXTINF:2,B - A\r\n"
                    "..\\\xE9\x9F\xB3\xE6\xA8\x82\\a.flac\r\n#EXTINF:-1,x\r\n\\\\nas\\share\\x.flac\r\n");
    EXPECT_FALSE(std::filesystem::exists(file.wstring() + L".tmp"));
    const auto again = playlistItems(*store, importPlaylistFile(*store, file).playlist, 0, 500);
    ASSERT_EQ(again.items.size(), 3u);
    EXPECT_EQ(again.items[0].track, (LibraryTrackKey{7}));
    EXPECT_EQ(again.items[1].track, (LibraryTrackKey{7}));
    EXPECT_EQ(again.items[2].path, "\\\\nas\\share\\x.flac");
}

TEST_F(PlaylistsTest, PlaylistFilesUseTheUsualPathFormAndTheOnDiskNameOfExtendedLibraryPaths) {
    store
        ->write([](sqlite::Database& db) {
            db.exec(R"(INSERT INTO files(id,folder_id,root_id,path,name,kind,size,mtime,available) VALUES
 (7,2,1,'\\?\unc\nas\music\a\track 1.flac','Track 1.flac','audio',1,1,1),
 (8,2,1,'\\?\d:\local\b.flac','B.flac','audio',1,1,1);
INSERT INTO tracks(id,file_id,album_id,sample_rate,total_frames,start_frame) VALUES
 (10,7,1,1000,1000,0),(11,8,1,1000,1000,0);)");
        })
        .get();
    const auto saved = savePlaylist(*store, "Nas", {{LibraryTrackKey{7}, ""}, {LibraryTrackKey{8}, ""}});
    const auto file = dir / "nas.m3u8";
    exportPlaylistFile(*store, saved.playlist, file);
    std::ifstream in(file, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(text, "#EXTM3U\r\n#EXTINF:1,Track 1\r\n\\\\nas\\music\\a\\Track 1.flac\r\n#EXTINF:1,B\r\nd:\\local\\B.flac\r\n");
    const auto page = playlistItems(*store, importPlaylistFile(*store, file).playlist, 0, 500);
    ASSERT_EQ(page.items.size(), 2u);
    EXPECT_EQ(page.items[0].track, (LibraryTrackKey{7}));
    EXPECT_EQ(page.items[1].track, (LibraryTrackKey{8}));
    const auto queued = createPlaylist(*store, "Queue", {{Kind::Path, 0, {}, "\\\\NAS\\Music\\a\\Track 1.flac"},
                                                         {Kind::Path, 0, {}, "D:\\Local\\B.flac"}});
    EXPECT_EQ(tracks(queued.playlist), (std::vector<LibraryTrackKey>{{7}, {8}}));
    EXPECT_EQ(createPlaylist(*store, "", {{Kind::Path, 0, {}, "\\\\NAS\\Music\\a\\Track 1.flac"}}).name, "Track 1");
    EXPECT_EQ(createPlaylist(*store, "", {track(8)}).name, "B");
}

TEST_F(PlaylistsTest, AHundredThousandTrackFolderBecomesAPlaylistQuicklyAndWithBoundedMemory) {
    store
        ->write([](sqlite::Database& db) {
            db.exec("INSERT INTO folders(id,root_id,parent_id,path) VALUES(10,1,1,'c:\\m\\big');"
                    "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<100000) "
                    "INSERT INTO files(id,folder_id,root_id,path,kind,size,mtime) "
                    "SELECT 1000+x,10,1,printf('c:\\m\\big\\%06d.flac',x),'audio',1,1 FROM n;"
                    "INSERT INTO tracks(file_id,title,artist,album,track_number,sample_rate,total_frames) "
                    "SELECT id,'T','A','Big',0,1000,1000 FROM files WHERE folder_id=10;");
        })
        .get();
    PROCESS_MEMORY_COUNTERS before{}, after{};
    GetProcessMemoryInfo(GetCurrentProcess(), &before, sizeof(before));
    const auto start = std::chrono::steady_clock::now();
    const auto change = createPlaylist(*store, "Big", {folder(10)});
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    GetProcessMemoryInfo(GetCurrentProcess(), &after, sizeof(after));
    RecordProperty("create_100k_seconds", std::to_string(seconds));
    EXPECT_EQ(change.added, 100000);
    EXPECT_LT(seconds, 30.0);
    EXPECT_LT(after.PeakWorkingSetSize - before.PeakWorkingSetSize, 256u << 20);
    const auto page = playlistItems(*store, change.playlist, 99990, 20);
    EXPECT_EQ(page.total, 100000);
    ASSERT_EQ(page.items.size(), 10u);
    EXPECT_EQ(page.items.back().source, "c:\\m\\big\\100000.flac");
    CancellationToken cancelled;
    cancelled.cancel();
    EXPECT_THROW(createPlaylist(*store, "Cancelled", {folder(10)}, cancelled), std::exception);
    EXPECT_EQ(listPlaylists(*store).size(), 1u);
}

} // namespace
