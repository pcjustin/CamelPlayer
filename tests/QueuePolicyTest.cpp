#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

#include "server/QueuePolicy.h"

using namespace camel::server;

namespace {

// Entry IDs are nonzero, so 0 stands for "no position".
uint64_t id(const std::optional<QueuePosition>& position) {
    return position ? position->entryId : 0;
}

} // namespace

TEST(QueuePolicyTest, PreviousRestartsAfterThreeSecondsAndOtherwiseUsesHistory) {
    QueuePolicy queue(1);
    queue.replace({1, 2, 3});
    queue.select(1);
    queue.select(2);
    queue.select(3);
    EXPECT_EQ(id(queue.previous(3'000, 1'000)), 2u);
    EXPECT_EQ(id(queue.previous(3'001, 1'000)), 2u);
    EXPECT_EQ(id(queue.previous(2'999, 1'000)), 1u);
}

TEST(QueuePolicyTest, RepeatOneAppliesOnlyToNaturalCompletion) {
    QueuePolicy queue(1);
    queue.replace({1, 2});
    queue.setRepeat(RepeatMode::One);
    queue.select(1);
    EXPECT_EQ(id(queue.next(true)), 1u);
    EXPECT_EQ(id(queue.next(false)), 2u);
}

TEST(QueuePolicyTest, RepeatAllSkipsFailuresAndExhaustsAfterOneFailedRound) {
    QueuePolicy queue(1);
    queue.replace({1, 2, 3});
    queue.setRepeat(RepeatMode::All);
    queue.select(1);
    queue.markFailed(2);
    EXPECT_EQ(id(queue.next(true)), 3u);
    queue.markFailed(3);
    queue.markFailed(1);
    EXPECT_FALSE(queue.next(true));
}

TEST(QueuePolicyTest, ReorderInvalidatesPreparedPositionAndRejectsMalformedOrder) {
    QueuePolicy queue(1);
    queue.replace({1, 2, 3});
    const auto prepared = queue.select(1);
    EXPECT_TRUE(queue.isCurrent(prepared));
    EXPECT_THROW(queue.reorder({1, 1, 3}), std::invalid_argument);
    EXPECT_THROW(queue.reorder({1, 2, 4}), std::invalid_argument);
    EXPECT_GT(queue.reorder({1, 3, 2}), prepared.revision);
    EXPECT_FALSE(queue.isCurrent(prepared));
    EXPECT_EQ(id(queue.next(false)), 3u);
}

TEST(QueuePolicyTest, RepeatAllWithOneEligibleEntryRepeatsIt) {
    QueuePolicy queue(1);
    queue.replace({1, 2, 3});
    queue.setRepeat(RepeatMode::All);
    queue.select(1);
    queue.markFailed(2);
    queue.markFailed(3);
    EXPECT_EQ(id(queue.next(true)), 1u);
    EXPECT_EQ(id(queue.next(false)), 1u);
}

TEST(QueuePolicyTest, ReplaceKeepsShuffleActive) {
    std::vector<uint64_t> ids(32);
    for (size_t i = 0; i < ids.size(); ++i) ids[i] = i + 1;
    QueuePolicy queue(7);
    queue.setShuffle(true);
    queue.replace(ids);
    std::vector<uint64_t> played;
    played.reserve(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) played.push_back(id(queue.next(true)));
    EXPECT_NE(played, ids);
    std::sort(played.begin(), played.end());
    EXPECT_EQ(played, ids);
}

TEST(QueuePolicyTest, RepeatAllShuffleRoundsVisitEveryEntry) {
    const std::vector<uint64_t> ids{1, 2, 3, 4};
    for (uint32_t seed = 1; seed <= 50; ++seed) {
        QueuePolicy queue(seed);
        queue.replace(ids);
        queue.setShuffle(true);
        queue.setRepeat(RepeatMode::All);
        std::vector<uint64_t> played;
        played.reserve(16);
        for (int i = 0; i < 16; ++i) played.push_back(id(queue.next(true)));
        for (std::ptrdiff_t round = 0; round < 4; ++round) {
            std::vector<uint64_t> entries(played.begin() + round * 4, played.begin() + round * 4 + 4);
            std::sort(entries.begin(), entries.end());
            EXPECT_EQ(entries, ids) << "seed " << seed << " round " << round;
        }
        for (size_t i = 1; i < played.size(); ++i) EXPECT_NE(played[i], played[i - 1]) << "seed " << seed;
    }
}

TEST(QueuePolicyTest, UpcomingPredictsNextWithoutChangingState) {
    QueuePolicy queue(5);
    queue.replace({1, 2, 3, 4});
    queue.setShuffle(true);
    queue.setRepeat(RepeatMode::All);
    queue.select(id(queue.next(false)));
    for (int i = 0; i < 12; ++i) {
        const auto predicted = queue.upcoming();
        const auto current = queue.current();
        ASSERT_TRUE(predicted);
        EXPECT_EQ(id(queue.current()), id(current));
        EXPECT_EQ(id(queue.next(true)), id(predicted));
    }
}

TEST(QueuePolicyTest, RepeatAllShuffleCreatesNewRoundWithoutBoundaryRepeat) {
    QueuePolicy queue(19);
    queue.replace({1, 2, 3, 4});
    queue.setShuffle(true);
    queue.setRepeat(RepeatMode::All);
    const auto first = queue.select(id(queue.next(false)));
    uint64_t previous = first.entryId;
    for (int i = 0; i < 7; ++i) {
        const auto next = queue.next(true);
        ASSERT_TRUE(next);
        EXPECT_NE(id(next), previous);
        previous = id(next);
    }
}
