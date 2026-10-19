// Raw DSD test fixtures (docs/Plan.md §16.6): known MSB-first DSD bytes written as DSF by this independent writer.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

namespace dsdfixture {

constexpr uint32_t kDsd64 = 2822400;
constexpr size_t kBlock = 4096; // DSF block size per channel

using Bytes = std::vector<uint8_t>;

inline uint8_t reversed(uint8_t b) {
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) r = uint8_t(r | ((b >> i) & 1) << (7 - i));
    return r;
}

// Interleaved, MSB-first raw DSD: frames * channels bytes.
inline Bytes rawDsd(size_t frames, uint32_t channels, uint32_t seed = 1) {
    Bytes out(frames * channels);
    uint32_t x = seed * 2654435761u + 1;
    for (auto& b : out) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        b = uint8_t(x);
    }
    return out;
}

inline void put(Bytes& out, std::string_view tag) {
    out.insert(out.end(), tag.begin(), tag.end());
}

template <typename T> inline void putLe(Bytes& out, T v) {
    for (size_t i = 0; i < sizeof(T); i++) out.push_back(uint8_t(uint64_t(v) >> (8 * i)));
}

template <typename T> inline void putBe(Bytes& out, T v) {
    for (size_t i = sizeof(T); i-- > 0;) out.push_back(uint8_t(uint64_t(v) >> (8 * i)));
}

inline void writeFile(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

struct DsfSpec {
    uint32_t channels = 2;
    uint32_t rate = kDsd64;
    uint32_t bitsPerSample = 1; // 1: LSB first, 8: MSB first
    int64_t sampleCountAdjust = 0;
    bool id3 = true;
};

// The final block is padded with nonzero bytes so that padding read as music is detected.
inline Bytes dsf(const Bytes& raw, const DsfSpec& s) {
    const size_t frames = raw.size() / s.channels;
    const size_t blocks = (frames + kBlock - 1) / kBlock;
    Bytes data(blocks * kBlock * s.channels, 0xA5);
    for (size_t i = 0; i < frames; i++)
        for (uint32_t c = 0; c < s.channels; c++) {
            const uint8_t b = raw[i * s.channels + c];
            data[(i / kBlock * s.channels + c) * kBlock + i % kBlock] = s.bitsPerSample == 1 ? reversed(b) : b;
        }
    const Bytes tag{'I', 'D', '3', 3, 0, 0, 0, 0, 0, 4, 'J', 'U', 'N', 'K'};
    const uint64_t dataChunk = 12 + data.size();
    const uint64_t total = 28 + 52 + dataChunk + (s.id3 ? tag.size() : 0);
    Bytes out;
    put(out, "DSD ");
    putLe<uint64_t>(out, 28);
    putLe<uint64_t>(out, total);
    putLe<uint64_t>(out, s.id3 ? 28 + 52 + dataChunk : 0);
    put(out, "fmt ");
    putLe<uint64_t>(out, 52);
    putLe<uint32_t>(out, 1);
    putLe<uint32_t>(out, 0);
    putLe<uint32_t>(out, s.channels <= 4 ? s.channels : s.channels + 1); // DSF channel type: 5 ch = 6, 5.1 = 7
    putLe<uint32_t>(out, s.channels);
    putLe<uint32_t>(out, s.rate);
    putLe<uint32_t>(out, s.bitsPerSample);
    putLe<uint64_t>(out, uint64_t(int64_t(frames * 8) + s.sampleCountAdjust));
    putLe<uint32_t>(out, uint32_t(kBlock));
    putLe<uint32_t>(out, 0);
    put(out, "data");
    putLe<uint64_t>(out, dataChunk);
    out.insert(out.end(), data.begin(), data.end());
    if (s.id3) out.insert(out.end(), tag.begin(), tag.end());
    return out;
}

// A second-order sigma-delta modulator (NTF (1 - z^-1)^2): `frames` byte frames of a sine at `amplitude` of full DSD
// modulation and `frequency` Hz, the same on every channel, interleaved and MSB-first.
inline Bytes dsdSine(uint32_t dsdRate, double frequency, double amplitude, size_t frames, uint32_t channels = 2) {
    Bytes out;
    out.reserve(frames * channels);
    double u1 = 0, u2 = 0, y = 1;
    for (size_t n = 0; n < frames; n++) {
        uint8_t byte = 0;
        for (int bit = 0; bit < 8; bit++) {
            const double t = double(n * 8 + size_t(bit)) / dsdRate;
            u1 += amplitude * std::sin(2 * 3.14159265358979323846 * frequency * t) - y;
            u2 += u1 - 2 * y;
            y = u2 >= 0 ? 1 : -1;
            if (y > 0) byte = uint8_t(byte | (0x80 >> bit));
        }
        out.insert(out.end(), channels, byte);
    }
    return out;
}

} // namespace dsdfixture
