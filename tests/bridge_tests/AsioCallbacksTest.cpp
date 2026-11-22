// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include <array>
#include <set>

#include "OutputBridge/AsioCallbacks.h"

using namespace camel::audio;

namespace {

struct Target final : asio::CallbackTarget {
    long switched = -1, selector = 0;
    int rateChanges = 0;
    void onBufferSwitch(long index) noexcept override { switched = index; }
    void onRateChanged() noexcept override { ++rateChanges; }
    long onMessage(long s, long) noexcept override {
        selector = s;
        return 7;
    }
};

} // namespace

// Two drivers at once (docs/Plan.md §5.12): each output's callbacks reach it alone, as one shared target once sent every
// driver's callbacks to the output opened last.
TEST(AsioCallbacks, EachBoundOutputsCallbacksReachItAlone) {
    std::array<Target, asio::kCallbackSlots> targets;
    std::array<asio::Callbacks*, asio::kCallbackSlots> callbacks{};
    std::set<asio::Callbacks*> distinct;
    for (size_t i = 0; i < targets.size(); ++i) {
        callbacks[i] = asio::bindCallbacks(&targets[i]);
        ASSERT_NE(callbacks[i], nullptr) << i;
        distinct.insert(callbacks[i]);
    }
    EXPECT_EQ(distinct.size(), targets.size());
    for (size_t i = 0; i < targets.size(); ++i) {
        callbacks[i]->bufferSwitch(long(i), asio::kTrue);
        EXPECT_EQ(callbacks[i]->bufferSwitchTimeInfo(nullptr, long(i) + 10, asio::kTrue), nullptr);
        callbacks[i]->sampleRateChanged(48000);
        EXPECT_EQ(callbacks[i]->message(long(i) + 100, 0, nullptr, nullptr), 7);
    }
    for (size_t i = 0; i < targets.size(); ++i) {
        EXPECT_EQ(targets[i].switched, long(i) + 10) << i;
        EXPECT_EQ(targets[i].rateChanges, 1) << i;
        EXPECT_EQ(targets[i].selector, long(i) + 100) << i;
    }
    for (auto& target : targets) asio::releaseCallbacks(&target);
}

TEST(AsioCallbacks, AFullSetRefusesAnotherAndAReleasedSlotIsTakenAgain) {
    std::array<Target, asio::kCallbackSlots> targets;
    std::array<asio::Callbacks*, asio::kCallbackSlots> callbacks{};
    for (size_t i = 0; i < targets.size(); ++i) callbacks[i] = asio::bindCallbacks(&targets[i]);
    Target more;
    EXPECT_EQ(asio::bindCallbacks(&more), nullptr);
    asio::releaseCallbacks(&targets[1]);
    callbacks[1]->bufferSwitch(5, asio::kTrue); // reaches nothing once released
    EXPECT_EQ(targets[1].switched, -1);
    EXPECT_EQ(callbacks[1]->message(1, 0, nullptr, nullptr), 0);
    EXPECT_EQ(asio::bindCallbacks(&more), callbacks[1]);
    callbacks[1]->bufferSwitch(3, asio::kTrue);
    EXPECT_EQ(more.switched, 3);
    asio::releaseCallbacks(&more);
    for (auto& target : targets) asio::releaseCallbacks(&target);
}
