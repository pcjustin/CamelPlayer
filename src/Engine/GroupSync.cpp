// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Engine/GroupSync.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

#include "Common/util/Log.h"
#include "Engine/EngineAbi.h"

namespace camel::audio {

namespace {

constexpr double kJumpBeyond = 0.010; // seconds off the clock that a follower jumps rather than drifts back
constexpr int kJumpAfter = 3;         // updates in a row that far off, so one stray reading moves nothing
constexpr double kSettle = 0.2;       // seconds after a jump, while what was buffered before it plays out
constexpr double kGain = 0.2;         // ratio per second of error
constexpr double kIntegralGain = 0.01;
constexpr double kMaxCorrection = 1e-3; // the ratio stays within 1000 ppm of 1
constexpr double kIgnoreBeyond = 5.0;   // seconds apart: not the same moment of the track, as a repeat starting
constexpr int64_t kStaleClockNs = 500'000'000;
constexpr size_t kChunk = 512;       // input frames pulled from the follower's Engine at a time
constexpr size_t kOutFrames = 32768; // resampled frames held, more than any device asks for at once

int64_t steadyNow() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void toDoubles(const uint8_t* src, size_t samples, SampleType type, double* dst) {
    switch (type) {
    case SampleType::Int16:
        for (size_t i = 0; i < samples; i++) {
            int16_t v;
            std::memcpy(&v, src + 2 * i, 2);
            dst[i] = v / 32768.0;
        }
        break;
    case SampleType::Int24:
        for (size_t i = 0; i < samples; i++) {
            const uint32_t v = (uint32_t(src[3 * i]) << 8) | (uint32_t(src[3 * i + 1]) << 16) | (uint32_t(src[3 * i + 2]) << 24);
            dst[i] = int32_t(v) / 2147483648.0;
        }
        break;
    case SampleType::Int24In32:
    case SampleType::Int32:
        for (size_t i = 0; i < samples; i++) {
            int32_t v;
            std::memcpy(&v, src + 4 * i, 4);
            dst[i] = v / 2147483648.0;
        }
        break;
    case SampleType::Float32:
        for (size_t i = 0; i < samples; i++) {
            float v;
            std::memcpy(&v, src + 4 * i, 4);
            dst[i] = v;
        }
        break;
    case SampleType::Dsd8:
        std::fill_n(dst, samples, 0.0);
        break;
    }
}

int32_t toInt32(double v) {
    return int32_t(std::lround(std::clamp(v * 2147483648.0, -2147483648.0, 2147483647.0)));
}

void fromDoubles(const double* src, size_t samples, SampleType type, uint8_t* dst) {
    switch (type) {
    case SampleType::Int16:
        for (size_t i = 0; i < samples; i++) {
            const auto v = int16_t(std::lround(std::clamp(src[i] * 32768.0, -32768.0, 32767.0)));
            std::memcpy(dst + 2 * i, &v, 2);
        }
        break;
    case SampleType::Int24:
        for (size_t i = 0; i < samples; i++) {
            const auto v = uint32_t(toInt32(src[i]));
            dst[3 * i] = uint8_t(v >> 8);
            dst[3 * i + 1] = uint8_t(v >> 16);
            dst[3 * i + 2] = uint8_t(v >> 24);
        }
        break;
    case SampleType::Int24In32:
        for (size_t i = 0; i < samples; i++) {
            const auto v = uint32_t(toInt32(src[i])) & 0xFFFFFF00U; // as a 24-bit sample, the low byte left clear
            std::memcpy(dst + 4 * i, &v, 4);
        }
        break;
    case SampleType::Int32:
        for (size_t i = 0; i < samples; i++) {
            const int32_t v = toInt32(src[i]);
            std::memcpy(dst + 4 * i, &v, 4);
        }
        break;
    case SampleType::Float32:
        for (size_t i = 0; i < samples; i++) {
            const auto v = float(src[i]);
            std::memcpy(dst + 4 * i, &v, 4);
        }
        break;
    case SampleType::Dsd8:
        std::memset(dst, 0, samples);
        break;
    }
}

struct ResamplerDeleter {
    void operator()(CamelDriftResampler* resampler) const { camel_drift_resampler_destroy_v1(resampler); }
};

// Frames the resampler holds that its reported delay leaves out: soxr writes each frame where its input frame was, so
// the length its filter looks ahead never shows in the delay. Measured once with an impulse at a ratio of 1.
double unreportedHold() {
    static const double hold = [] {
        std::unique_ptr<CamelDriftResampler, ResamplerDeleter> resampler(camel_drift_resampler_create_v1(1));
        if (!resampler) return 0.0;
        constexpr size_t kFrames = 4096, kImpulse = 1000;
        std::vector<double> input(kFrames, 0.0), output(2 * kFrames, 0.0);
        input[kImpulse] = 1.0;
        size_t fed = 0, made = 0;
        while (fed < kFrames) {
            size_t used = 0, produced = 0;
            if (camel_drift_resampler_process_v1(resampler.get(), input.data() + fed, std::min<size_t>(512, kFrames - fed), &used,
                                                 output.data() + made, output.size() - made, &produced) != 0 ||
                (!used && !produced))
                return 0.0;
            fed += used;
            made += produced;
        }
        const auto peak = std::max_element(output.begin(), output.begin() + std::ptrdiff_t(made),
                                           [](double a, double b) { return std::abs(a) < std::abs(b); });
        const double shift = double(peak - output.begin()) - double(kImpulse);
        return double(fed) - double(made) + shift - camel_drift_resampler_delay_v1(resampler.get());
    }();
    return hold;
}

class GroupMember final : public IOutput, private IRenderSource {
public:
    GroupMember(std::unique_ptr<IOutput> device, std::shared_ptr<GroupLink> link)
        : device_(std::move(device)), link_(std::move(link)) {
        device_->setErrorHandler([this](const Error& e) { raiseError(e); });
        device_->setRestartHandler([this] { raiseRestart(); });
    }
    // The device stops calling this member before it goes.
    ~GroupMember() override {
        try {
            device_->close();
        } catch (const std::exception& e) {
            log::warn("group member: %s", e.what());
        }
    }
    GroupMember(const GroupMember&) = delete;
    GroupMember& operator=(const GroupMember&) = delete;

