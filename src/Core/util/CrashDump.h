// Writes a minidump to `dir` when the process dies from an unhandled exception (docs/任務拆解.md M0-07).
#pragma once
#include <filesystem>

namespace camel {

void installCrashDumps(const std::filesystem::path& dir);

} // namespace camel
