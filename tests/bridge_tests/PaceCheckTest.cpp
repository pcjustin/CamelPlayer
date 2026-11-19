// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Whether a device takes audio far faster than it plays it, as the LG monitor's display audio did: about 230 times
// faster in WASAPI and KS event modes, twice to six times when polled.
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>

#include "OutputBridge/PaceCheck.h"

using camel::audio::PaceCheck;
using namespace std::chrono_literals;

namespace {

constexpr uint32_t kRate = 44100;
constexpr uint32_t kPeriod = 448; // the display audio's aligned period at 44.1 kHz

// Feeds the check a period every `step` from a full buffer at the start, as an output primes the device, for `audio`
// seconds of frames; the frames taken when it first said too fast, or 0.
uint64_t takenWhenCaught(PaceCheck& check, std::chrono::nanoseconds step, double audio, uint32_t primed = kPeriod) {
    auto now = PaceCheck::Clock::time_point{} + 1h;
    check.restart(now);
    uint64_t taken = primed;
    if (check.took(primed, now)) return taken;
    while (taken < uint64_t(audio * kRate)) {
        now += step;
        taken += kPeriod;
        if (check.took(kPeriod, now)) return taken;
    }
    return 0;
}

std::chrono::nanoseconds periodAt(double speed) {
    return std::chrono::nanoseconds(int64_t(1e9 * kPeriod / kRate / speed));
}

} // namespace

TEST(PaceCheck, ADeviceThatPlaysInRealTimeIsNeverCaught) {
    PaceCheck check(kRate, 1000ms + 210ms);
    EXPECT_EQ(takenWhenCaught(check, periodAt(1.0), 600.0, 9216), 0U) << "a polled WASAPI buffer of 209 ms, primed full";
}

TEST(PaceCheck, ADeviceALittleFastOrHeldUpIsNotCaught) {
    PaceCheck check(kRate, 1000ms + 10ms);
    EXPECT_EQ(takenWhenCaught(check, periodAt(1.05), 600.0), 0U) << "5% fast is half a second ahead in each 10 s";
    PaceCheck slow(kRate, 1000ms + 10ms);
    EXPECT_EQ(takenWhenCaught(slow, periodAt(0.5), 60.0), 0U);
}

TEST(PaceCheck, AClockThatDriftsIsNotCaughtOverHoursOfPlaying) {
    PaceCheck check(kRate, 1000ms + 10ms);
    EXPECT_EQ(takenWhenCaught(check, periodAt(1.0001), 4 * 3600.0), 0U) << "100 ppm fast is 1.4 s ahead after 4 hours";
}

TEST(PaceCheck, ADeviceThatTakesAudioAsFastAsItIsGivenIsCaughtWithinItsSlack) {
    PaceCheck check(kRate, 1000ms + 10ms);
    const uint64_t taken = takenWhenCaught(check, periodAt(230.0), 600.0);
    EXPECT_GT(taken, uint64_t(1.0 * kRate));
    EXPECT_LT(taken, uint64_t(1.1 * kRate)) << "caught about a second of audio in, not after the track";
}

TEST(PaceCheck, ADevicePolledTwiceAsFastIsCaughtToo) {
    PaceCheck check(kRate, 1000ms + 23ms);
    const uint64_t taken = takenWhenCaught(check, periodAt(2.2), 600.0);
    EXPECT_GT(taken, 0U);
    EXPECT_LT(taken, uint64_t(2.0 * kRate));
}

TEST(PaceCheck, ARestartCountsFromTheNewStart) {
    PaceCheck check(kRate, 1000ms + 10ms);
    auto now = PaceCheck::Clock::time_point{} + 1h;
    check.restart(now);
    for (int i = 0; i < 500; ++i) EXPECT_FALSE(check.took(kPeriod, now += periodAt(1.0)));
    now += 30s; // paused
    check.restart(now);
    EXPECT_FALSE(check.took(uint64_t{kPeriod} * 4, now)) << "the buffer primed again on resuming";
    for (int i = 0; i < 500; ++i) EXPECT_FALSE(check.took(kPeriod, now += periodAt(1.0)));
}
