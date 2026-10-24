// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pch.h"

#include "BrowseItem.h"
#if __has_include("BrowseItem.g.cpp")
#include "BrowseItem.g.cpp"
#endif

namespace winrt::CamelPlayer::implementation {

void BrowseItem::fill(const ui::BrowseRow& row) {
    row_ = row;
    loaded_ = true;
    changed_(*this, Microsoft::UI::Xaml::Data::PropertyChangedEventArgs(L"")); // every property changed
}

} // namespace winrt::CamelPlayer::implementation