    bool supports(const DeviceFormat& f) override {
        return (!link_->follower.load() || f.type != SampleType::Dsd8) && device_->supports(f);
    }

    // A PCM stream can follow a clock, whatever the member's place when it opens.
    void open(const DeviceFormat& f, IRenderSource* source) override {
        format_ = f;
        resampler_.reset();
        if (f.type != SampleType::Dsd8) {
            resampler_.reset(camel_drift_resampler_create_v1(f.channels));
            if (!resampler_) throw Error(ErrorCode::Other, "the group's drift resampler could not be made");
            packed_.assign(kChunk * f.bytesPerFrame(), 0);
            in_.assign(kChunk * f.channels, 0.0);
            out_.assign(kOutFrames * f.channels, 0.0);
            hold_ = unreportedHold();
            ratio_ = 1.0;
        }
        following_ = false;
        source_.store(source, std::memory_order_release);
        try {
            device_->open(f, this);
        } catch (...) {
            source_.store(nullptr, std::memory_order_release);
            throw;
        }
    }

    void start() override {
        latency_ = device_->status().bufferFrames;
        device_->start();
    }

    void pause() override {
        device_->pause();
        clearClock();
    }

    void close() override {
        device_->close();
        source_.store(nullptr, std::memory_order_release);
        clearClock();
    }

    OutputStatus status() const override { return device_->status(); }
    bool gapless() const override { return device_->gapless(); }
    IOutput* device() override { return device_->device(); }

private:
    void clearClock() noexcept {
        if (GroupClock* clock = link_->clock.load(); clock && !link_->follower.load()) clock->clear();
    }

    // ---- the device's source (its real-time thread) ----
    size_t render(uint8_t* dst, size_t frames) noexcept override {
        IRenderSource* source = source_.load(std::memory_order_acquire);
        if (!source) return 0;
        GroupClock* clock = link_->clock.load(std::memory_order_acquire);
        const bool follower = clock && resampler_ && link_->follower.load(std::memory_order_acquire);
        if (follower != following_) {
            following_ = follower;
            startFollowing();
        }
        if (follower) return follow(*clock, *source, dst, frames);
        const size_t n = source->render(dst, frames);
        if (!clock) return n;
        RenderedPoint point;
        if (n == frames && source->renderedPoint(point))
            clock->publish(point.track, point.seconds - double(latency_) / format_.sampleRate, clock->now());
        else
            clock->clear();
        return n;
    }

