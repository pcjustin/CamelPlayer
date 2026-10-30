// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The one-click diagnostic bundle attached to GitHub issues (docs/Plan.md §17.5): logs, settings, device
// capabilities and the newest crash dump in one zip. Paths are left out unless the user includes them.
#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/settings/Settings.h"
#include "OutputBridge/IOutput.h"
#include "Server/Api/ServerApi.h"

namespace camel::server {

struct DiagnosticInput {
    std::filesystem::path dataDirectory;    // holds settings.json, logs\ and crashdumps\.
    std::filesystem::path programDirectory; // checked against its install-manifest.txt
    bool includePaths = false;
    std::string appVersion;
    Snapshot snapshot;
    std::vector<LibraryRootInfo> roots;
    std::string capabilities; // probeCapabilities() or why it was skipped
};

void writeDiagnosticBundle(const std::filesystem::path& zip, const DiagnosticInput& input);

// Replaces each file or folder path (C:\..., \\server\..., \\?\...) with "<path>". A path ends at the line end or
// before " via ", " is reported " or ": ", which follow paths in the log lines.
std::string redactPaths(std::string_view text);

// Which stereo formats the device selected in `settings` takes, as a table per rate. Asks `output` only, so it must
// not be the device of a running stream.
std::string probeCapabilities(audio::IOutput& output, const Settings& settings);

} // namespace camel::server
