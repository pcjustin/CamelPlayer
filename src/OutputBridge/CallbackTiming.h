// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// How regularly a driver calls back. A callback that comes clearly later than the driver's own rhythm means its thread
// was held up, which the device may play as a gap even though the Engine had audio ready (no underrun).
#pragma once
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>

#include "OutputBridge/IOutput.h"

namespace camel::audio {

class CallbackTiming {
public:
    // Before the callbacks of a new stream or after a pause: `periodMs` is the buffer period.
    void restart(double periodMs) noexcept {
        periodUs_.store(uint64_t(periodMs * 1000.0), std::memory_order_relaxed);
        restart_.store(true, std::memory_order_release);
    }

    // A new stream: also clears the counts.
    void reset(double periodMs) noexcept {
        late_.store(0, std::memory_order_relaxed);
        longestUs_.store(0, std::memory_order_relaxed);
        priority_.store(kUnknown, std::memory_order_relaxed);
        restart(periodMs);
    }

    // Driver thread, once per callback. Never blocks or allocates.
    void tick() noexcept {
        const auto now = std::chrono::steady_clock::now();
        const auto gap = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - last_).count());
        last_ = now;
        if (restart_.exchange(false, std::memory_order_acq_rel)) {
            priority_.store(GetThreadPriority(GetCurrentThread()), std::memory_order_relaxed);
            settling_ = kSettlingCallbacks;
            const uint64_t period = std::max<uint64_t>(1, periodUs_.load(std::memory_order_relaxed));
            learning_ = uint32_t(kLearningUs / period) + 1;
            limitUs_ = period * 3 / 2;
            return;
        }
        if (settling_ > 0) {
            --settling_;
        } else if (learning_ > 0) {
            // USB drivers often call back in bursts (several buffers, then a pause of tens of ms) at high rates; that
            // rhythm is their normal, so only gaps clearly beyond it count.
            --learning_;
            limitUs_ = std::max(limitUs_, gap * 5 / 4);
        } else if (gap > limitUs_) {
            late_.fetch_add(1, std::memory_order_relaxed);
            if (gap > longestUs_.load(std::memory_order_relaxed)) longestUs_.store(gap, std::memory_order_relaxed);
        }
    }

    // Adds the measurements to a status; the detail only once the driver has called back.
    void report(OutputStatus& s) const {
        s.lateCallbacks = late_.load(std::memory_order_relaxed);
        s.longestGapMs = double(longestUs_.load(std::memory_order_relaxed)) / 1000.0;
        s.periodMs = double(periodUs_.load(std::memory_order_relaxed)) / 1000.0;
        const int priority = priority_.load(std::memory_order_relaxed);
        if (priority == kUnknown) return;
        char text[128];
        const int length = snprintf(text, sizeof text, ", callback thread priority %d, late callbacks %llu (longest gap %.1f ms)",
                                    priority, (unsigned long long)s.lateCallbacks, s.longestGapMs);
        if (length > 0 && size_t(length) < sizeof text) s.detail += text;
    }

private:
    static constexpr int kUnknown = THREAD_PRIORITY_ERROR_RETURN;
    static constexpr int kSettlingCallbacks = 8; // drivers often call back irregularly right after starting
    static constexpr uint64_t kLearningUs = 1000000;
    std::atomic<uint64_t> periodUs_{0}, late_{0}, longestUs_{0};
    std::atomic<bool> restart_{true};
    std::atomic<int> priority_{kUnknown};
    // Driver thread only.
    int settling_ = 0;
    uint32_t learning_ = 0;
    uint64_t limitUs_ = 0;
    std::chrono::steady_clock::time_point last_;
};

} // namespace camel::audio
