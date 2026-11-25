// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// What a right-click on a list row acts on: the whole selection when the row is part of it, the row alone when not;
// and moving the selected rows.
#pragma once

#include <algorithm>
#include <functional>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

#include "UiText.h"

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

// Moves the selected rows one place up (`step` -1) or down (+1) through the list's RemoveAt and InsertAt, gathered
// together as dragging them leaves them. Returns the index of the first moved row, or nothing when none moved; the
// moved rows stay selected and the first takes the focus.
template <class Items>
std::optional<uint32_t> moveSelected(winrt::Microsoft::UI::Xaml::Controls::ListViewBase const& list, Items& items, int step) {
    std::vector<uint32_t> selected;
    forEachSelected(list, [&](uint32_t i) { selected.push_back(i); });
    if (selected.empty()) return std::nullopt;
    std::ranges::sort(selected);
    const auto count = uint32_t(selected.size());
    const bool together = selected.back() - selected.front() + 1 == count;
    const auto position = uint32_t(std::clamp<int64_t>(int64_t(selected.front()) + step, 0, int64_t(items.Size() - count)));
    if (together && position == selected.front()) return std::nullopt;
    std::vector<winrt::Windows::Foundation::IInspectable> moved;
    moved.reserve(count);
    for (const auto i : selected) moved.push_back(items.GetAt(i));
    for (const auto i : std::views::reverse(selected)) items.RemoveAt(i);
    for (uint32_t k = 0; k < count; ++k) items.InsertAt(position + k, moved[k]);
    list.SelectRange(winrt::Microsoft::UI::Xaml::Data::ItemIndexRange(int32_t(position), count));
    list.ScrollIntoView(moved.front());
    list.UpdateLayout();
    if (const auto row = list.ContainerFromIndex(int32_t(position)).try_as<winrt::Microsoft::UI::Xaml::Controls::Control>())
        row.Focus(winrt::Microsoft::UI::Xaml::FocusState::Keyboard);
    return position;
}

// Alt+Up or Alt+Down, which move a list's selected rows: -1, +1, or 0 for any other key.
inline int moveStep(winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& e) {
    using winrt::Windows::System::VirtualKey;
    const auto key = e.Key();
    if ((key != VirtualKey::Up && key != VirtualKey::Down) || GetKeyState(VK_MENU) >= 0 || GetKeyState(VK_CONTROL) < 0 ||
        GetKeyState(VK_SHIFT) < 0)
        return 0;
    return key == VirtualKey::Up ? -1 : 1;
}

// Move up and Move down, inserted into a row menu at `at`.
inline void insertMoveMenu(winrt::Microsoft::UI::Xaml::Controls::MenuFlyout const& menu, uint32_t at,
                           const std::function<void(int)>& move) {
    using namespace winrt::Microsoft::UI::Xaml::Controls;
    const auto insert = [&](const wchar_t* key, const wchar_t* glyph, const wchar_t* keys, int step) {
        MenuFlyoutItem entry;
        entry.Text(tr(key));
        FontIcon icon;
        icon.Glyph(glyph);
        entry.Icon(icon);
        entry.KeyboardAcceleratorTextOverride(keys);
        entry.Click([move, step](auto&&, auto&&) { move(step); });
        menu.Items().InsertAt(at++, entry);
    };
    insert(L"Dsp_MoveUp", L"\xE70E", L"Alt+Up", -1);
    insert(L"Dsp_MoveDown", L"\xE70D", L"Alt+Down", 1);
}

} // namespace ui
