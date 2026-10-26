// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "QueueItem.g.h"

namespace ui {

struct QueueRow {
    uint64_t id = 0; // queue entry id
    winrt::hstring title, path;
};

} // namespace ui

namespace winrt::CamelPlayer::implementation {

struct QueueItem : QueueItemT<QueueItem> {
    QueueItem() = default;

    hstring Title() const { return row_.title; }
    hstring Path() const { return row_.path; }
    bool IsCurrent() const { return current_; }
    bool IsNotCurrent() const { return !current_; }
    hstring RemoveLabel() const;
    event_token PropertyChanged(Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler) {
        return changed_.add(handler);
    }
    void PropertyChanged(event_token const& token) noexcept { changed_.remove(token); }

    const ui::QueueRow& row() const { return row_; }
    void fill(const ui::QueueRow& row, bool current);

private:
    ui::QueueRow row_;
    bool current_ = false;
    event<Microsoft::UI::Xaml::Data::PropertyChangedEventHandler> changed_;
};

} // namespace winrt::CamelPlayer::implementation
