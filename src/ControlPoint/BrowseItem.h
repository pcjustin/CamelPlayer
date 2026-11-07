// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "ArtistAvatar.h"
#include "UiText.h"
#include "BrowseItem.g.h"

namespace ui {

// What a library row refers to; the view decides what clicking it does.
struct BrowseRow {
    int64_t id = 0;    // album, folder or track id
    std::string value; // group name, or the file path of a track
    winrt::hstring title, subtitle, detail, trailing;
    winrt::hstring number, fileName; // a track's place on its album, and its file on a folder page
    bool folder = false;
    bool album = false;       // an album's card
    int64_t file = 0;         // a track's file, which playlists refer to
    bool unavailable = false; // a playlist entry whose file is missing or offline
    bool marks = false;       // shows the track's favorite heart
    bool favorite = false;
    bool composer = false; // a group row naming a composer, which lists of favorite artists mix with artists
    bool avatar = false;   // an artist or composer row, shown with their picture
    int depth = -1;        // a folder tree row: how far below the music folders it is
    bool expanded = false; // a folder tree row whose folder is open
};

// The track the queue is on while it plays or pauses, which rows mark; read and changed on the UI thread only.
struct NowPlaying {
    int64_t file = 0; // 0: stopped, or a file outside the library
    bool paused = false;
    int64_t album = 0;
    std::vector<int64_t> folders; // the file's library folder down to its own folder, once read
    bool operator==(const NowPlaying&) const = default;
    // Whether `row` is the track, its album or a folder on its path.
    bool marks(const BrowseRow& row) const {
        return (row.file != 0 && row.file == file) || (row.album && row.id != 0 && row.id == album) ||
               (row.folder && std::ranges::find(folders, row.id) != folders.end());
    }
};

const NowPlaying& nowPlaying();
// Records what plays now and returns what played before, or nothing when that did not change.
std::optional<NowPlaying> setNowPlaying(NowPlaying playing);

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
    bool ShowsNumber() const { return !row_.number.empty() && !IsPlaying(); }
    bool IsPlaying() const { return ui::nowPlaying().marks(row_); }
    bool IsNotPlaying() const { return !IsPlaying(); }
    hstring PlayingGlyph() const { return ui::nowPlaying().paused ? L"\xE769" : L"\xE767"; }
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
    hstring ToString() const {
        const auto title = row_.number.empty() ? row_.title : row_.number + L" " + row_.title;
        const auto named = row_.subtitle.empty() ? title : title + L", " + row_.subtitle;
        if (!IsPlaying()) return named;
        return named + L", " + ui::tr(ui::nowPlaying().paused ? L"Library_PausedMark" : L"Library_PlayingMark");
    }
    event_token PropertyChanged(Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler) {
        return changed_.add(handler);
    }
    void PropertyChanged(event_token const& token) noexcept { changed_.remove(token); }

    const ui::BrowseRow& row() const { return row_; }
    bool loaded() const { return loaded_; }
    void fill(const ui::BrowseRow& row);
    void setFavorite(bool favorite);
    void setExpanded(bool expanded);
    // Updates the playing mark when this row was marked for what played before, `was`, or is for what plays now.
    void playingChanged(const ui::NowPlaying& was);

private:
    ui::BrowseRow row_;
    bool loaded_ = false;
    event<Microsoft::UI::Xaml::Data::PropertyChangedEventHandler> changed_;
};

} // namespace winrt::CamelPlayer::implementation
