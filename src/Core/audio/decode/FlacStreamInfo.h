// Minimal FLAC STREAMINFO reader, used to report the MD5 and stream parameters independently of the decoder.
#pragma once
#include <filesystem>

#include "audio/AudioTypes.h"

namespace camel::audio {

// Reads STREAMINFO from a native FLAC file (skipping a leading ID3v2 tag). Returns false if the file is not FLAC.
// The MD5 is left empty when the encoder stored all zeros (no MD5).
bool readFlacStreamInfo(const std::filesystem::path& path, SourceInfo& out);

} // namespace camel::audio
