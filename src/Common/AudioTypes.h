// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Basic audio types shared by decoders, the engine and outputs (docs/Plan.md §5, §16).
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

namespace camel::audio {

// Sample layouts a device can accept. Integer layouts follow WAVEFORMATEXTENSIBLE conventions:
// Int24In32 is a 24-bit value MSB-aligned in a 32-bit word (low 8 bits zero). Dsd8 is native DSD: one byte per
// channel per frame, eight samples with the oldest in the MSB, so a DeviceFormat's sampleRate is the DSD rate / 8.
enum class SampleType : uint8_t { Int16, Int24, Int24In32, Int32, Float32, Dsd8 };

constexpr uint32_t bytesPerSample(SampleType t) {
    switch (t) {
    case SampleType::Int16:
        return 2;
    case SampleType::Int24:
        return 3;
    case SampleType::Dsd8:
        return 1;
    default:
        return 4;
    }
}

// Number of significant bits the layout carries (container width for Int32 / Float32).
constexpr uint32_t validBits(SampleType t) {
    switch (t) {
    case SampleType::Int16:
        return 16;
    case SampleType::Int24:
    case SampleType::Int24In32:
        return 24;
    case SampleType::Dsd8:
        return 1;
    default:
        return 32;
    }
}

const char* sampleTypeName(SampleType t);

struct DeviceFormat {
    uint32_t sampleRate = 0;
    uint32_t channels = 2;
    SampleType type = SampleType::Int32;

    uint32_t bytesPerFrame() const { return bytesPerSample(type) * channels; }
    bool operator==(const DeviceFormat&) const = default;
};

std::string describe(const DeviceFormat& f); // e.g. "96 kHz / 24-in-32"

// What a decoder reports about the stream it opened.
struct SourceInfo {
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    uint32_t bitsPerSample = 0;                 // original bit depth; decoded samples are right-aligned to it
    uint64_t totalFrames = 0;                   // 0 if unknown
    std::optional<std::array<uint8_t, 16>> md5; // FLAC STREAMINFO MD5 when present (all zero means absent)
};

inline double framesToSeconds(uint64_t frames, uint32_t rate) {
    return rate ? double(frames) / rate : 0.0;
}
inline uint64_t secondsToFrames(double seconds, uint32_t rate) {
    if (!std::isfinite(seconds) || seconds <= 0 || !rate) return 0;
    const double frames = seconds * rate;
    if (frames >= double(std::numeric_limits<uint64_t>::max())) return std::numeric_limits<uint64_t>::max();
    return uint64_t(std::round(frames));
}

// Errors carry a category the UI can turn into a localized message, plus a technical detail string.
enum class ErrorCode {
    FileOpen,
    UnsupportedStream, // decoder cannot handle this file (e.g. bit depth)
    DecodeFailed,      // corrupt data
    SeekFailed,
    DeviceNotFound,
    DeviceInUse,
    DeviceLost,
    FormatNotSupported, // device cannot take the file's rate / bit depth without conversion
    DriverError,
    Other,
};

const char* errorCodeName(ErrorCode c);

class Error : public std::runtime_error {
public:
    Error(ErrorCode code, const std::string& detail) : std::runtime_error(detail), code_(code) {}
    ErrorCode code() const { return code_; }

private:
    ErrorCode code_;
};

} // namespace camel::audio
