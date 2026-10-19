// Saved output parameters the device cannot take fall back to the driver's choice and say so (docs/Plan.md M2-R10).
#include <gtest/gtest.h>

#include "OutputBridge/OutputChoices.h"

using namespace camel::audio;

TEST(OutputChoices, AsioBufferOutsideTheDriverRangeUsesThePreferredSize) {
    const auto pick = [](long requested, long granularity) { return asioBufferSize(requested, 64, 2048, 512, granularity); };
    EXPECT_EQ(pick(0, 64).size, 512);
    EXPECT_FALSE(pick(0, 64).adjusted);
    EXPECT_EQ(pick(256, 64).size, 256);
    EXPECT_FALSE(pick(256, 64).adjusted);
    for (const long invalid : {32L, 4096L, 100L}) {
        const auto choice = pick(invalid, 64);
        EXPECT_EQ(choice.size, 512) << invalid;
        EXPECT_TRUE(choice.adjusted) << invalid;
    }
    EXPECT_FALSE(pick(1024, -1).adjusted); // powers of two
    EXPECT_TRUE(pick(1000, -1).adjusted);
    EXPECT_FALSE(pick(1000, 0).adjusted); // no granularity
    // A driver with one fixed size reports any other request.
    EXPECT_EQ(asioBufferSize(1024, 32768, 32768, 32768, 0).size, 32768);
    EXPECT_TRUE(asioBufferSize(1024, 32768, 32768, 32768, 0).adjusted);
    EXPECT_FALSE(asioBufferSize(32768, 32768, 32768, 32768, 0).adjusted);
}

TEST(OutputChoices, MissingAsioOutputChannelsFallBackToOneAndTwo) {
    const auto kept = asioChannels(2, 3, 8);
    EXPECT_EQ(kept.left, 2);
    EXPECT_EQ(kept.right, 3);
    EXPECT_FALSE(kept.adjusted);
    for (const auto& [left, right] : {std::pair{4, 5}, std::pair{1, 1}, std::pair{-1, 0}}) {
        const auto choice = asioChannels(left, right, 4);
        EXPECT_EQ(choice.left, 0);
        EXPECT_EQ(choice.right, 1);
        EXPECT_TRUE(choice.adjusted) << left << "/" << right;
    }
    EXPECT_EQ(requestedVsActive("ASIO buffer", "100", "512"), "ASIO buffer: requested 100, using 512");
}
