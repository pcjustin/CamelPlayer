// ASIO channel buffer layouts (docs/Plan.md §5.6, §5.8): PCM samples and native DSD bytes split per channel, with
// the bit order DSDInt8LSB1 drivers expect.
#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "OutputBridge/AsioLayout.h"

using namespace camel::audio;

TEST(AsioLayout, SplitsPcmSamplesPerChannel) {
    const std::array<uint8_t, 12> interleaved{0x01, 0x02, 0x03, 0x11, 0x12, 0x13, 0x21, 0x22, 0x23, 0x31, 0x32, 0x33};
    std::array<uint8_t, 6> left{}, right{};
    splitStereo(interleaved.data(), 2, 3, false, left.data(), right.data());
    EXPECT_EQ(left, (std::array<uint8_t, 6>{0x01, 0x02, 0x03, 0x21, 0x22, 0x23}));
    EXPECT_EQ(right, (std::array<uint8_t, 6>{0x11, 0x12, 0x13, 0x31, 0x32, 0x33}));
}

TEST(AsioLayout, NativeDsdKeepsMsbFirstBytesAndReversesForLsbFirstDrivers) {
    const std::array<uint8_t, 4> interleaved{0x69, 0x80, 0x01, 0xC3};
    std::array<uint8_t, 2> left{}, right{};
    splitStereo(interleaved.data(), 2, 1, false, left.data(), right.data());
    EXPECT_EQ(left, (std::array<uint8_t, 2>{0x69, 0x01}));
    EXPECT_EQ(right, (std::array<uint8_t, 2>{0x80, 0xC3}));
    splitStereo(interleaved.data(), 2, 1, true, left.data(), right.data());
    EXPECT_EQ(left, (std::array<uint8_t, 2>{0x96, 0x80}));
    EXPECT_EQ(right, (std::array<uint8_t, 2>{0x01, 0xC3}));
    for (int b = 0; b < 256; b++) EXPECT_EQ(reverseDsdBits(reverseDsdBits(uint8_t(b))), b);
}
