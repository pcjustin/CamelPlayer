// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include "Common/AudioTypes.h"
#include "Engine/decode/IDecoder.h"

using namespace camel::audio;

TEST(Smoke, DecoderIdsRoundTrip) {
    for (DecoderKind k : kAllDecoders) {
        DecoderKind back;
        ASSERT_TRUE(decoderFromId(decoderId(k), back));
        EXPECT_EQ(back, k);
    }
}

TEST(Smoke, SampleTypeSizes) {
    EXPECT_EQ(bytesPerSample(SampleType::Int24), 3u);
    EXPECT_EQ(validBits(SampleType::Int24In32), 24u);
    EXPECT_EQ((DeviceFormat{96000, 2, SampleType::Int24}).bytesPerFrame(), 6u);
}
