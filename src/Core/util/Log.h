// Minimal thread-safe file log in %LOCALAPPDATA%\CamelPlayer\logs (kept 7 days, docs/計劃書.md §10).
// Never call from a real-time audio thread.
#pragma once
#include <filesystem>

namespace camel::log {

// Opens logs\<prefix>-YYYYMMDD.log and deletes logs older than 7 days. Without init, messages go to the debugger only.
void init(const std::filesystem::path& logDir, const wchar_t* prefix);

void info(const char* fmt, ...);
void warn(const char* fmt, ...);
void error(const char* fmt, ...);

}  // namespace camel::log
