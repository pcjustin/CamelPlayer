// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// What a right-click on a list row acts on: the whole selection when the row is part of it, the row alone when not.
#pragma once

#include <utility>

namespace ui {

// `items` finds the clicked row's index: the list's ItemsSource or its Items.
template <class Items>
bool actsOnSelection(winrt::Microsoft::UI::Xaml::Controls::ListViewBase const& list, Items const& items,
                     winrt::Windows::Foundation::IInspectable const& clicked) {
    uint32_t index = 0;
    if (!clicked || !items.IndexOf(clicked, index)) return false;
    for (const auto& range : list.SelectedRanges())
        if (std::cmp_greater_equal(index, range.FirstIndex()) && std::cmp_less_equal(index, range.LastIndex())) return true;
    return false;
}

// The selected indexes in list order.
template <class F> void forEachSelected(winrt::Microsoft::UI::Xaml::Controls::ListViewBase const& list, F&& f) {
    for (const auto& range : list.SelectedRanges())
        for (auto i = range.FirstIndex(); i <= range.LastIndex(); ++i) f(uint32_t(i));
}

} // namespace ui
