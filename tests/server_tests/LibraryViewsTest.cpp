// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include <gtest/gtest.h>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <random>

#include "Server/Library/LibraryViews.h"
#include "Server/Library/PlayStatistics.h"
#include "TestUtil.h"

using namespace camel::server;

namespace {

class LibraryViewsTest : public ::testing::Test {
protected:
    std::filesystem::path dir;
    std::unique_ptr<LibraryStore> store;

    // Root 1 (online): folder 1 ─ 2 "a" (album 1 "Beta", 2020, two discs) and 3 "b" (album 2 "Alpha", 2022).
    // Album 3 "Gamma" has only an unavailable track; root 2 is offline.
    void SetUp() override {
        dir = testutil::tempDir() / "library-views" /
              (std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()) + "-" +
               std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        std::filesystem::create_directories(dir);
        store = std::make_unique<LibraryStore>(dir);
        store
            ->write([](sqlite::Database& db) {
                db.exec(R"sql(
INSERT INTO roots(id,path,added,online) VALUES(1,'c:\m',0,1),(2,'d:\off',0,0);
INSERT INTO folders(id,root_id,parent_id,path) VALUES(1,1,NULL,'c:\m'),(2,1,1,'c:\m\a'),(3,1,1,'c:\m\b'),(4,2,NULL,'d:\off');
INSERT INTO files(id,folder_id,root_id,path,kind,size,mtime,available) VALUES
 (1,2,1,'c:\m\a\1.flac','audio',1,1,1),(2,2,1,'c:\m\a\2.flac','audio',1,1,1),(3,2,1,'c:\m\a\3.flac','audio',1,1,1),
 (4,3,1,'c:\m\b\1.flac','audio',1,1,1),(5,3,1,'c:\m\b\gone.flac','audio',1,1,0),(6,4,2,'d:\off\1.flac','audio',1,1,1);
INSERT INTO albums(id,folder_id,grouping_key,title,album_artist,year,added,sort_key) VALUES
 (1,2,'a','Beta','Zed',2020,10,CAST('beta' AS BLOB)),(2,3,'b','Alpha','Amy',2022,20,CAST('alpha' AS BLOB)),
 (3,3,'c','Gamma','Amy',2021,30,CAST('gamma' AS BLOB)),(4,4,'d','Offline','Amy',2019,40,CAST('offline' AS BLOB));
INSERT INTO tracks(id,file_id,album_id,title,artist,album,composer,genre,year,disc_number,track_number,format,sample_rate,total_frames,title_sort,artist_sort,album_sort) VALUES
 (1,1,1,'Song C','Zed','Beta','Bach','Rock',2020,1,2,'flac',1000,30000,CAST('song c' AS BLOB),CAST('zed' AS BLOB),CAST('beta' AS BLOB)),
 (2,2,1,'Song A','Zed','Beta','Bach','Rock',2020,1,1,'flac',1000,10000,CAST('song a' AS BLOB),CAST('zed' AS BLOB),CAST('beta' AS BLOB)),
 (3,3,1,'Song B','Amy & Zed','Beta','','Jazz',2020,2,1,'flac',1000,20000,CAST('song b' AS BLOB),CAST('amy' AS BLOB),CAST('beta' AS BLOB)),
 (4,4,2,'Song D','Amy','Alpha','','Jazz',2022,1,1,'mp3',1000,40000,CAST('song d' AS BLOB),CAST('amy' AS BLOB),CAST('alpha' AS BLOB)),
 (5,5,3,'Song E','Amy','Gamma','','Jazz',2021,1,1,'flac',1000,5000,CAST('song e' AS BLOB),CAST('amy' AS BLOB),CAST('gamma' AS BLOB)),
 (6,6,4,'Song F','Amy','Offline','','Jazz',2019,1,1,'flac',1000,5000,CAST('song f' AS BLOB),CAST('amy' AS BLOB),CAST('offline' AS BLOB));
INSERT INTO track_values(track_id,field,position,value,sort_key) VALUES
 (3,'artist',0,'Amy',CAST('amy' AS BLOB)),(3,'artist',1,'Zed',CAST('zed' AS BLOB));
)sql");
            })
            .get();
    }

