// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Files the player and CamelUpdater.exe exchange (docs/Plan.md §8.6).
#pragma once

namespace camel::updatefiles {

// In the data folder: one line "<installed|failed|cancelled> <version>", written by the updater; "installed" adds the
// replaced version.
inline constexpr wchar_t kResult[] = L"update-result";
// In the data folder: created by the new player once it has started.
inline constexpr wchar_t kStarted[] = L"update-started";
// In the program folder while an update runs: the previous version's files.
inline constexpr wchar_t kBackup[] = L"update-backup";
// Beside the running updater: settings.json and library.db with its WAL files from before the update.
inline constexpr wchar_t kDataBackup[] = L"data-backup";
// In the program folder of a packaged build: "<sha256>  <size>  <path>" for every other file of the package.
inline constexpr wchar_t kManifest[] = L"install-manifest.txt";
// In the data folder: the version whose program files last passed a full check.
inline constexpr wchar_t kVerified[] = L"install-verified";
inline constexpr wchar_t kUpdater[] = L"CamelUpdater.exe";
inline constexpr wchar_t kPlayer[] = L"CamelPlayer.exe";
inline constexpr const wchar_t* kDataFiles[] = {L"settings.json", L"library.db", L"library.db-wal", L"library.db-shm"};

} // namespace camel::updatefiles
