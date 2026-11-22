// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "OutputBridge/AsioCallbacks.h"

#include <array>
#include <atomic>
#include <utility>

namespace camel::audio::asio {
namespace {

std::array<std::atomic<CallbackTarget*>, kCallbackSlots> g_targets{};

template <size_t N> struct Slot {
    static void bufferSwitch(long index, Bool) {
        if (auto* target = g_targets[N].load(std::memory_order_acquire)) target->onBufferSwitch(index);
    }
    static void rateChanged(SampleRate) {
        if (auto* target = g_targets[N].load(std::memory_order_acquire)) target->onRateChanged();
    }
    static long message(long selector, long value, void*, double*) {
        auto* target = g_targets[N].load(std::memory_order_acquire);
        return target ? target->onMessage(selector, value) : 0;
    }
    static Time* bufferSwitchTimeInfo(Time* time, long index, Bool) {
        if (auto* target = g_targets[N].load(std::memory_order_acquire)) target->onBufferSwitch(index);
        return time;
    }
};

template <size_t... N> constexpr std::array<Callbacks, sizeof...(N)> slotCallbacks(std::index_sequence<N...>) noexcept {
    return {Callbacks{&Slot<N>::bufferSwitch, &Slot<N>::rateChanged, &Slot<N>::message, &Slot<N>::bufferSwitchTimeInfo}...};
}

// The drivers keep pointers to these for as long as their buffers exist.
constinit std::array<Callbacks, kCallbackSlots> g_callbacks = slotCallbacks(std::make_index_sequence<kCallbackSlots>{});

} // namespace

Callbacks* bindCallbacks(CallbackTarget* target) noexcept {
    for (size_t i = 0; i < kCallbackSlots; ++i) {
        CallbackTarget* none = nullptr;
        if (g_targets[i].compare_exchange_strong(none, target, std::memory_order_acq_rel)) return &g_callbacks[i];
    }
    return nullptr;
}

void releaseCallbacks(CallbackTarget* target) noexcept {
    for (auto& slot : g_targets) {
        CallbackTarget* bound = target;
        slot.compare_exchange_strong(bound, nullptr, std::memory_order_acq_rel);
    }
}

} // namespace camel::audio::asio
