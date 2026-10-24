// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include <gtest/gtest.h>

#include <windows.h>
#include <psapi.h>

#include <chrono>
#include <filesystem>

#include "Server/Library/Playlists.h"
#include "TestUtil.h"

using namespace camel::server;
using Kind = PlaylistSource::Kind;

namespace {

PlaylistSource folder(int64_t id) { return {Kind::Folder, id, {}, {}}; }
PlaylistSource album(int64_t id) { return {Kind::Album, id, {}, {}}; }
PlaylistSource track(int64_t file, int64_t cue = 0) { return {Kind::Track, 0, {file, cue}, {}}; }
PlaylistSource artist(std::string name) { return {Kind::Artist, 0, {}, std::move(name)}; }

class PlaylistsTest : public ::testing::Test {
protected:
    std::filesystem::path dir;
    std::unique_ptr<LibraryStore> store;

    // c:\m (1) ─┬─ album (2, album 1: disc 2 track 1 = file 3, disc 1 track 2 = file 2, disc 1 track 1 = file 1)
    //           │   └─ cd2 (3: file 4 without a track number, file 5 unavailable)
    //           └─ cue (4, album 2: file 6 with CUE virtual tracks 1, 2 and a hidden lead-in)
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
INSERT INTO folders(id,root_id,parent_id,path) VALUES(1,1,NULL,'c:\m'),(2,1,1,'c:\m\album'),(3,1,2,'c:\m\album\cd2'),(4,1,1,'c:\m\cue');
INSERT INTO files(id,folder_id,root_id,path,kind,size,mtime,available) VALUES
 (1,2,1,'c:\m\album\01.flac','audio',1,1,1),(2,2,1,'c:\m\album\02.flac','audio',1,1,1),
 (3,3,1,'c:\m\album\cd2\01.flac','audio',1,1,1),(4,3,1,'c:\m\album\cd2\bonus.flac','audio',1,1,1),
 (5,3,1,'c:\m\album\cd2\gone.flac','audio',1,1,0),(6,4,1,'c:\m\cue\live.flac','audio',1,1,1);
INSERT INTO albums(id,folder_id,grouping_key,title,album_artist,added) VALUES(1,2,'a','Stellar','Suisei',0),(2,4,'b','Live','Pekora',0);
INSERT INTO tracks(id,file_id,cue_index,album_id,title,artist,album,album_artist,year,disc_number,track_number,sample_rate,total_frames,start_frame,end_frame) VALUES
 (1,1,0,1,'Comet','Suisei','Stellar','Suisei',2021,1,1,1000,60000,0,NULL),
 (2,2,0,1,'Bluerose','Suisei','Stellar','Suisei',2021,1,2,1000,30000,0,NULL),
 (3,3,0,1,'Stellar','Suisei','Stellar','Suisei',2021,2,1,1000,90000,0,NULL),
 (4,4,0,1,'Bonus','Suisei','Stellar','Suisei',2021,2,0,1000,10000,0,NULL),
 (5,5,0,1,'Gone','Suisei','Stellar','Suisei',2021,2,2,1000,10000,0,NULL),
 (6,6,0,2,'Live','Pekora','Live','Pekora',2022,1,0,1000,100000,0,NULL),
 (7,6,-1,2,'','Pekora','Live','Pekora',2022,1,0,1000,100000,0,5000),
 (8,6,1,2,'Opening','Pekora','Live','Pekora',2022,1,1,1000,100000,5000,40000),
 (9,6,2,2,'Encore','Pekora & Miko','Live','Pekora',2022,1,2,1000,100000,40000,NULL);
INSERT INTO track_values(track_id,field,position,value) VALUES(9,'artist',0,'Pekora'),(9,'artist',1,'Miko');
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
              (std::vector<LibraryTrackKey>{{1, 0}, {2, 0}, {4, 0}, {3, 0}})); // disc 2 lists the unnumbered bonus first
}

