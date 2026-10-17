// Test material for the MVP (docs/Plan.md §16.6): FLAC files generated with the libFLAC encoder,
// and an independent libFLAC reference decode used by capture comparison and the decoder report.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "audio/AudioTypes.h"

namespace camel::audio::testaudio {

struct GenSpec {
    std::string name; // file stem, e.g. "24-96k"
    uint32_t sampleRate;
    uint32_t bits;
    uint32_t channels = 2;
    double seconds = 10.0;
    bool md5 = true;
    double toneDb = 0; // < 0: a plain 440 Hz sine at this level (safe for listening tests) instead of the full-scale test signal
};

// The §16.6 matrix: 16/44.1, 16/48, 24/96, 24/192, 24/352.8, 32/384, 32/768.
std::vector<GenSpec> standardMatrix(double seconds = 10.0);

// Deterministic test signal (sine + noise reaching every bit), right-aligned to `bits`, interleaved.
std::vector<int32_t> makeSignal(const GenSpec& spec);

// Encodes `spec` to `path` with libFLAC. Throws std::runtime_error.
void writeFlac(const std::filesystem::path& path, const GenSpec& spec);

// Copies `src` to `dst` with one byte flipped at `fraction` of the file (a damaged frame).
void writeCorrupted(const std::filesystem::path& src, const std::filesystem::path& dst, double fraction = 0.5);
// Copies the first `fraction` of `src` to `dst` (a truncated download).
void writeTruncated(const std::filesystem::path& src, const std::filesystem::path& dst, double fraction = 0.6);

// Generates the matrix plus "damaged", "truncated" and "no-md5" files into `dir`. Returns all paths.
std::vector<std::filesystem::path> generateAll(const std::filesystem::path& dir, double seconds = 10.0);

struct ReferenceDecode {
    SourceInfo info;
    std::vector<int32_t> samples; // interleaved, right-aligned
    bool md5Checked = false;      // libFLAC verified the STREAMINFO MD5
    bool md5Ok = false;
    std::string error; // empty when the whole file decoded cleanly
};

// Decodes the whole file with libFLAC (MD5 checking on).
ReferenceDecode referenceDecode(const std::filesystem::path& path);

} // namespace camel::audio::testaudio
