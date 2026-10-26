// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Writes a minidump to `dir` when the process dies from an unhandled exception (docs/Task-breakdown.md M0-07, M5-R05).
#pragma once
#include <cstddef>
#include <filesystem>

namespace camel {

constexpr size_t kKeptCrashDumps = 5;

// Keeps the newest `kKeptCrashDumps` dumps already in `dir`; a missing or unwritable directory never blocks startup.
void installCrashDumps(const std::filesystem::path& dir);

// Deletes all but the newest `keep` crash-*.dmp files in `dir`; failures are ignored.
void pruneCrashDumps(const std::filesystem::path& dir, size_t keep);

} // namespace camel