    std::vector<std::string> titles(const LibraryAlbumPage& page) {
        std::vector<std::string> out;
        for (const auto& a : page.albums) out.push_back(a.title);
        return out;
    }
    std::vector<int64_t> ids(const LibraryTrackRowPage& page) {
        std::vector<int64_t> out;
        for (const auto& t : page.tracks) out.push_back(t.id);
        return out;
    }
};

TEST_F(LibraryViewsTest, RecentlyPlayedAlbumsFollowTheLatestPlayAndHideUnavailableOnes) {
    EXPECT_EQ(recentlyPlayedAlbums(*store, 0, 10).total, 0);
    store
        ->write([](sqlite::Database& db) {
            db.exec("INSERT INTO play_events(file_id,started,utc_offset,heard_ms,counted) VALUES"
                    "(1,100,480,1000,1),(4,200,480,1000,1),(2,300,480,1000,1)," // Beta, Alpha, then Beta again
                    "(5,400,480,1000,1),(6,500,480,1000,1)");                   // unavailable and offline
        })
        .get();
    const auto page = recentlyPlayedAlbums(*store, 0, 10);
    EXPECT_EQ(page.total, 2);
    EXPECT_EQ(titles(page), (std::vector<std::string>{"Beta", "Alpha"}));
    EXPECT_EQ(page.albums[0].tracks, 3);
    EXPECT_EQ(titles(recentlyPlayedAlbums(*store, 1, 10)), (std::vector<std::string>{"Alpha"}));
}

TEST_F(LibraryViewsTest, TheAlbumWallListsVisibleAlbumsInEachSortOrder) {
    const auto byTitle = browseAlbums(*store, AlbumSort::Title, std::nullopt, 0, 50);
    EXPECT_EQ(byTitle.total, 2); // Gamma has no available track and Offline sits on an offline root
    EXPECT_EQ(titles(byTitle), (std::vector<std::string>{"Alpha", "Beta"}));
    const auto& beta = byTitle.albums[1];
    EXPECT_EQ(beta.artist, "Zed");
    EXPECT_EQ(beta.tracks, 3);
    EXPECT_EQ(beta.discs, 2);
    EXPECT_EQ(beta.durationMs, 60000);
    EXPECT_EQ(beta.year, 2020);
    EXPECT_EQ(beta.folder, 2);
    EXPECT_EQ(titles(browseAlbums(*store, AlbumSort::Artist, std::nullopt, 0, 50)), (std::vector<std::string>{"Alpha", "Beta"}));
    EXPECT_EQ(titles(browseAlbums(*store, AlbumSort::Year, std::nullopt, 0, 50)), (std::vector<std::string>{"Alpha", "Beta"}));
    EXPECT_EQ(titles(browseAlbums(*store, AlbumSort::Added, std::nullopt, 0, 50)), (std::vector<std::string>{"Alpha", "Beta"}));
    const auto second = browseAlbums(*store, AlbumSort::Title, std::nullopt, 1, 1);
    EXPECT_EQ(second.total, 2);
    EXPECT_EQ(titles(second), (std::vector<std::string>{"Beta"}));
    EXPECT_TRUE(browseAlbums(*store, AlbumSort::Title, std::nullopt, 5, 10).albums.empty());
    EXPECT_THROW(browseAlbums(*store, AlbumSort::Title, std::nullopt, 0, 0), std::invalid_argument);
}

TEST_F(LibraryViewsTest, GroupFiltersNarrowTheAlbumWall) {
    const auto zed = browseAlbums(*store, AlbumSort::Title, LibraryGroupFilter{LibraryGroupField::Artist, "Zed"}, 0, 50);
    EXPECT_EQ(titles(zed), (std::vector<std::string>{"Beta"}));
    EXPECT_EQ(zed.albums[0].tracks, 3); // two by the artist column, one by its multivalue row
    const auto amy = browseAlbums(*store, AlbumSort::Title, LibraryGroupFilter{LibraryGroupField::Artist, "Amy"}, 0, 50);
    EXPECT_EQ(titles(amy), (std::vector<std::string>{"Alpha", "Beta"}));
    EXPECT_EQ(amy.albums[1].tracks, 1);
    EXPECT_EQ(titles(browseAlbums(*store, AlbumSort::Title, LibraryGroupFilter{LibraryGroupField::Year, "2022"}, 0, 50)),
              (std::vector<std::string>{"Alpha"}));
    EXPECT_EQ(titles(browseAlbums(*store, AlbumSort::Title, LibraryGroupFilter{LibraryGroupField::Genre, "Rock"}, 0, 50)),
              (std::vector<std::string>{"Beta"}));
    EXPECT_EQ(browseAlbums(*store, AlbumSort::Title, LibraryGroupFilter{LibraryGroupField::Artist, "O'Neil"}, 0, 50).total, 0);
    EXPECT_THROW(browseAlbums(*store, AlbumSort::Title, LibraryGroupFilter{LibraryGroupField::Year, "20x"}, 0, 50),
                 std::invalid_argument);
}

TEST_F(LibraryViewsTest, GroupsCountAlbumsAndTracksAndCreditEveryArtist) {
    const auto artists = browseGroups(*store, LibraryGroupField::Artist, 0, 50);
    ASSERT_EQ(artists.total, 2);
    EXPECT_EQ(artists.groups[0].name, "Amy");
    EXPECT_EQ(artists.groups[0].albums, 2);
    EXPECT_EQ(artists.groups[0].tracks, 2);
    EXPECT_EQ(artists.groups[1].name, "Zed");
    EXPECT_EQ(artists.groups[1].tracks, 3);
    const auto composers = browseGroups(*store, LibraryGroupField::Composer, 0, 50);
    ASSERT_EQ(composers.groups.size(), 1u);
    EXPECT_EQ(composers.groups[0].name, "Bach");
    EXPECT_EQ(composers.groups[0].tracks, 2);
    const auto years = browseGroups(*store, LibraryGroupField::Year, 0, 50);
    ASSERT_EQ(years.groups.size(), 2u);
    EXPECT_EQ(years.groups[0].name, "2022"); // newest first
    EXPECT_EQ(years.groups[1].name, "2020");
    EXPECT_EQ(browseGroups(*store, LibraryGroupField::Genre, 1, 1).groups[0].name, "Rock");
}

TEST_F(LibraryViewsTest, TrackRowsSortByEachColumnInEitherDirection) {
    EXPECT_EQ(ids(browseTrackRows(*store, TrackSort::Title, false, 0, 50)), (std::vector<int64_t>{2, 3, 1, 4}));
    EXPECT_EQ(ids(browseTrackRows(*store, TrackSort::Title, true, 0, 50)), (std::vector<int64_t>{4, 1, 3, 2}));
    EXPECT_EQ(ids(browseTrackRows(*store, TrackSort::Album, false, 0, 50)), (std::vector<int64_t>{4, 2, 1, 3}));
    EXPECT_EQ(ids(browseTrackRows(*store, TrackSort::Artist, false, 0, 50)), (std::vector<int64_t>{4, 3, 2, 1}));
    EXPECT_EQ(ids(browseTrackRows(*store, TrackSort::Duration, true, 0, 50)), (std::vector<int64_t>{4, 1, 3, 2}));
    EXPECT_EQ(ids(browseTrackRows(*store, TrackSort::Year, true, 0, 50)), (std::vector<int64_t>{4, 2, 1, 3}));
    EXPECT_EQ(ids(browseTrackRows(*store, TrackSort::Added, true, 0, 50)), (std::vector<int64_t>{4, 3, 2, 1}));
    const auto page = browseTrackRows(*store, TrackSort::Title, false, 0, 1);
    EXPECT_EQ(page.total, 4);
    ASSERT_EQ(page.tracks.size(), 1u);
    EXPECT_EQ(page.tracks[0].durationMs, 10000);
    EXPECT_EQ(page.tracks[0].track, (LibraryTrackKey{2}));
    EXPECT_EQ(page.tracks[0].format, "flac");
}

TEST_F(LibraryViewsTest, FavoritesShowInTrackRowsAndAlbums) {
    const auto before = browseTrackRows(*store, TrackSort::Title, false, 0, 50).revision;
    setTrackFavorite(*store, {{3}, {4}}, true);
    setTrackFavorite(*store, {{4}}, false);
    const auto rows = browseTrackRows(*store, TrackSort::Title, false, 0, 50);
    EXPECT_NE(rows.revision, before);
    ASSERT_EQ(rows.tracks.size(), 4u);
    EXPECT_TRUE(rows.tracks[1].favorite); // Song B
    EXPECT_FALSE(rows.tracks[3].favorite); // Song D, cleared again
    const auto detail = albumDetail(*store, 1);
    ASSERT_EQ(detail.tracks.size(), 3u);
    EXPECT_TRUE(detail.tracks[2].favorite);
    EXPECT_FALSE(detail.tracks[0].favorite);
    EXPECT_FALSE(detail.album.favorite);

    setAlbumFavorite(*store, {1}, true);
    EXPECT_TRUE(albumDetail(*store, 1).album.favorite);
    const auto wall = browseAlbums(*store, AlbumSort::Title, std::nullopt, 0, 10);
    ASSERT_EQ(wall.albums.size(), 2u);
    EXPECT_FALSE(wall.albums[0].favorite); // Alpha
    EXPECT_TRUE(wall.albums[1].favorite);  // Beta
    setAlbumFavorite(*store, {1}, false);
    EXPECT_FALSE(albumDetail(*store, 1).album.favorite);
    EXPECT_TRUE(albumDetail(*store, 1).tracks[2].favorite); // the album mark leaves track favorites alone
}

TEST_F(LibraryViewsTest, SmartListsFollowPlaysFavoritesAndTheDateAdded) {
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    store
        ->write([now](sqlite::Database& db) {
            db.exec("UPDATE files SET added=" + std::to_string(now - 40 * 86400) + " WHERE id IN (1,2);"
                    "UPDATE files SET added=" + std::to_string(now - 86400) + " WHERE id=3;"
                    "UPDATE files SET added=" + std::to_string(now) + " WHERE id IN (4,5);"
                    "INSERT INTO stats(file_id,play_count,listened_ms) VALUES(1,3,1000),(2,3,2000),(5,9,9000);"
                    "INSERT INTO play_events(file_id,started,utc_offset,heard_ms,counted) VALUES"
                    "(1,100,0,1000,1),(2,300,0,1000,1),(3,200,0,10,0),(5,400,0,1000,1);");
        })
        .get();
    const auto list = [&](SmartList smart) { return ids(smartTracks(*store, smart, 0, 50)); };
    EXPECT_EQ(list(SmartList::RecentlyAdded), (std::vector<int64_t>{4, 3})); // 5 is unavailable
    EXPECT_EQ(list(SmartList::MostPlayed), (std::vector<int64_t>{2, 1}));    // equal counts: more listening first
    EXPECT_EQ(list(SmartList::RecentlyPlayed), (std::vector<int64_t>{2, 3, 1}));
    EXPECT_EQ(list(SmartList::NeverPlayed), (std::vector<int64_t>{4, 3})); // 3 was heard but never counted
    store->write([](sqlite::Database& db) { db.exec("UPDATE stats SET play_count=4 WHERE file_id=1"); }).get();
    EXPECT_EQ(list(SmartList::MostPlayed), (std::vector<int64_t>{1, 2})); // plays are read again without a library change

    setTrackFavorite(*store, {{1}}, true);
    Sleep(5);
    setTrackFavorite(*store, {{4}}, true);
    EXPECT_EQ(list(SmartList::Favorites), (std::vector<int64_t>{4, 1}));
    setTrackFavorite(*store, {{1}}, true); // keeps the time it became a favorite
    EXPECT_EQ(list(SmartList::Favorites), (std::vector<int64_t>{4, 1}));

    EXPECT_EQ(favoriteAlbums(*store, 0, 10).total, 0);
    setAlbumFavorite(*store, {1}, true);
    Sleep(5);
    setAlbumFavorite(*store, {2}, true);
    const auto albums = favoriteAlbums(*store, 0, 10);
    ASSERT_EQ(albums.albums.size(), 2u);
    EXPECT_EQ(albums.albums[0].id, 2);
    EXPECT_EQ(albums.albums[1].id, 1);
    EXPECT_TRUE(albums.albums[0].favorite);
}

TEST_F(LibraryViewsTest, AFavoriteAlbumStaysAFavoriteWhenItsTracksMoveToAnotherAlbum) {
    setAlbumFavorite(*store, {2}, true);
    store->write([](sqlite::Database& db) { db.exec("UPDATE tracks SET album_id=1 WHERE id=4"); }).get();
    EXPECT_TRUE(albumDetail(*store, 1).album.favorite);
}

TEST_F(LibraryViewsTest, PagingRefusesAChangedRevision) {
    const auto first = browseTrackRows(*store, TrackSort::Title, false, 0, 2);
    EXPECT_NO_THROW(browseTrackRows(*store, TrackSort::Title, false, 2, 2, first.revision));
    store->write([](sqlite::Database& db) { db.exec("UPDATE tracks SET title='Renamed' WHERE id=1"); }).get();
    EXPECT_THROW(browseTrackRows(*store, TrackSort::Title, false, 2, 2, first.revision), std::runtime_error);
    EXPECT_THROW(browseAlbums(*store, AlbumSort::Title, std::nullopt, 0, 2, first.revision), std::runtime_error);
    EXPECT_THROW(browseGroups(*store, LibraryGroupField::Artist, 0, 2, first.revision), std::runtime_error);
}

TEST_F(LibraryViewsTest, AlbumDetailListsTracksByDiscAndNumber) {
    const auto detail = albumDetail(*store, 1);
    EXPECT_EQ(detail.album.title, "Beta");
    ASSERT_EQ(detail.tracks.size(), 3u);
    EXPECT_EQ(detail.tracks[0].id, 2);
    EXPECT_EQ(detail.tracks[1].id, 1);
    EXPECT_EQ(detail.tracks[2].id, 3);
    EXPECT_EQ(detail.tracks[2].disc, 2);
    EXPECT_THROW(albumDetail(*store, 3), std::invalid_argument); // nothing available
    EXPECT_THROW(albumDetail(*store, 99), std::invalid_argument);
}

TEST_F(LibraryViewsTest, FoldersStartAtOnlineRootsAndListSubfoldersAndTracks) {
    // Folders without tracks below them are hidden: "empty" holds only a picture, "c" only a folder with nothing playable.
    store
        ->write([](sqlite::Database& db) {
            db.exec(R"sql(
INSERT INTO folders(id,root_id,parent_id,path) VALUES(5,1,1,'c:\m\empty'),(6,1,1,'c:\m\c'),(7,1,6,'c:\m\c\d');
INSERT INTO files(id,folder_id,root_id,path,kind,size,mtime,available) VALUES(7,5,1,'c:\m\empty\cover.jpg','image',1,1,1);
)sql");
        })
        .get();
    const auto roots = browseFolder(*store, std::nullopt);
    ASSERT_EQ(roots.folders.size(), 1u); // the offline root is hidden
    EXPECT_EQ(roots.folders[0].path, "c:\\m");
    EXPECT_EQ(roots.folders[0].name, "m");
    const auto& location = roots.folders[0].location; // "COMPUTER › [C:] label › m"
    EXPECT_NE(location.find("[C:]"), std::string::npos);
    EXPECT_TRUE(location.ends_with("\xe2\x80\xba m"));
    EXPECT_TRUE(roots.tracks.empty());
    EXPECT_TRUE(roots.trail.empty());
    const auto top = browseFolder(*store, 1);
    ASSERT_EQ(top.folders.size(), 2u);
    EXPECT_EQ(top.folders[0].name, "a");
    EXPECT_EQ(top.folders[1].id, 3);
    store
        ->write([](sqlite::Database& db) {
            db.exec("INSERT INTO files(id,folder_id,root_id,path,kind,size,mtime,available) "
                    "VALUES(8,7,1,'c:\\m\\c\\d\\1.flac','audio',1,1,1);"
                    "INSERT INTO tracks(id,file_id,title) VALUES(7,8,'Nested');");
        })
        .get();
    EXPECT_EQ(browseFolder(*store, 1).folders.size(), 3u); // "c" has a track two levels down
    const auto a = browseFolder(*store, 2);
    EXPECT_TRUE(a.folders.empty());
    ASSERT_EQ(a.trail.size(), 2u);
    EXPECT_EQ(a.trail[0].name, "m");
    EXPECT_EQ(a.trail[1].id, 2);
    EXPECT_EQ(a.trail[1].name, "a");
    ASSERT_EQ(a.tracks.size(), 3u);
    EXPECT_EQ(a.tracks[0].id, 2);
    EXPECT_EQ(browseFolder(*store, 3).tracks.size(), 1u); // the unavailable file is not listed
    const auto all = libraryRoots(*store);
    ASSERT_EQ(all.size(), 2u);
    EXPECT_TRUE(all[0].online);
    EXPECT_FALSE(all[1].online);
}

TEST_F(LibraryViewsTest, DeepPagesOfAHundredThousandTracksStayFast) {
    store
        ->write([](sqlite::Database& db) {
            db.exec("INSERT INTO folders(id,root_id,parent_id,path) VALUES(10,1,1,'c:\\m\\big');"
                    "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<100000) "
                    "INSERT INTO files(id,folder_id,root_id,path,kind,size,mtime) "
                    "SELECT 1000+x,10,1,printf('c:\\m\\big\\%06d.flac',x),'audio',1,1 FROM n;"
                    "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<1000) "
                    "INSERT INTO albums(id,folder_id,grouping_key,title,album_artist,added,sort_key) "
                    "SELECT 100+x,10,'big'||x,printf('Album %04d',x),'Artist',x,CAST(printf('album %04d',x) AS BLOB) FROM n;"
                    "INSERT INTO tracks(file_id,album_id,title,artist,album,sample_rate,total_frames,title_sort,artist_sort,album_sort) "
                    "SELECT id,101+(id-1001)%1000,printf('Track %06d',id),'Artist','Album',1000,1000,"
                    "CAST(printf('track %06d',id) AS BLOB),CAST('artist' AS BLOB),CAST('album' AS BLOB) FROM files WHERE folder_id=10;");
        })
        .get();
    // The first page of a listing builds its order once per library revision; later pages only read their slice.
    const auto coldStart = std::chrono::steady_clock::now();
    ASSERT_EQ(browseTrackRows(*store, TrackSort::Title, false, 0, 100).total, 100004);
    const double trackCold = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - coldStart).count();
    const auto albumStart = std::chrono::steady_clock::now();
    ASSERT_EQ(browseAlbums(*store, AlbumSort::Title, std::nullopt, 0, 100).total, 1002);
    const double albumCold = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - albumStart).count();
    std::mt19937 random(7);
    std::vector<double> tracks, albums;
    for (int i = 0; i < 30; ++i) {
        auto start = std::chrono::steady_clock::now();
        const auto page = browseTrackRows(*store, TrackSort::Title, false, int64_t(random() % 99'900), 100);
        tracks.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        ASSERT_EQ(page.tracks.size(), 100u);
        start = std::chrono::steady_clock::now();
        const auto wall = browseAlbums(*store, AlbumSort::Title, std::nullopt, int64_t(random() % 900), 100);
        albums.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        ASSERT_EQ(wall.albums.size(), 100u);
    }
    std::sort(tracks.begin(), tracks.end());
    std::sort(albums.begin(), albums.end());
    const double trackP95 = tracks[28], albumP95 = albums[28];
    RecordProperty("track_order_cold_ms", std::to_string(trackCold));
    RecordProperty("album_order_cold_ms", std::to_string(albumCold));
    RecordProperty("track_page_p95_ms", std::to_string(trackP95));
    RecordProperty("album_page_p95_ms", std::to_string(albumP95));
    std::cout << "100k orders cold " << trackCold << " / " << albumCold << " ms; random page p95 tracks " << trackP95
              << " ms, albums " << albumP95 << " ms" << std::endl;
#ifdef NDEBUG
    EXPECT_LT(trackP95, 50.0);
    EXPECT_LT(albumP95, 100.0);
    EXPECT_LT(trackCold, 3000.0);
    EXPECT_LT(albumCold, 3000.0);
#endif
}

} // namespace
