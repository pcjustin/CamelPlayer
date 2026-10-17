#include <gtest/gtest.h>

#include "server/QueuePolicy.h"

using namespace camel::server;

TEST(QueuePolicyTest, PreviousRestartsAfterThreeSecondsAndOtherwiseUsesHistory) {
    QueuePolicy queue(1);
    queue.replace({1, 2, 3});
    queue.select(1);
    queue.select(2);
    queue.select(3);
    EXPECT_EQ(queue.previous(3'000, 1'000)->entryId, 2u);
    EXPECT_EQ(queue.previous(3'001, 1'000)->entryId, 2u);
    EXPECT_EQ(queue.previous(2'999, 1'000)->entryId, 1u);
}

TEST(QueuePolicyTest, RepeatOneAppliesOnlyToNaturalCompletion) {
    QueuePolicy queue(1);
    queue.replace({1, 2});
    queue.setRepeat(RepeatMode::One);
    queue.select(1);
    EXPECT_EQ(queue.next(true)->entryId, 1u);
    EXPECT_EQ(queue.next(false)->entryId, 2u);
}

TEST(QueuePolicyTest, RepeatAllSkipsFailuresAndExhaustsAfterOneFailedRound) {
    QueuePolicy queue(1);
    queue.replace({1, 2, 3});
    queue.setRepeat(RepeatMode::All);
    queue.select(1);
    queue.markFailed(2);
    EXPECT_EQ(queue.next(true)->entryId, 3u);
    queue.markFailed(3);
    queue.markFailed(1);
    EXPECT_FALSE(queue.next(true));
}

TEST(QueuePolicyTest, ReorderInvalidatesPreparedPositionAndRejectsMalformedOrder) {
    QueuePolicy queue(1);
    queue.replace({1, 2, 3});
    const auto prepared = queue.select(1).value();
    EXPECT_TRUE(queue.isCurrent(prepared));
    EXPECT_THROW(queue.reorder({1, 1, 3}), std::invalid_argument);
    EXPECT_THROW(queue.reorder({1, 2, 4}), std::invalid_argument);
    EXPECT_GT(queue.reorder({1, 3, 2}), prepared.revision);
    EXPECT_FALSE(queue.isCurrent(prepared));
    EXPECT_EQ(queue.next(false)->entryId, 3u);
}

TEST(QueuePolicyTest, RepeatAllShuffleCreatesNewRoundWithoutBoundaryRepeat) {
    QueuePolicy queue(19);
    queue.replace({1, 2, 3, 4});
    queue.setShuffle(true);
    queue.setRepeat(RepeatMode::All);
    const auto first = queue.select(queue.next(false)->entryId);
    uint64_t previous = first->entryId;
    for (int i = 0; i < 7; ++i) {
        const auto next = queue.next(true);
        ASSERT_TRUE(next);
        EXPECT_NE(next->entryId, previous);
        previous = next->entryId;
    }
}
