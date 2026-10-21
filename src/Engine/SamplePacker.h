// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Packs right-aligned integer samples into a device layout without changing any bit (docs/Plan.md §5.2, §16.2).
#pragma once
#include <cstddef>
#include <cstdint>

#include "Common/AudioTypes.h"

namespace camel::audio {

// True when `type` can carry 16/24-bit integer samples losslessly by zero padding.
bool canCarryBitPerfect(SampleType type, uint32_t bits);

// Packs `count` samples (right-aligned to `bits`) into `dst` as `type`. Requires canCarryBitPerfect(type, bits).
void packSamples(const int32_t* src, size_t count, uint32_t bits, SampleType type, uint8_t* dst);

// Inverse of packSamples, used by tests and capture comparison.
void unpackSamples(const uint8_t* src, size_t count, uint32_t bits, SampleType type, int32_t* dst);

} // namespace camel::audio
