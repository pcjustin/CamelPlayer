// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// A group of this computer's outputs (docs/Plan.md §5.12): two devices on clocks of their own, run in simulated time, the
// follower kept to the clock by its drift resampler.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

#include "Engine/GroupSync.h"

using namespace camel;
using namespace camel::audio;

namespace {

constexpr uint32_t kRate = 44100;
constexpr size_t kBuffer = 882; // the fake devices' buffer, 20 ms

// Music whose every frame tells its own moment: the left channel is sin(2πt) and the right cos(2πt) for t seconds into
// the track, so the moment a device plays can be read back from its samples to within nanoseconds of a second.
class MomentSource final : public IRenderSource {
public:
    explicit MomentSource(double start = 0.0) : frame_(int64_t(std::llround(start * kRate))) {}
    size_t render(uint8_t* dst, size_t frames) noexcept override {
        if (!playing_) return 0;
        for (size_t i = 0; i < frames; ++i) {
            const double t = double(frame_ + int64_t(i)) / kRate;
            const float sample[2] = {float(std::sin(2 * std::numbers::pi * t) * 0.5),
                                     float(std::cos(2 * std::numbers::pi * t) * 0.5)};
            std::memcpy(dst + 8 * i, sample, 8);
        }
        frame_ += int64_t(frames);
        return frames;
    }
    bool renderedPoint(RenderedPoint& point) const noexcept override {
        point = {7, double(frame_) / kRate};
        return true;
    }
    bool playing_ = true;

private:
    int64_t frame_;
};

// A device that asks its source for frames as its own clock runs, `ppm` off the system's, and keeps what it was given.
class FakeDevice final : public IOutput {
public:
    explicit FakeDevice(double ppm) : rate_(kRate * (1 + ppm * 1e-6)) {}
    bool supports(const DeviceFormat& f) override { return f.type != SampleType::Dsd8; }
    void open(const DeviceFormat& f, IRenderSource* source) override {
        format_ = f;
        source_ = source;
    }
    void start() override {}
    void pause() override {}
    void close() override { source_ = nullptr; }
    OutputStatus status() const override {
        OutputStatus s;
        s.bufferFrames = uint32_t(kBuffer);
        return s;
    }
    // The device's clock moves on by `seconds`: it asks for the frames that played meanwhile.
    void run(double seconds) {
        owed_ += rate_ * seconds;
        const auto frames = size_t(owed_);
        owed_ -= double(frames);
        block_.assign(frames * format_.bytesPerFrame(), 0);
        if (source_) source_->render(block_.data(), frames);
        for (size_t i = 0; i < frames; ++i) {
            float s[2];
            std::memcpy(s, block_.data() + 8 * i, 8);
            recent_.push_back({s[0], s[1]});
        }
        if (recent_.size() > 4 * kBuffer) recent_.erase(recent_.begin(), recent_.end() - std::ptrdiff_t(2 * kBuffer));
    }
    // The moment of the track the device plays now, modulo a second: the frame written a buffer ago.
    double playing() const {
        const auto& s = recent_[recent_.size() - 1 - kBuffer];
        return std::atan2(s[0], s[1]) / (2 * std::numbers::pi);
    }
    std::vector<uint8_t> block_;

private:
    double rate_, owed_ = 0;
    DeviceFormat format_;
    IRenderSource* source_ = nullptr;
    std::vector<std::array<float, 2>> recent_;
};

// How far `follower` plays behind `clock`, in seconds, the moments being known modulo one.
double behind(double clock, double follower) {
    double d = clock - follower;
    return d - std::round(d);
}

struct Group {
    int64_t now = 0;
    std::shared_ptr<GroupClock> clock = std::make_shared<GroupClock>([this] { return now; });
    FakeDevice* clockDevice = nullptr;
    FakeDevice* followerDevice = nullptr;
    std::shared_ptr<GroupLink> clockLink = std::make_shared<GroupLink>(), followerLink = std::make_shared<GroupLink>();
    std::unique_ptr<IOutput> clockOut, followerOut;
    MomentSource clockSource, followerSource;

