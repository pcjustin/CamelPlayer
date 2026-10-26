// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Quick playlist actions shared by the library and playlist pages (docs/Plan.md §7.9, §8.3).
#pragma once

#include <functional>

#include "Server/Library/Playlists.h"

namespace ui {

// Collects a selection's sources off the UI thread, in on-screen order.
using SourceFetch = std::function<std::vector<camel::server::PlaylistSource>()>;

// Appends "Create playlist" and "Add to playlist ▸" to a menu. `selection` runs on the UI thread when one of them is
// chosen and returns the fetch for what the menu acts on.
using PlaylistMenu =
    std::function<void(winrt::Microsoft::UI::Xaml::Controls::MenuFlyout const& menu, std::function<SourceFetch()> selection)>;

} // namespace ui
