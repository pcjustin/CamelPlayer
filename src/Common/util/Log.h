// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Minimal thread-safe file log in %LOCALAPPDATA%\CamelPlayer\logs (kept 7 days, docs/Plan.md §10).
// Never call from a real-time audio thread.
#pragma once
#include <filesystem>

namespace camel::log {

// Opens logs\<prefix>-YYYYMMDD.log and deletes logs older than 7 days. Without init, messages go to the debugger only.
void init(const std::filesystem::path& logDir, const wchar_t* prefix);

void info(const char* fmt, ...);
void warn(const char* fmt, ...);
void error(const char* fmt, ...);

} // namespace camel::log
