// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Replaces the installed version's files, as its install-manifest.txt lists them, with the new version's, leaving any
// other file in the program folder where it is, and rolls back when that or the new version's start fails
// (docs/Plan.md §8.6). Plain Win32 and the standard library, so CamelUpdater.exe runs from the temp folder on its own.
#pragma once
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>

namespace camel::updater {

struct Options {
    std::filesystem::path app;        // the program folder
    std::filesystem::path files;      // the new version's extracted files
    std::filesystem::path data;       // the player's data folder
    std::filesystem::path dataBackup; // where settings.json and library.db are kept until the new version started
    std::wstring version;             // the new version
    std::wstring previous;            // the version being replaced; empty when not known
    // SHA-256 of the new version's install-manifest.txt as the player extracted it; empty: the files are not checked.
    std::string manifestSha256;
};

// What the installation needs from the system; tests replace it.
struct Host {
    std::function<void(const std::filesystem::path& exe)> startPlayer;
    std::function<bool()> playerRunning; // a CamelPlayer.exe from the program folder is running
    std::function<void()> endPlayer;     // ends it and waits until it has exited
    std::chrono::milliseconds startTimeout{30000};
    std::chrono::milliseconds launchGrace{5000}; // how long a started player may take to appear
    std::function<void(const std::wstring&)> log;
};

enum class Outcome { Installed, RolledBack };

// Expects the old player to have exited. Writes the update-result file the started player reads.
Outcome install(const Options& options, const Host& host);

// Whether two paths name the player's program.
bool sameFile(const std::filesystem::path& a, const std::filesystem::path& b);

// The data folder's update-result line.
void writeResult(const std::filesystem::path& data, const wchar_t* kind, const std::wstring& version,
                 const std::wstring& previous = {});

} // namespace camel::updater
