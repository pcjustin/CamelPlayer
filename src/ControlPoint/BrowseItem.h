// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "ArtistAvatar.h"
#include "BrowseItem.g.h"

namespace ui {

// What a library row refers to; the view decides what clicking it does.
struct BrowseRow {
    int64_t id = 0;    // album, folder or track id
    std::string value; // group name, or the file path of a track
    winrt::hstring title, subtitle, detail, trailing;
    winrt::hstring number, fileName; // a folder page track's album place and file
    bool folder = false;
    int64_t file = 0;         // a track's file, which playlists refer to
    bool unavailable = false; // a playlist entry whose file is missing or offline
    bool marks = false;       // shows the track's favorite heart
    bool favorite = false;
    bool composer = false; // a group row naming a composer, which lists of favorite artists mix with artists
    bool avatar = false;   // an artist or composer row, shown with their picture
    int depth = -1;        // a folder tree row: how far below the music folders it is
    bool expanded = false; // a folder tree row whose folder is open
};

} // namespace ui

namespace winrt::CamelPlayer::implementation {

struct BrowseItem : BrowseItemT<BrowseItem> {
    BrowseItem() = default;

    hstring Title() const { return row_.title; }
    hstring Subtitle() const { return row_.subtitle; }
    hstring Detail() const { return row_.detail; }
    hstring Trailing() const { return row_.trailing; }
    hstring Number() const { return row_.number; }
    hstring FileName() const { return row_.fileName; }
    bool IsFolder() const { return row_.folder; }
    bool IsTrack() const { return !row_.folder && row_.id != 0 && !row_.value.empty(); }
    bool IsUnavailable() const { return row_.unavailable; }
    bool HasMarks() const { return row_.marks; }
    bool IsFavorite() const { return row_.favorite; }
    hstring FavoriteGlyph() const { return row_.favorite ? L"\xE00B" : L"\xE006"; }
    bool HasAvatar() const { return row_.avatar; }
    hstring Initial() const { return ui::initialOf(row_.title); }
    Microsoft::UI::Xaml::Media::Brush AvatarBrush() const {
        return Microsoft::UI::Xaml::Media::SolidColorBrush(ui::avatarColor(row_.title));
    }
    Microsoft::UI::Xaml::Thickness Indent() const { return {20.0 * std::max(row_.depth, 0), 0, 0, 0}; }
    hstring ChevronGlyph() const { return !row_.folder ? L"" : row_.expanded ? L"\xE70D" : L"\xE76C"; }
    hstring TreeGlyph() const { return row_.folder ? L"\xE8B7" : L"\xE8D6"; }
    // What a screen reader announces for the tile or row.
    hstring ToString() const { return row_.subtitle.empty() ? row_.title : row_.title + L", " + row_.subtitle; }
    event_token PropertyChanged(Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler) {
        return changed_.add(handler);
    }
    void PropertyChanged(event_token const& token) noexcept { changed_.remove(token); }

    const ui::BrowseRow& row() const { return row_; }
    bool loaded() const { return loaded_; }
    void fill(const ui::BrowseRow& row);
    void setFavorite(bool favorite);
    void setExpanded(bool expanded);

private:
    ui::BrowseRow row_;
    bool loaded_ = false;
    event<Microsoft::UI::Xaml::Data::PropertyChangedEventHandler> changed_;
};

} // namespace winrt::CamelPlayer::implementation
