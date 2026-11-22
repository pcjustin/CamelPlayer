// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// ASIO callbacks carry no user pointer (docs/Plan.md §5.6): each open output takes one of a fixed set of slots, whose
// static callbacks reach only it, so several drivers play at once (docs/Plan.md §5.12).
#pragma once
#include <cstddef>

#include "OutputBridge/AsioAbi.h"

namespace camel::audio::asio {

// What a slot's callbacks reach, on the driver's thread.
class CallbackTarget {
public:
    virtual void onBufferSwitch(long index) noexcept = 0;
    virtual void onRateChanged() noexcept = 0;
    virtual long onMessage(long selector, long value) noexcept = 0;

protected:
    CallbackTarget() = default;
    ~CallbackTarget() = default;
    CallbackTarget(const CallbackTarget&) = default;
    CallbackTarget& operator=(const CallbackTarget&) = default;
};

inline constexpr size_t kCallbackSlots = 4;

// A free slot's callbacks, reaching `target` until released; nullptr when every slot is taken.
Callbacks* bindCallbacks(CallbackTarget* target) noexcept;
// Its slot reaches nothing from then on; nothing when it holds none.
void releaseCallbacks(CallbackTarget* target) noexcept;

} // namespace camel::audio::asio
