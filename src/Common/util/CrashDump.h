// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Writes a minidump to `dir` when the process dies from an unhandled exception (docs/Task-breakdown.md M0-07, M5-R05).
#pragma once
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <thread>

namespace camel {

constexpr size_t kKeptCrashDumps = 5;

// Keeps the newest `kKeptCrashDumps` dumps already in `dir`; a missing or unwritable directory never blocks startup.
// Also dumps on abort(), std::terminate, pure virtual calls and invalid CRT parameters.
void installCrashDumps(const std::filesystem::path& dir);

// Deletes all but the newest `keep` crash-*.dmp files in `dir`; failures are ignored.
void pruneCrashDumps(const std::filesystem::path& dir, size_t keep);

// Posts a ping through `post` every second; when one goes unanswered for `timeout`, logs it and writes
// crash-…-hang.dmp to the installCrashDumps directory, once per hang. Also logs each further GB of private memory.
// `post` returning false ends the watch.
class HangWatch {
public:
    HangWatch(std::function<bool(std::function<void()>)> post, std::chrono::seconds timeout);

private:
    std::jthread thread_;
};

} // namespace camel
