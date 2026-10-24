// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "BrowseItem.g.h"

namespace ui {

// What a library row refers to; the view decides what clicking it does.
struct BrowseRow {
    int64_t id = 0;    // album, folder or track id
    std::string value; // group name, or the file path of a track
    winrt::hstring title, subtitle, detail, trailing;
    bool folder = false;
};

} // namespace ui

namespace winrt::CamelPlayer::implementation {

struct BrowseItem : BrowseItemT<BrowseItem> {
    BrowseItem() = default;

    hstring Title() const { return row_.title; }
    hstring Subtitle() const { return row_.subtitle; }
    hstring Detail() const { return row_.detail; }
    hstring Trailing() const { return row_.trailing; }
    bool IsFolder() const { return row_.folder; }
    bool IsTrack() const { return !row_.folder; }
    event_token PropertyChanged(Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler) {
        return changed_.add(handler);
    }
    void PropertyChanged(event_token const& token) noexcept { changed_.remove(token); }

    const ui::BrowseRow& row() const { return row_; }
    bool loaded() const { return loaded_; }
    void fill(const ui::BrowseRow& row);

private:
    ui::BrowseRow row_;
    bool loaded_ = false;
    event<Microsoft::UI::Xaml::Data::PropertyChangedEventHandler> changed_;
};

} // namespace winrt::CamelPlayer::implementation
