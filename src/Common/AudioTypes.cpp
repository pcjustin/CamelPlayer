#include "Common/AudioTypes.h"

#include <cstdio>

namespace camel::audio {

const char* sampleTypeName(SampleType t) {
    switch (t) {
    case SampleType::Int16:
        return "16";
    case SampleType::Int24:
        return "24";
    case SampleType::Int24In32:
        return "24-in-32";
    case SampleType::Int32:
        return "32";
    case SampleType::Float32:
        return "32 float";
    case SampleType::Dsd8:
        return "DSD native";
    }
    return "?";
}

std::string describe(const DeviceFormat& f) {
    char buf[64];
    const int length =
        f.type == SampleType::Dsd8 ? snprintf(buf, sizeof buf, "DSD%u / %s", f.sampleRate * 8 / 44100, sampleTypeName(f.type))
        : f.sampleRate % 1000 == 0 ? snprintf(buf, sizeof buf, "%u kHz / %s", f.sampleRate / 1000, sampleTypeName(f.type))
                                   : snprintf(buf, sizeof buf, "%.1f kHz / %s", f.sampleRate / 1000.0, sampleTypeName(f.type));
    if (length < 0 || size_t(length) >= sizeof buf) throw std::runtime_error("failed to format device description");
    return std::string(buf, size_t(length));
}

const char* errorCodeName(ErrorCode c) {
    switch (c) {
    case ErrorCode::FileOpen:
        return "FileOpen";
    case ErrorCode::UnsupportedStream:
        return "UnsupportedStream";
    case ErrorCode::DecodeFailed:
        return "DecodeFailed";
    case ErrorCode::SeekFailed:
        return "SeekFailed";
    case ErrorCode::DeviceNotFound:
        return "DeviceNotFound";
    case ErrorCode::DeviceInUse:
        return "DeviceInUse";
    case ErrorCode::DeviceLost:
        return "DeviceLost";
    case ErrorCode::FormatNotSupported:
        return "FormatNotSupported";
    case ErrorCode::DriverError:
        return "DriverError";
    case ErrorCode::Other:
        return "Other";
    }
    return "?";
}

} // namespace camel::audio
