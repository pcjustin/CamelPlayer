// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Whether a device takes audio far faster than it plays it. An endpoint whose clock runs free, as a monitor's display
// audio can, asks for the next buffer as soon as it has the last, and playback would race through the queue in silence.
#pragma once
#include <chrono>
#include <cstdint>

namespace camel::audio {

// The error's detail, which the Control point names in the user's language.
inline constexpr char kDeviceRunsFast[] = "the device takes audio far faster than it plays it; it may be asleep or not connected";

// Counts the frames a device takes against the time that passes, from its start and then afresh every 10 s, so that a
// clock a little off never adds up to a verdict. Real-time safe.
class PaceCheck {
public:
    using Clock = std::chrono::steady_clock;

    // `slack`: how far ahead of real time the device may get, its whole buffer included.
    PaceCheck(uint32_t sampleRate, std::chrono::milliseconds slack) : rate_(sampleRate), slack_(slack) {}

    // At each start, before the device is primed.
    void restart(Clock::time_point now) {
        since_ = now;
        frames_ = 0;
    }

    // True once the frames taken are more than the slack ahead of the time since the count began.
    bool took(uint64_t frames, Clock::time_point now) {
        if (now - since_ > kWindow) restart(now);
        frames_ += frames;
        const std::chrono::duration<double> taken(double(frames_) / rate_);
        return taken - (now - since_) > slack_;
    }

private:
    static constexpr auto kWindow = std::chrono::seconds(10);

    uint32_t rate_;
    std::chrono::milliseconds slack_;
    Clock::time_point since_;
    uint64_t frames_ = 0;
};

} // namespace camel::audio
