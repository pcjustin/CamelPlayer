#include "audio/engine/SamplePacker.h"

#include <cstring>

namespace camel::audio {

bool canCarryBitPerfect(SampleType type, uint32_t bits) {
    return type != SampleType::Float32 && bits >= 1 && bits <= validBits(type);
}

void packSamples(const int32_t* src, size_t count, uint32_t bits, SampleType type, uint8_t* dst) {
    switch (type) {
    case SampleType::Int16: {
        const int shift = 16 - int(bits);
        for (size_t i = 0; i < count; i++) {
            int16_t v = int16_t(uint32_t(src[i]) << shift);
            std::memcpy(dst + 2 * i, &v, 2);
        }
        break;
    }
    case SampleType::Int24: {
        const int shift = 24 - int(bits);
        for (size_t i = 0; i < count; i++) {
            uint32_t v = uint32_t(src[i]) << shift;
            dst[3 * i] = uint8_t(v);
            dst[3 * i + 1] = uint8_t(v >> 8);
            dst[3 * i + 2] = uint8_t(v >> 16);
        }
        break;
    }
    case SampleType::Int24In32:
    case SampleType::Int32: {
        const int shift = 32 - int(bits);
        for (size_t i = 0; i < count; i++) {
            uint32_t v = uint32_t(src[i]) << shift;
            std::memcpy(dst + 4 * i, &v, 4);
        }
        break;
    }
    case SampleType::Float32:
        break; // never chosen by the bit-perfect rule
    }
}

void unpackSamples(const uint8_t* src, size_t count, uint32_t bits, SampleType type, int32_t* dst) {
    switch (type) {
    case SampleType::Int16: {
        const int shift = 16 - int(bits);
        for (size_t i = 0; i < count; i++) {
            int16_t v;
            std::memcpy(&v, src + 2 * i, 2);
            dst[i] = int32_t(v) >> shift;
        }
        break;
    }
    case SampleType::Int24: {
        const int shift = 32 - int(bits);
        for (size_t i = 0; i < count; i++) {
            uint32_t v = (uint32_t(src[3 * i]) << 8) | (uint32_t(src[3 * i + 1]) << 16) | (uint32_t(src[3 * i + 2]) << 24);
            dst[i] = int32_t(v) >> shift;
        }
        break;
    }
    case SampleType::Int24In32:
    case SampleType::Int32: {
        const int shift = 32 - int(bits);
        for (size_t i = 0; i < count; i++) {
            int32_t v;
            std::memcpy(&v, src + 4 * i, 4);
            dst[i] = v >> shift;
        }
        break;
    }
    case SampleType::Float32:
        break;
    }
}

} // namespace camel::audio