    // From its own frames on, the follower's resampler empty and at a ratio of 1.
    void startFollowing() noexcept {
        outFrames_ = outRead_ = 0;
        skip_ = 0;
        lastUpdate_ = 0;
        havePoint_ = false;
        controller_.reset();
        if (resampler_ && ratio_ != 1.0 && camel_drift_resampler_set_ratio_v1(resampler_.get(), 1.0, 0) == 0) ratio_ = 1.0;
    }

    bool writesSilence() const noexcept override {
        const IRenderSource* source = source_.load(std::memory_order_acquire);
        return !following_ && source && source->writesSilence();
    }
    size_t boundaryInLastRender() const noexcept override {
        const IRenderSource* source = source_.load(std::memory_order_acquire);
        return !following_ && source ? source->boundaryInLastRender() : kNoBoundary;
    }
    bool renderEnded() const noexcept override {
        const IRenderSource* source = source_.load(std::memory_order_acquire);
        return source && source->renderEnded();
    }
    bool renderedPoint(RenderedPoint& point) const noexcept override {
        const IRenderSource* source = source_.load(std::memory_order_acquire);
        return !following_ && source && source->renderedPoint(point);
    }
    StreamTrack streamTrack() const override {
        const IRenderSource* source = source_.load(std::memory_order_acquire);
        return source ? source->streamTrack() : StreamTrack{};
    }
    std::optional<StreamTrack> nextStreamTrack() const override {
        const IRenderSource* source = source_.load(std::memory_order_acquire);
        return source ? source->nextStreamTrack() : std::nullopt;
    }

    // The follower's frames: what its Engine renders, resampled at the ratio that keeps it to the clock. While its Engine
    // has none, paused like the clock, what it holds waits for it to play again rather than sounding on alone.
    size_t follow(const GroupClock& clock, IRenderSource& source, uint8_t* dst, size_t frames) noexcept {
        const uint32_t channels = format_.channels, bpf = format_.bytesPerFrame();
        while (outFrames_ - outRead_ < frames && outFrames_ < kOutFrames)
            if (!refill(source)) {
                std::memset(dst, 0, frames * bpf);
                keepToClock(clock);
                return 0;
            }
        const size_t take = std::min(outFrames_ - outRead_, frames);
        fromDoubles(out_.data() + outRead_ * channels, take * channels, format_.type, dst);
        outRead_ += take;
        if (take < frames) std::memset(dst + take * bpf, 0, (frames - take) * bpf);
        keepToClock(clock);
        return take;
    }

    // Resamples the next frames of the Engine, or silence the controller adds, after what is held; false when the Engine
    // has none now.
    bool refill(IRenderSource& source) noexcept {
        const uint32_t channels = format_.channels;
        size_t frames = 0;
        if (skip_ < 0) {
            frames = size_t(std::min<int64_t>(-skip_, int64_t(kChunk)));
            std::fill_n(in_.data(), frames * channels, 0.0);
            skip_ += int64_t(frames);
        } else {
            const size_t n = source.render(packed_.data(), kChunk);
            if (!n) return false;
            if (source.renderedPoint(point_)) havePoint_ = true;
            const size_t dropped = size_t(std::min<int64_t>(skip_, int64_t(n)));
            skip_ -= int64_t(dropped);
            frames = n - dropped;
            toDoubles(packed_.data() + dropped * format_.bytesPerFrame(), frames * channels, format_.type, in_.data());
        }
        if (outRead_) {
            std::memmove(out_.data(), out_.data() + outRead_ * channels, (outFrames_ - outRead_) * channels * sizeof(double));
            outFrames_ -= outRead_;
            outRead_ = 0;
        }
        size_t offset = 0;
        while (offset < frames && outFrames_ < kOutFrames) {
            size_t used = 0, produced = 0;
            if (camel_drift_resampler_process_v1(resampler_.get(), in_.data() + offset * channels, frames - offset, &used,
                                                 out_.data() + outFrames_ * channels, kOutFrames - outFrames_, &produced) != 0)
                return false;
            offset += used;
            outFrames_ += produced;
            if (!used && !produced) break;
        }
        return true;
    }

