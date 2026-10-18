// MP3 test files encoded with LAME. Only tests.exe links LAME; the player never does.
#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>

namespace testutil {

// Encodes right-aligned 16/24-bit `samples` as 320 kbit/s CBR MP3. `lameTag` writes the Xing/LAME frame that
// declares encoder delay and padding; without it the file carries no trim metadata. Throws std::runtime_error.
void writeMp3(const std::filesystem::path& path, const std::vector<int32_t>& samples, uint32_t sampleRate, uint32_t bits,
              uint32_t channels, bool lameTag);

} // namespace testutil
