// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "ControlPoint/ConsentWait.h"

using namespace std::chrono_literals;

// A Server busy for longer than two seconds still applies the answer; the lookup waits for it, not for a fixed time.
TEST(ConsentWait, AnAnswerAppliedLateIsWaitedFor) {
    std::atomic<bool> applied = false;
    std::thread server([&] {
        std::this_thread::sleep_for(4s);
        applied = true;
    });
    const camel::server::CancellationToken token;
    EXPECT_TRUE(ui::consentApplied([&] { return applied.load(); }, token));
    server.join();
}

// Leaving the page ends the wait.
TEST(ConsentWait, ACancelledWaitEndsAtOnce) {
    camel::server::CancellationToken token;
    token.cancel();
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(ui::consentApplied([] { return false; }, token));
    EXPECT_LT(std::chrono::steady_clock::now() - start, 500ms);
}
