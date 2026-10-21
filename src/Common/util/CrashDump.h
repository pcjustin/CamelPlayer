// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Writes a minidump to `dir` when the process dies from an unhandled exception (docs/Task-breakdown.md M0-07).
#pragma once
#include <filesystem>

namespace camel {

void installCrashDumps(const std::filesystem::path& dir);

} // namespace camel
