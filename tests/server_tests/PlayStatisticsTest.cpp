// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include <gtest/gtest.h>

#include <filesystem>

#include "Server/Library/LibraryIdentity.h"
#include "Server/Library/PlayStatistics.h"
#include "TestUtil.h"

using namespace camel::server;

namespace {

constexpr uint32_t kRate = 1000; // one frame per millisecond keeps the arithmetic readable
constexpr uint64_t kAny = UINT64_MAX;

PlaySession session(uint64_t length, uint64_t rangeFirst = 0, std::optional<uint64_t> rangeEnd = std::nullopt) {
    return PlaySession({1}, kRate, length, rangeFirst, rangeEnd);
}

// Plays [from, to) in 500-frame steps, as a polling Server observes a running stream.
void play(PlaySession& s, uint64_t from, uint64_t to) {
    s.interrupt();
    for (uint64_t p = from; p < to; p += 500) s.observe(p, 1000);
    s.observe(to, 1000);
}

TEST(PlaySession, CreditsContinuousPlaybackButNotSeeksOrPauses) {
    auto s = session(100'000);
    play(s, 0, 10'000);
    EXPECT_EQ(s.heardMs(), 10'000u);
    s.observe(60'000, 1000); // forward seek: the skipped audio was not heard
    s.observe(61'000, 1000);
    EXPECT_EQ(s.heardMs(), 11'000u);
    s.observe(20'000, 1000); // backward seek: replayed audio counts again
    s.observe(21'000, 1000);
    EXPECT_EQ(s.heardMs(), 12'000u);
    s.interrupt(); // pause: resuming does not credit the time spent paused
    s.observe(21'000, kAny);
    s.observe(80'000, kAny);
    EXPECT_EQ(s.heardMs(), 71'000u);
}

TEST(PlaySession, CountsOnceAtHalfTheLengthOrFourMinutes) {
    auto shortTrack = session(100'000);
    play(shortTrack, 0, 49'999);
    EXPECT_FALSE(shortTrack.counted());
    play(shortTrack, 49'999, 50'000);
    EXPECT_TRUE(shortTrack.counted());
    auto longTrack = session(900'000);
    play(longTrack, 0, 239'999);
    EXPECT_FALSE(longTrack.counted());
    play(longTrack, 239'999, 240'000);
    EXPECT_TRUE(longTrack.counted());
    auto interlude = session(8'000);
    play(interlude, 0, 4'000);
    EXPECT_TRUE(interlude.counted());
}

TEST(PlaySession, UnknownLengthCountsAtFourMinutesOrNaturalEnd) {
    auto ended = session(0);
    play(ended, 0, 30'000);
    EXPECT_FALSE(ended.counted());
    ended.finish();
    EXPECT_TRUE(ended.counted());
    auto open = session(0);
    play(open, 0, 240'000);
    EXPECT_TRUE(open.counted());
    auto unheard = session(0);
    unheard.finish();
    EXPECT_FALSE(unheard.counted());
}

TEST(PlaySession, FinishCreditsTheTailAndRangesLimitCredit) {
    auto whole = session(100'000);
    play(whole, 0, 99'950);
    whole.finish();
    EXPECT_EQ(whole.heardMs(), 100'000u);
    auto stopped = session(100'000);
    play(stopped, 0, 30'000);
    stopped.interrupt();
    stopped.finish(); // an interrupted span has no tail to credit
    EXPECT_EQ(stopped.heardMs(), 30'000u);
    auto virtualTrack = session(10'000, 10'000, 20'000);
    play(virtualTrack, 0, 30'000);
    EXPECT_EQ(virtualTrack.heardMs(), 10'000u);
    EXPECT_TRUE(virtualTrack.counted());
}

class PlayStatisticsTest : public ::testing::Test {
protected:
    std::filesystem::path dir;
    std::unique_ptr<LibraryStore> store;

    void SetUp() override {
        dir = testutil::tempDir() / "play-statistics" / ::testing::UnitTest::GetInstance()->current_test_info()->name();
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        store = std::make_unique<LibraryStore>(dir);
        // Track 1 (file 1) has two ARTIST values; track 2 (file 2) has only the single-valued artist column.
        store
            ->write([](sqlite::Database& db) {
                db.exec("INSERT INTO roots(id,path,added) VALUES(1,'c:/music',0);"
                        "INSERT INTO folders(id,root_id,path) VALUES(1,1,'c:/music');"
                        "INSERT INTO files(id,folder_id,root_id,path,kind,size,mtime) VALUES"
                        "(1,1,1,'c:/music/a.flac','audio',1,1),(2,1,1,'c:/music/b.flac','audio',1,1),"
                        "(3,1,1,'c:/music/c.flac','audio',1,1);"
                        "INSERT INTO albums(id,folder_id,grouping_key,title,album_artist,added) VALUES(1,1,'k','Album','AA',0);"
                        "INSERT INTO tracks(id,file_id,album_id,title,artist,album) VALUES"
                        "(1,1,1,'One','Pekora','Album'),(2,2,1,'Two','Miko','Album'),(3,3,NULL,'Three','Suisei','');"
                        "INSERT INTO track_values(track_id,field,position,value) VALUES"
                        "(1,'artist',0,'Pekora'),(1,'artist',1,'Miko');");
            })
            .get();
    }

    void event(int64_t file, int64_t startedMs, int32_t offset, int64_t heardMs, bool counted) {
        store
            ->write([=](sqlite::Database& db) {
                db.prepare("INSERT INTO play_events(file_id,started,utc_offset,heard_ms,counted) VALUES(?,?,?,?,?)")
                    .bind(1, file)
                    .bind(2, startedMs)
                    .bind(3, int64_t(offset))
                    .bind(4, heardMs)
                    .bind(5, int64_t(counted))
                    .run();
            })
            .get();
    }

    int64_t scalar(const std::string& sql) { return store->openReader().scalar(sql).value_or(-1); }
};

constexpr int64_t kDay = 86'400'000;
constexpr int64_t k20251024 = 1'761'264'000'000; // 2025-10-24T00:00:00Z

TEST_F(PlayStatisticsTest, PersistCreatesOneEventAndCountsOnce) {
    PlaySession s({1}, kRate, 100'000);
    play(s, 0, 20'000);
    s.persist(*store);
    play(s, 20'000, 60'000);
    s.persist(*store);
    s.persist(*store);
    play(s, 60'000, 70'000);
    s.persist(*store);
    store->write([](sqlite::Database&) {}).get();
    EXPECT_EQ(scalar("SELECT COUNT(*) FROM play_events"), 1);
    EXPECT_EQ(scalar("SELECT heard_ms FROM play_events"), 70'000);
    EXPECT_EQ(scalar("SELECT counted FROM play_events"), 1);
    EXPECT_EQ(scalar("SELECT play_count FROM stats WHERE file_id=1"), 1);
    EXPECT_EQ(scalar("SELECT listened_ms FROM stats WHERE file_id=1"), 70'000);
    EXPECT_GT(scalar("SELECT last_played FROM stats WHERE file_id=1"), 0);

    PlaySession unheard({2}, kRate, 100'000);
    unheard.persist(*store);
    store->write([](sqlite::Database&) {}).get();
    EXPECT_EQ(scalar("SELECT COUNT(*) FROM play_events"), 1);
}

TEST_F(PlayStatisticsTest, RankingsCreditEveryArtistAndFollowTheMetric) {
    event(1, k20251024, 480, 50'000, true);
    event(1, k20251024 + 1000, 480, 50'000, true);
    event(2, k20251024 + 2000, 480, 400'000, true);
    event(3, k20251024 + 3000, 480, 10'000, false);
    PlayStatisticsQuery query;
    auto byPlays = playStatistics(*store, query);
    EXPECT_EQ(byPlays.overview.plays, 3);
    EXPECT_EQ(byPlays.overview.listenedMs, 510'000);
    EXPECT_EQ(byPlays.overview.tracks, 3);
    EXPECT_EQ(byPlays.overview.albums, 1);
    EXPECT_EQ(byPlays.overview.artists, 3);
    ASSERT_EQ(byPlays.tracks.size(), 3u);
    EXPECT_EQ(byPlays.tracks[0].track, (LibraryTrackKey{1}));
    EXPECT_EQ(byPlays.tracks[0].plays, 2);
    EXPECT_TRUE(byPlays.tracks[0].available);
    ASSERT_EQ(byPlays.albums.size(), 1u);
    EXPECT_EQ(byPlays.albums[0].plays, 3);
    EXPECT_EQ(byPlays.albums[0].listenedMs, 500'000);
    ASSERT_EQ(byPlays.artists.size(), 3u);
    EXPECT_EQ(byPlays.artists[0].name, "Miko"); // credited by both tracks
    EXPECT_EQ(byPlays.artists[0].plays, 3);
    EXPECT_EQ(byPlays.artists[1].name, "Pekora");
    EXPECT_EQ(byPlays.artists[1].plays, 2);
    query.metric = PlayMetric::ListeningTime;
    auto byTime = playStatistics(*store, query);
    EXPECT_EQ(byTime.tracks[0].track, (LibraryTrackKey{2}));
    EXPECT_EQ(byTime.tracks[0].listenedMs, 400'000);
}

TEST_F(PlayStatisticsTest, PeriodsAndBucketsUseTheRecordedLocalDay) {
    // 2025-10-23T20:00Z is 2025-10-24 04:00 in UTC+8 but still 2025-10-23 in UTC.
    event(1, k20251024 - 4 * 3'600'000, 480, 1000, true);
    event(2, k20251024 - 4 * 3'600'000, 0, 2000, true);
    event(3, k20251024 + 30 * kDay, 480, 3000, true); // 2025-11-23, a Sunday
    PlayStatisticsQuery query;
    auto days = playStatistics(*store, query).buckets;
    ASSERT_EQ(days.size(), 3u);
    EXPECT_EQ(days[0].start, "2025-10-23");
    EXPECT_EQ(days[0].listenedMs, 2000);
    EXPECT_EQ(days[1].start, "2025-10-24");
    EXPECT_EQ(days[1].listenedMs, 1000);
    EXPECT_EQ(days[2].start, "2025-11-23");
    query.bucket = PlayBucketSize::Week;
    auto weeks = playStatistics(*store, query).buckets;
    ASSERT_EQ(weeks.size(), 2u);
    EXPECT_EQ(weeks[0].start, "2025-10-20");
    EXPECT_EQ(weeks[0].plays, 2);
    EXPECT_EQ(weeks[1].start, "2025-11-17");
    query.bucket = PlayBucketSize::Month;
    auto months = playStatistics(*store, query).buckets;
    ASSERT_EQ(months.size(), 2u);
    EXPECT_EQ(months[0].start, "2025-10-01");
    EXPECT_EQ(months[1].start, "2025-11-01");
    query.period = {k20251024, k20251024 + 31 * kDay};
    const auto period = playStatistics(*store, query);
    EXPECT_EQ(period.overview.plays, 1);
    EXPECT_EQ(period.historyTotal, 1);
    query.period = {k20251024 + kDay, k20251024};
    EXPECT_THROW(playStatistics(*store, query), std::invalid_argument);
}

TEST_F(PlayStatisticsTest, HistoryIsNewestFirstAndMarksUnavailableTracks) {
    event(1, k20251024, 480, 1000, true);
    event(2, k20251024 + 1000, 480, 2000, false);
    event(3, k20251024 + 2000, 480, 3000, true);
    store->write([](sqlite::Database& db) { db.exec("UPDATE files SET available=0 WHERE id=3"); }).get();
    PlayStatisticsQuery query;
    query.historyLimit = 2;
    const auto first = playStatistics(*store, query);
    EXPECT_EQ(first.historyTotal, 3);
    ASSERT_EQ(first.history.size(), 2u);
    EXPECT_EQ(first.history[0].track, (LibraryTrackKey{3}));
    EXPECT_FALSE(first.history[0].available);
    EXPECT_EQ(first.history[0].utcOffsetMinutes, 480);
    EXPECT_EQ(first.history[1].title, "Two");
    EXPECT_FALSE(first.history[1].counted);
    query.historyOffset = 2;
    const auto second = playStatistics(*store, query);
    ASSERT_EQ(second.history.size(), 1u);
    EXPECT_EQ(second.history[0].track, (LibraryTrackKey{1}));
    EXPECT_TRUE(second.history[0].available);
}

TEST_F(PlayStatisticsTest, ResetSelectedOrAllDeletesEventsAndCounters) {
    for (const int64_t file : {1, 2, 3}) {
        PlaySession s({file}, kRate, 10'000);
        play(s, 0, 10'000);
        s.persist(*store);
    }
    resetPlayStatistics(*store, {{1}, {3}});
    EXPECT_EQ(scalar("SELECT COUNT(*) FROM play_events"), 1);
    EXPECT_EQ(scalar("SELECT file_id FROM play_events"), 2);
    EXPECT_EQ(scalar("SELECT SUM(play_count) FROM stats"), 1);
    EXPECT_EQ(scalar("SELECT listened_ms FROM stats WHERE file_id=1"), 0);
    EXPECT_EQ(scalar("SELECT COUNT(*) FROM stats WHERE last_played IS NOT NULL"), 1);
    resetPlayStatistics(*store, {});
    EXPECT_EQ(scalar("SELECT COUNT(*) FROM play_events"), 0);
    EXPECT_EQ(scalar("SELECT SUM(play_count)+SUM(listened_ms) FROM stats"), 0);
}

TEST_F(PlayStatisticsTest, DiscardedSessionStartsANewEventFromZero) {
    PlaySession s({1}, kRate, 100'000);
    play(s, 0, 60'000);
    s.persist(*store);
    resetPlayStatistics(*store, {{1}});
    s.discard();
    play(s, 60'000, 70'000);
    s.persist(*store);
    store->write([](sqlite::Database&) {}).get();
    EXPECT_EQ(scalar("SELECT COUNT(*) FROM play_events"), 1);
    EXPECT_EQ(scalar("SELECT heard_ms FROM play_events"), 10'000);
    EXPECT_EQ(scalar("SELECT listened_ms FROM stats WHERE file_id=1"), 10'000);
    EXPECT_EQ(scalar("SELECT play_count FROM stats WHERE file_id=1"), 0);
}

TEST_F(PlayStatisticsTest, ANewFileWithPlaysIsNotMergedIntoAMovedIdentity) {
    store
        ->write([](sqlite::Database& db) {
            db.exec("UPDATE files SET identity_state=1 WHERE id=3;"
                    "INSERT INTO play_events(file_id,started,utc_offset,heard_ms,counted) VALUES(3,1,0,1000,0);");
            EXPECT_EQ(library::reconcileMoves(db, {3}), 0u);
        })
        .get();
    EXPECT_EQ(scalar("SELECT COUNT(*) FROM play_events WHERE file_id=3"), 1);
}

} // namespace
