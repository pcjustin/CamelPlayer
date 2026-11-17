// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Digital silence as libFLAC encodes it, every frame two CONSTANT subframes, through every FLAC decoder.
#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "Common/AudioFormat.h"
#include "Engine/decode/IDecoder.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::audio;

TEST(FlacSilence, EveryDecoderReadsLibFlacSilenceWhole) {
    // A sine far below one quantization step encodes as zeros: libFLAC writes each frame as CONSTANT subframes.
    const auto file = testutil::generatedFile({"flac-silence", 44100, 16, 2, 30.0, true, -400});
    for (const auto kind : decodersFor(AudioFormat::Flac)) {
        SCOPED_TRACE(decoderName(kind));
        std::unique_ptr<IDecoder> decoder;
        ASSERT_NO_THROW(decoder = openDecoder(kind, file));
        EXPECT_EQ(decoder->info().totalFrames, 44100u * 30);
        std::vector<int32_t> buffer(size_t(4096) * 2);
        uint64_t frames = 0;
        bool silent = true;
        while (const size_t n = decoder->read(buffer.data(), 4096)) {
            frames += n;
            silent =
                silent && std::all_of(buffer.begin(), buffer.begin() + std::ptrdiff_t(n * 2), [](int32_t s) { return s == 0; });
        }
        EXPECT_EQ(frames, 44100u * 30);
        EXPECT_TRUE(silent);
    }
}
