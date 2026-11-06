// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// A DTS-CD file and FFmpeg's decode of it, both committed in tests/fixtures.
#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>

namespace testutil {

struct DtsCd {
    uint64_t frames = 0;          // the WAVE file's length in 16-bit stereo word frames
    std::vector<float> reference; // FFmpeg's decode of the same DTS frames: interleaved L R C LFE Ls Rs
};

// Copies a 5.1 tone as DTS-CD does: DTS frames at 1234.8 kbit/s packed 14 bits to a 16-bit word, in a 44.1 kHz 16-bit
// stereo PCM WAVE file, one word frame per sample. Throws std::runtime_error.
DtsCd copyDtsCd(const std::filesystem::path& path);

} // namespace testutil
