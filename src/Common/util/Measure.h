// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Measurements for docs/Plan.md §12.1. Double-click to audible output: the Control point marks the request, the Engine
// logs the delay once the device has taken the first frames of music. Search latency and frame times are logged only
// when CAMELPLAYER_MEASURE is set.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <windows.h>

namespace camel::measure {

inline std::atomic<int64_t> playRequested{0}; // steady_clock ticks; 0 = no pending request

inline int64_t now() {
    return std::chrono::steady_clock::now().time_since_epoch().count();
}

inline void markPlayRequested() {
    playRequested.store(now(), std::memory_order_relaxed);
}

inline double milliseconds(int64_t from, int64_t to) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::duration(to - from)).count();
}

inline bool measuring() {
    static const bool enabled = GetEnvironmentVariableW(L"CAMELPLAYER_MEASURE", nullptr, 0) > 1;
    return enabled;
}

} // namespace camel::measure
