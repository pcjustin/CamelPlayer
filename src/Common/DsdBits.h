// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include <cstdint>

namespace camel::audio {

// A DSD byte with its eight samples in the other order: oldest in the least significant bit, as an LSB-first DSF
// stores them and ASIOSTDSDInt8LSB1 wants them, or back to the Engine's oldest in the most significant bit.
constexpr uint8_t reverseDsdBits(uint8_t b) {
    b = uint8_t((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = uint8_t((b & 0xCC) >> 2 | (b & 0x33) << 2);
    return uint8_t((b & 0xAA) >> 1 | (b & 0x55) << 1);
}

} // namespace camel::audio
