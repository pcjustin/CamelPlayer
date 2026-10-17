// Identifies a file's container and codec from its contents; the extension is ignored (docs/Plan.md §3.1).
#pragma once
#include <filesystem>

#include "Common/AudioFormat.h"

namespace camel::audio {

// Throws Error (FileOpen when unreadable, UnsupportedStream when unrecognized or an unsupported codec/profile).
AudioFormat probeFormat(const std::filesystem::path& path);

} // namespace camel::audio