    // How far behind the clock this member plays now, from where each device is, its delay allowed for.
    void keepToClock(const GroupClock& clock) noexcept {
        uint64_t track = 0;
        double seconds = 0;
        int64_t at = 0;
        const int64_t now = clock.now();
        if (!havePoint_ || !clock.read(track, seconds, at) || track != point_.track || now - at > kStaleClockNs) return;
        const double rate = format_.sampleRate;
        const double held =
            (camel_drift_resampler_delay_v1(resampler_.get()) + hold_ + double(outFrames_ - outRead_) + double(latency_)) / rate;
        const double error = seconds + double(now - at) * 1e-9 - link_->delaySeconds.load() - (point_.seconds - held);
        const double elapsed = lastUpdate_ ? double(now - lastUpdate_) * 1e-9 : 0.0;
        lastUpdate_ = now;
        if (std::abs(error) > kIgnoreBeyond) return;
        const auto step = controller_.update(error, elapsed, format_.sampleRate);
        skip_ += step.skipFrames;
        if (step.ratio != ratio_ &&
            camel_drift_resampler_set_ratio_v1(resampler_.get(), step.ratio, format_.sampleRate / 10) == 0)
            ratio_ = step.ratio;
    }

    std::unique_ptr<IOutput> device_;
    std::shared_ptr<GroupLink> link_;
    DeviceFormat format_;
    std::atomic<IRenderSource*> source_{nullptr};
    uint32_t latency_ = 0; // the device's buffer, frames it holds before what is written now plays

    // The follower's, used on the device's thread only once the stream is open.
    bool following_ = false;
    std::unique_ptr<CamelDriftResampler, ResamplerDeleter> resampler_;
    std::vector<uint8_t> packed_;
    std::vector<double> in_, out_;
    size_t outFrames_ = 0, outRead_ = 0;
    double hold_ = 0;
    int64_t skip_ = 0;
    double ratio_ = 1.0;
    int64_t lastUpdate_ = 0;
    RenderedPoint point_;
    bool havePoint_ = false;
    DriftController controller_;
};

} // namespace

GroupClock::GroupClock(std::function<int64_t()> now) : now_(now ? std::move(now) : std::function<int64_t()>(steadyNow)) {}

void GroupClock::publish(uint64_t track, double seconds, int64_t at) noexcept {
    sequence_.fetch_add(1, std::memory_order_acq_rel);
    track_.store(track, std::memory_order_relaxed);
    seconds_.store(seconds, std::memory_order_relaxed);
    at_.store(at, std::memory_order_relaxed);
    sequence_.fetch_add(1, std::memory_order_acq_rel);
}

void GroupClock::clear() noexcept {
    publish(0, 0.0, 0);
}

bool GroupClock::read(uint64_t& track, double& seconds, int64_t& at) const noexcept {
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint64_t before = sequence_.load(std::memory_order_acquire);
        if (before & 1) continue;
        track = track_.load(std::memory_order_relaxed);
        seconds = seconds_.load(std::memory_order_relaxed);
        at = at_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (sequence_.load(std::memory_order_relaxed) == before) return track != 0;
    }
    return false;
}

DriftController::Step DriftController::update(double error, double elapsed, uint32_t rate) {
    if (settling_ > 0) {
        settling_ -= elapsed;
        return {0, ratio_};
    }
    if (!aligned_ || std::abs(error) > kJumpBeyond) {
        if (aligned_ && ++far_ < kJumpAfter) return {0, ratio_};
        aligned_ = true;
        far_ = 0;
        settling_ = kSettle;
        return {std::llround(error * rate), ratio_};
    }
    far_ = 0;
    integral_ = std::clamp(integral_ + error * elapsed, -kMaxCorrection / kIntegralGain, kMaxCorrection / kIntegralGain);
    ratio_ = 1.0 + std::clamp(kGain * error + kIntegralGain * integral_, -kMaxCorrection, kMaxCorrection);
    return {0, ratio_};
}

void DriftController::reset() {
    *this = DriftController();
}

std::unique_ptr<IOutput> groupMemberOutput(std::unique_ptr<IOutput> device, std::shared_ptr<GroupLink> link) {
    return std::make_unique<GroupMember>(std::move(device), std::move(link));
}

} // namespace camel::audio
