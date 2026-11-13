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

// Read each time: a change of language opens a new window in the same process.
hstring QueueItem::RemoveLabel() const {
    return ui::tr(L"Queue_RemoveEntry");
}

void QueueItem::fill(const ui::QueueRow& row, bool current, const camel::server::QueueEntry* entry) {
    entry_ = entry;
    if (row_ == row && current_ == current) return;
    row_ = row;
    current_ = current;
    changed_(*this, Microsoft::UI::Xaml::Data::PropertyChangedEventArgs(L"")); // every property changed
}

} // namespace winrt::CamelPlayer::implementation
