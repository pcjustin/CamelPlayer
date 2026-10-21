// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Saved output parameters the device cannot take (docs/Plan.md §2, M2-R10): the driver's default is used instead, the
// status area shows requested ≠ active, and the saved setting is never overwritten.
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>

namespace camel::audio {

struct AsioBufferChoice {
    long size = 0;
    bool adjusted = false; // a requested size the driver cannot use
};

// `requested` 0 means the driver's preferred size. A size outside min..max or off the granularity (-1: powers of two)
// is not rounded to a near value: the preferred size is used and reported.
inline AsioBufferChoice asioBufferSize(long requested, long minSize, long maxSize, long preferred, long granularity) {
    if (requested <= 0 || minSize == maxSize) return {preferred, requested > 0 && requested != preferred};
    bool valid = requested >= minSize && requested <= maxSize;
    if (valid && granularity > 0) valid = (requested - minSize) % granularity == 0;
    if (valid && granularity == -1) valid = (requested & (requested - 1)) == 0;
    return valid ? AsioBufferChoice{requested, false} : AsioBufferChoice{preferred, true};
}

struct ChannelChoice {
    int left = 0, right = 1;
    bool adjusted = false;
};

// Zero-based output channels; a pair the driver does not have falls back to channels 1-2.
inline ChannelChoice asioChannels(int left, int right, long outputs) {
    if (left >= 0 && right >= 0 && left < outputs && right < outputs && left != right) return {left, right, false};
    return {0, 1, true};
}

inline std::string requestedVsActive(const char* what, const std::string& requested, const std::string& active) {
    return std::string(what) + ": requested " + requested + ", using " + active;
}

} // namespace camel::audio
