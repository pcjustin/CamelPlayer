// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Favorite tracks and albums (docs/Plan.md §7.6, §8.3), shared by the views that show them.
#pragma once

#include <functional>

#include "Server/Api/ServerApi.h"

namespace ui {

using Submit = std::function<void(camel::server::Command)>;

camel::server::Command favoriteCommand(std::vector<camel::server::LibraryTrackKey> tracks, bool favorite);
camel::server::Command albumFavoriteCommand(std::vector<int64_t> albums, bool favorite);

// Appends a separator, "Add to favorites" and "Remove from favorites"; returns the entries so a caller can hide them for
// rows that are not tracks.
std::vector<winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItemBase>
appendFavoriteMenu(winrt::Microsoft::UI::Xaml::Controls::MenuFlyout const& menu, std::function<void(bool)> choose);
// "Add to favorite albums" / "Remove from favorite albums".
void appendAlbumFavoriteMenu(winrt::Microsoft::UI::Xaml::Controls::MenuFlyout const& menu, std::function<void(bool)> choose);

// A row's heart was clicked: updates its BrowseItem and submits the change for its track.
void rowFavoriteClicked(winrt::Windows::Foundation::IInspectable const& button, Submit const& submit);

} // namespace ui
