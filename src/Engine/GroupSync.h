// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// A group of this computer's outputs playing one queue (docs/Plan.md §5.12): each member has an Engine session of its
// own; the clock member plays as it would alone, and every other member keeps to it through a resampler whose ratio
// follows the difference between the two devices' clocks.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

#include "OutputBridge/IOutput.h"

namespace camel::audio {

// Where the clock member is in the music, which its output tells and the followers read: the track, the seconds into
// it its device plays, and when.
class GroupClock {
public:
    // `now` gives nanoseconds of a steady clock; the system's by default.
    explicit GroupClock(std::function<int64_t()> now = {});

    int64_t now() const { return now_(); }
    // From the clock member's output callback alone.
    void publish(uint64_t track, double seconds, int64_t at) noexcept;
    // Nothing to keep to while the clock member plays no music.
    void clear() noexcept;
    bool read(uint64_t& track, double& seconds, int64_t& at) const noexcept;

private:
    std::function<int64_t()> now_;
    std::atomic<uint64_t> sequence_{0}; // odd while a publish is under way
    std::atomic<uint64_t> track_{0};
    std::atomic<double> seconds_{0};
    std::atomic<int64_t> at_{0};
};

// A follower's correction from how far it plays behind the clock: a jump at once while it is further off than a few
// milliseconds, as when it starts, then a ratio a few hundred ppm around 1 that takes its drift out.
class DriftController {
public:
    struct Step {
        int64_t skipFrames = 0; // input frames to drop (above 0) or silence to add (below 0) at once
        double ratio = 1.0;     // input frames per output frame from now on
    };
    // `error` seconds behind the clock (ahead when negative), `elapsed` seconds since the last update.
    Step update(double error, double elapsed, uint32_t rate);
    void reset();

private:
    double integral_ = 0.0;
    double ratio_ = 1.0;
    double settling_ = 0.0; // seconds after a jump before errors are trusted again
    bool aligned_ = false;
    int far_ = 0; // updates in a row beyond the jump threshold
};

// A member's place in its group, which may change while it plays: no clock while it plays alone; the clock member tells
// `clock` where it is; a follower keeps to it, `delaySeconds` after it.
struct GroupLink {
    std::atomic<GroupClock*> clock{nullptr};
    std::atomic<bool> follower{false};
    std::atomic<double> delaySeconds{0.0};
};

// A zone's device, as every zone of this computer's outputs has it, alone or in a group. Alone or the clock it plays
// what its Engine renders untouched; as a follower it plays its Engine's frames through the drift resampler and takes no
// DSD, so its Engine converts DSD to PCM.
std::unique_ptr<IOutput> groupMemberOutput(std::unique_ptr<IOutput> device, std::shared_ptr<GroupLink> link);

} // namespace camel::audio
