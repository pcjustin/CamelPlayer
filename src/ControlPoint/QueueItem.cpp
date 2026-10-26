// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pch.h"

#include "QueueItem.h"
#if __has_include("QueueItem.g.cpp")
#include "QueueItem.g.cpp"
#endif

#include "UiText.h"

namespace winrt::CamelPlayer::implementation {

hstring QueueItem::RemoveLabel() const {
    static const hstring label = ui::tr(L"Queue_RemoveEntry");
    return label;
}

void QueueItem::fill(const ui::QueueRow& row, bool current) {
    if (row_.id == row.id && row_.title == row.title && row_.path == row.path && current_ == current) return;
    row_ = row;
    current_ = current;
    changed_(*this, Microsoft::UI::Xaml::Data::PropertyChangedEventArgs(L"")); // every property changed
}

} // namespace winrt::CamelPlayer::implementation
