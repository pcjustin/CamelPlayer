// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Server/Library/LibraryViews.h"

namespace ui {

// A step of the Folders page's trail; without a folder, "Folders": the list of every library folder.
struct FolderStep {
    std::optional<int64_t> folder;
    std::string name;
    bool operator==(const FolderStep&) const = default;
};

// "Folders" first, as Roon's Folders browser keeps its root, then the library folder and the folders down to the one shown.
inline std::vector<FolderStep> folderTrail(const std::vector<camel::server::LibraryFolderEntry>& trail, std::string folders) {
    std::vector<FolderStep> steps{{std::nullopt, std::move(folders)}};
    for (const auto& entry : trail) steps.push_back({entry.id, entry.name});
    return steps;
}

} // namespace ui
