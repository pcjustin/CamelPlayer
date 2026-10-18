// Output backend interface (docs/Plan.md §4, §16.2).
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Common/AudioTypes.h"

namespace camel {
struct Settings;
}

namespace camel::audio {

// Supplies packed device bytes. Called on the output's real-time thread: must not block or allocate.
class IRenderSource {
public:
    virtual ~IRenderSource() = default;
    // Writes up to `frames` frames into `dst`; returns how many it wrote. The output fills the rest with silence.
    virtual size_t render(uint8_t* dst, size_t frames) noexcept = 0;
    // True while render() writes format-correct silence after its frames itself (raw DSD: idle pattern, continuing
    // DoP markers), so nothing may overwrite the rest with zeros.
    virtual bool writesSilence() const noexcept { return false; }
};

struct OutputStatus {
    std::string mode;          // e.g. "WASAPI exclusive · event", "KS · standard streaming"
    uint32_t bufferFrames = 0; // device buffer actually in use
    std::string detail;        // backend specific (ASIO sample type, KS buffer size, ...)
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
std::vector<DeviceEntry> listAsioDrivers();
std::vector<DeviceEntry> listKsPins(); // PCM render pins

// Creates the output for the backend selected in `s`.
std::unique_ptr<IOutput> createOutput(const Settings& s);

} // namespace camel::audio
