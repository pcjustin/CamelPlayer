// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// What an output keeps of the frames it wrote, for a pause that empties the device.
#include <gtest/gtest.h>

#include <numeric>
#include <vector>

#include "OutputBridge/KeptFrames.h"

using camel::audio::KeptFrames;

namespace {

// Frames of one byte each, numbered from `first`.
std::vector<uint8_t> frames(uint8_t first, size_t count) {
    std::vector<uint8_t> out(count);
    std::iota(out.begin(), out.end(), first);
    return out;
}

} // namespace

TEST(KeptFrames, APauseKeepsTheFramesTheDeviceHadNotPlayedInOrder) {
    KeptFrames kept;
    kept.reset(8, 1);
    const auto a = frames(1, 6);
    const auto b = frames(7, 5); // wraps the ring of 8
    kept.wrote(a.data(), a.size());
    kept.wrote(b.data(), b.size());
    kept.unplayed(5); // frames 7 to 11 were still in the device
    std::vector<uint8_t> out(3);
    EXPECT_EQ(kept.take(out.data(), 3), 3u);
    EXPECT_EQ(out, frames(7, 3));
    EXPECT_EQ(kept.take(out.data(), 3), 2u);
    EXPECT_EQ(std::vector<uint8_t>(out.begin(), out.begin() + 2), frames(10, 2));
    EXPECT_EQ(kept.take(out.data(), 3), 0u) << "each kept frame is written once";
}

TEST(KeptFrames, NoMoreIsKeptThanWasWrittenSinceThePause) {
    KeptFrames kept;
    kept.reset(4, 2);
    const std::vector<uint8_t> one{1, 2, 3, 4}; // two frames of two bytes
    kept.wrote(one.data(), 2);
    kept.unplayed(10);
    std::vector<uint8_t> out(8);
    EXPECT_EQ(kept.take(out.data(), 4), 2u);
    EXPECT_EQ(std::vector<uint8_t>(out.begin(), out.begin() + 4), one);
    kept.unplayed(4); // a second pause before anything new was written
    EXPECT_EQ(kept.take(out.data(), 4), 0u);
    const auto many = frames(0, 20); // more than the ring: the last four frames remain
    kept.wrote(many.data(), 10);
    kept.unplayed(4);
    EXPECT_EQ(kept.take(out.data(), 4), 4u);
    EXPECT_EQ(out, frames(12, 8));
}