    Group(double clockPpm, double followerPpm, double delay = 0.0, double followerStart = 0.0) : followerSource(followerStart) {
        auto a = std::make_unique<FakeDevice>(clockPpm);
        auto b = std::make_unique<FakeDevice>(followerPpm);
        clockDevice = a.get();
        followerDevice = b.get();
        clockLink->clock = clock.get();
        followerLink->clock = clock.get();
        followerLink->follower = true;
        followerLink->delaySeconds = delay;
        clockOut = groupMemberOutput(std::move(a), clockLink);
        followerOut = groupMemberOutput(std::move(b), followerLink);
        const DeviceFormat format{kRate, 2, SampleType::Float32};
        clockOut->open(format, &clockSource);
        followerOut->open(format, &followerSource);
        clockOut->start();
        followerOut->start();
    }
    // Both devices run for `seconds` in 10 ms periods.
    void run(double seconds) {
        for (int i = 0; i < int(std::lround(seconds * 100)); ++i) {
            now += 10'000'000;
            clockDevice->run(0.01);
            followerDevice->run(0.01);
        }
    }
};

} // namespace

// Two devices whose clocks are 100 ppm apart stay within a millisecond of each other over an hour, once the follower
// has caught up with the clock in its first minute.
TEST(GroupSync, ClocksAHundredPpmApartStayWithinAMillisecondForAnHour) {
    Group group(20.0, -80.0);
    group.run(60);
    double worst = 0;
    for (int second = 60; second < 3600; ++second) {
        group.run(1);
        worst = std::max(worst, std::abs(behind(group.clockDevice->playing(), group.followerDevice->playing())));
    }
    EXPECT_LT(worst, 0.001) << "the follower was " << worst * 1000 << " ms off at worst";
}

// The clock member's device gets exactly what its source renders.
TEST(GroupSync, TheClocksSamplesAreUnchanged) {
    Group group(0.0, 50.0);
    MomentSource reference;
    std::vector<uint8_t> expected;
    for (int i = 0; i < 300; ++i) {
        group.run(0.01);
        expected.assign(group.clockDevice->block_.size(), 0);
        reference.render(expected.data(), expected.size() / 8);
        ASSERT_EQ(group.clockDevice->block_, expected) << "period " << i;
    }
}

// A follower with a delay plays that much after the clock.
TEST(GroupSync, AFollowersDelayIsKept) {
    Group group(0.0, 100.0, 0.025);
    group.run(90);
    EXPECT_NEAR(behind(group.clockDevice->playing(), group.followerDevice->playing()), 0.025, 0.001);
}

// A follower that starts 300 ms behind or ahead of the clock jumps to it rather than drifting back for minutes.
TEST(GroupSync, AFollowerStartingOffJumpsToTheClock) {
    for (const double start : {-0.3, 0.3}) {
        Group group(0.0, 0.0, 0.0, start);
        group.run(3);
        EXPECT_NEAR(behind(group.clockDevice->playing(), group.followerDevice->playing()), 0.0, 0.001) << start;
    }
}

// A follower takes no DSD, so its Engine converts DSD to PCM; the clock takes what its device takes.
TEST(GroupSync, AFollowerTakesNoDsd) {
    GroupClock clock;
    auto link = std::make_shared<GroupLink>();
    link->clock = &clock;
    link->follower = true;
    auto follower = groupMemberOutput(std::make_unique<FakeDevice>(0.0), link);
    EXPECT_FALSE(follower->supports({kRate * 64, 2, SampleType::Dsd8}));
    EXPECT_TRUE(follower->supports({kRate, 2, SampleType::Int24In32}));
}

// The clock passing to the follower while they play: the new clock plays its own frames untouched from then on, and
// the old one keeps to it.
TEST(GroupSync, TheClockPassesToTheFollowerWhileTheyPlay) {
    Group group(0.0, 100.0);
    group.run(30);
    group.clockLink->follower = true;
    group.followerLink->follower = false;
    group.run(60);
    EXPECT_NEAR(behind(group.clockDevice->playing(), group.followerDevice->playing()), 0.0, 0.001);
}

// While the group is paused nothing is corrected, and it plays on together once it resumes.
TEST(GroupSync, NothingIsCorrectedWhileTheClockPlaysNothing) {
    Group group(0.0, 100.0);
    group.run(30);
    group.clockSource.playing_ = group.followerSource.playing_ = false;
    group.run(5);
    group.clockSource.playing_ = group.followerSource.playing_ = true;
    group.run(2);
    EXPECT_NEAR(behind(group.clockDevice->playing(), group.followerDevice->playing()), 0.0, 0.001);
}
