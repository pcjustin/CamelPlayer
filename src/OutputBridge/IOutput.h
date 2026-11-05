// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Output backend interface (docs/Plan.md §4, §16.2).
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Common/AudioTypes.h"

namespace camel {
struct Settings;
}

namespace camel::audio {

// A track as a stream carries it, for outputs that announce each track to a network player (docs/Plan.md §5.10).
struct StreamTrack {
    std::wstring file;
    uint64_t frames = 0;     // output frames the stream has of it; 0 while the length is unknown
    uint64_t startFrame = 0; // output frames of the file before the stream's start
    uint64_t sequence = 0;   // tells the same file queued twice apart
    uint64_t leadFrames = 0; // silence render() gives before the track, which the stream carries too
    bool operator==(const StreamTrack&) const = default;
};

// Supplies packed device bytes. Called on the output's real-time thread: must not block or allocate.
class IRenderSource {
public:
    static constexpr size_t kNoBoundary = static_cast<size_t>(-1);

    virtual ~IRenderSource() = default;
    // Writes up to `frames` frames into `dst`; returns how many it wrote. The output fills the rest with silence.
    virtual size_t render(uint8_t* dst, size_t frames) noexcept = 0;
    // True while render() writes format-correct silence after its frames itself (raw DSD: idle pattern, continuing
    // DoP markers), so nothing may overwrite the rest with zeros.
    virtual bool writesSilence() const noexcept { return false; }
    // Where the next track starts in the frames of the last render(), or kNoBoundary.
    virtual size_t boundaryInLastRender() const noexcept { return kNoBoundary; }
    // True once render() has handed out the last frame of the last track.
    virtual bool renderEnded() const noexcept { return false; }
    // The track the stream starts with, and the queued one after it. Not for the real-time thread.
    virtual StreamTrack streamTrack() const { return {}; }
    virtual std::optional<StreamTrack> nextStreamTrack() const { return std::nullopt; }
};

struct OutputStatus {
    std::string mode;          // e.g. "WASAPI exclusive · event", "KS · standard streaming"
    uint32_t bufferFrames = 0; // device buffer actually in use
    std::string detail;        // backend specific (ASIO sample type, KS buffer size, ...)
    // Saved parameters the device could not take and what is used instead (requested ≠ active); the settings keep
    // the requested values.
    std::vector<std::string> adjustments;
    // Driver callbacks clearly later than the driver's own rhythm since the stream opened (CallbackTiming.h); only ASIO,
    // whose driver owns the thread, measures them. Each can be an audible gap that no underrun counts.
    uint64_t lateCallbacks = 0;
    double longestGapMs = 0;
    double periodMs = 0; // the buffer period
    // Frames rendered that the device has not played yet, where that is far more than its buffer (a network player).
    uint64_t heldFrames = 0;
};

class IOutput {
public:
    virtual ~IOutput() = default;

    // Whether the device takes this exact format. May open the device or driver.
    virtual bool supports(const DeviceFormat& f) = 0;

    // Opens the stream; `source` must outlive the stream. Throws Error (DeviceNotFound / DeviceInUse / FormatNotSupported / ...).
    virtual void open(const DeviceFormat& f, IRenderSource* source) = 0;
    virtual void start() = 0; // begin or resume pulling from the source
    virtual void pause() = 0; // stop pulling, keep the device open
    virtual void close() = 0; // after close() returns the source is never called again

    virtual OutputStatus status() const = 0;
    // False when the device cannot follow one track with the next without a gap, so nothing is queued on it.
    virtual bool gapless() const { return true; }

    // Called from any thread when the stream fails after open() (device unplugged, taken over, ...).
    void setErrorHandler(std::function<void(const Error&)> h) { onError_ = std::move(h); }
    // Called when the driver asks for a restart (ASIO reset request, sample rate changed elsewhere).
    void setRestartHandler(std::function<void()> h) { onRestart_ = std::move(h); }

protected:
    void raiseError(const Error& e) {
        if (onError_) onError_(e);
    }
    void raiseRestart() {
        if (onRestart_) onRestart_();
    }

private:
    std::function<void(const Error&)> onError_;
    std::function<void()> onRestart_;
};

struct DeviceEntry {
    std::string id;   // value stored in settings
    std::string name; // display name (UTF-8)
};

std::vector<DeviceEntry> listWasapiDevices(); // active render endpoints
std::string defaultWasapiDevice();            // the default console render endpoint; empty when there is none
std::vector<DeviceEntry> listAsioDrivers();
std::vector<DeviceEntry> listKsPins(); // PCM render pins

// Creates the output for the backend selected in `s`.
std::unique_ptr<IOutput> createOutput(const Settings& s);

} // namespace camel::audio
