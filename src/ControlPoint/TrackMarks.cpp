// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pch.h"

#include "TrackMarks.h"

#include "BrowseItem.h"
#include "UiText.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace camel::server;

namespace ui {
namespace {

MenuFlyoutItem entry(hstring const& text, const wchar_t* glyph, std::function<void()> click) {
    MenuFlyoutItem item;
    item.Text(text);
    FontIcon icon;
    icon.Glyph(glyph);
    item.Icon(icon);
    item.Click([click = std::move(click)](auto&&, auto&&) { click(); });
    return item;
}

} // namespace

Command favoriteCommand(std::vector<LibraryTrackKey> tracks, bool favorite) {
    Command command;
    command.kind = CommandKind::SetFavorite;
    command.tracks = std::move(tracks);
    command.favorite = favorite;
    return command;
}

Command albumFavoriteCommand(std::vector<int64_t> albums, bool favorite) {
    Command command;
    command.kind = CommandKind::SetAlbumFavorite;
    command.albums = std::move(albums);
    command.favorite = favorite;
    return command;
}

std::vector<MenuFlyoutItemBase> appendFavoriteMenu(MenuFlyout const& menu, std::function<void(bool)> choose) {
    std::vector<MenuFlyoutItemBase> added{MenuFlyoutSeparator(),
                                          entry(tr(L"Marks_AddFavorite"), L"\xE00B", [choose] { choose(true); }),
                                          entry(tr(L"Marks_RemoveFavorite"), L"\xE006", [choose] { choose(false); })};
    for (const auto& item : added) menu.Items().Append(item);
    return added;
}

void appendAlbumFavoriteMenu(MenuFlyout const& menu, std::function<void(bool)> choose) {
    menu.Items().Append(MenuFlyoutSeparator());
    menu.Items().Append(entry(tr(L"Marks_AddFavoriteAlbum"), L"\xE00B", [choose] { choose(true); }));
    menu.Items().Append(entry(tr(L"Marks_RemoveFavoriteAlbum"), L"\xE006", [choose] { choose(false); }));
}

void rowFavoriteClicked(Windows::Foundation::IInspectable const& button, Submit const& submit) {
    const auto value = button.as<FrameworkElement>().DataContext().try_as<CamelPlayer::BrowseItem>();
    if (!value) return;
    auto* item = get_self<winrt::CamelPlayer::implementation::BrowseItem>(value);
    if (!item->row().file) return;
    item->setFavorite(!item->row().favorite);
    if (submit) submit(favoriteCommand({{item->row().file}}, item->row().favorite));
}

} // namespace ui