TEST_F(PlaylistsTest, CueVirtualTracksReplaceTheirFileAndTheLeadInIsHidden) {
    const auto change = createPlaylist(*store, "Whole library", {folder(1)});
    const auto keys = tracks(change.playlist);
    EXPECT_EQ(keys.size(), 6u);
    EXPECT_EQ(std::count(keys.begin(), keys.end(), LibraryTrackKey{6, 1}), 1);
    EXPECT_EQ(std::count(keys.begin(), keys.end(), LibraryTrackKey{6, 2}), 1);
    EXPECT_EQ(std::count(keys.begin(), keys.end(), LibraryTrackKey{6, 0}), 0);
    EXPECT_EQ(std::count(keys.begin(), keys.end(), LibraryTrackKey{6, -1}), 0);
    const auto page = playlistItems(*store, change.playlist, 0, 500);
    const auto opening = std::find_if(page.items.begin(), page.items.end(), [](const auto& i) { return i.track == LibraryTrackKey{6, 1}; });
    ASSERT_NE(opening, page.items.end());
    EXPECT_EQ(opening->durationMs, 35000);
    EXPECT_EQ(opening->title, "Opening");
}

TEST_F(PlaylistsTest, OverlappingSourcesKeepTheFirstPositionAndExplicitTracksKeepTheirOrder) {
    const auto change = createPlaylist(*store, "Mix", {track(3), track(1), album(1), artist("Miko")});
    EXPECT_EQ(tracks(change.playlist), (std::vector<LibraryTrackKey>{{3, 0}, {1, 0}, {2, 0}, {4, 0}, {6, 2}}));
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
    EXPECT_EQ(tracks(created.playlist), (std::vector<LibraryTrackKey>{{1, 0}, {2, 0}, {4, 0}, {3, 0}}));
    EXPECT_THROW(addToPlaylist(*store, created.playlist, created.revision, {track(6, 1)}), std::runtime_error);
    EXPECT_THROW(addToPlaylist(*store, 999, std::nullopt, {track(6, 1)}), std::invalid_argument);
    EXPECT_EQ(addToPlaylist(*store, created.playlist, std::nullopt, {track(6, 1)}).added, 1);
}

TEST_F(PlaylistsTest, SavedEntriesKeepOrderDuplicatesAndEntriesWithoutAnIdentity) {
    const auto saved = savePlaylist(*store, "Queue",
                                    {{LibraryTrackKey{2, 0}, "c:\\m\\album\\02.flac"},
                                     {std::nullopt, "d:\\elsewhere\\x.flac"},
                                     {LibraryTrackKey{2, 0}, "c:\\m\\album\\02.flac"},
                                     {LibraryTrackKey{5, 0}, "c:\\m\\album\\cd2\\gone.flac"}});
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
    EXPECT_EQ(tracks(created.playlist), (std::vector<LibraryTrackKey>{{1, 0}, {4, 0}, {3, 0}}));
    PlaylistEdit move{PlaylistEdit::Kind::Move, created.playlist, std::nullopt, {}, {items[3], items[0]}, 0};
    editPlaylist(*store, move);
    EXPECT_EQ(tracks(created.playlist), (std::vector<LibraryTrackKey>{{1, 0}, {3, 0}, {4, 0}})); // moved keep their order
    move.items = {items[0]};
    move.position = 99;
    editPlaylist(*store, move);
    EXPECT_EQ(tracks(created.playlist), (std::vector<LibraryTrackKey>{{3, 0}, {4, 0}, {1, 0}}));
    const auto page = playlistItems(*store, created.playlist, 0, 500);
    for (int64_t i = 0; i < int64_t(page.items.size()); ++i) EXPECT_EQ(page.items[size_t(i)].position, i);

    addToPlaylist(*store, created.playlist, std::nullopt, {track(6, 2)});
    savePlaylist(*store, "unused", {});
    PlaylistEdit sort{PlaylistEdit::Kind::Sort, created.playlist};
    sort.key = PlaylistSortKey::Title;
    editPlaylist(*store, sort);
    EXPECT_EQ(tracks(created.playlist), (std::vector<LibraryTrackKey>{{4, 0}, {1, 0}, {6, 2}, {3, 0}}));
    sort.key = PlaylistSortKey::Duration;
    sort.descending = true;
    editPlaylist(*store, sort);
    EXPECT_EQ(tracks(created.playlist), (std::vector<LibraryTrackKey>{{3, 0}, {1, 0}, {6, 2}, {4, 0}})); // ties keep order
    sort.key = PlaylistSortKey::TrackNumber;
    sort.descending = false;
    editPlaylist(*store, sort);
    EXPECT_EQ(tracks(created.playlist), (std::vector<LibraryTrackKey>{{1, 0}, {6, 2}, {4, 0}, {3, 0}}));

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
    EXPECT_EQ(tracks(created.playlist), (std::vector<LibraryTrackKey>{{1, 0}}));
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
