// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pch.h"

#include "TrackColumns.h"
#if __has_include("TrackColumnWidths.g.cpp")
#include "TrackColumnWidths.g.cpp"
#endif
#if __has_include("ColumnDivider.g.cpp")
#include "ColumnDivider.g.cpp"
#include "ThemeBrush.h"
#endif

#include <algorithm>
#include <utility>
#include <vector>

#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace ui {

CamelPlayer::implementation::TrackColumnWidths& trackColumns() {
    const auto shared =
        Application::Current().Resources().Lookup(box_value(L"TrackColumns")).as<CamelPlayer::TrackColumnWidths>();
    return *get_self<CamelPlayer::implementation::TrackColumnWidths>(shared);
}

} // namespace ui

namespace winrt::CamelPlayer::implementation {

namespace {

constexpr double kNarrowest = 80; // a column keeps room for a few characters and its "…"

// The title, artist and album columns: the first three that share the room in proportion.
std::vector<uint32_t> starColumns(Controls::Grid const& grid) {
    std::vector<uint32_t> out;
    const auto columns = grid.ColumnDefinitions();
    for (uint32_t i = 0; i < columns.Size() && out.size() < 3; ++i)
        if (columns.GetAt(i).Width().GridUnitType == GridUnitType::Star) out.push_back(i);
    return out;
}

} // namespace

void TrackColumnWidths::columns(const camel::TrackColumns& columns) {
    if (columns == columns_) return;
    columns_ = columns;
    for (const auto* name : {L"Title", L"Artist", L"Album"}) changed_(*this, Data::PropertyChangedEventArgs(name));
}

ColumnDivider::ColumnDivider() {
    Width(16);
    Background(Media::SolidColorBrush(Microsoft::UI::Colors::Transparent()));
    ProtectedCursor(Microsoft::UI::Input::InputSystemCursor::Create(Microsoft::UI::Input::InputSystemCursorShape::SizeWestEast));
    Shapes::Rectangle line;
    line.Width(1);
    line.Margin(ThicknessHelper::FromLengths(0, 4, 0, 4));
    line.HorizontalAlignment(HorizontalAlignment::Center);
    // In the theme the window shows, which a change in Settings gives the content alone, not the Application.
    line.Fill(ui::themeBrush(L"DividerStrokeColorDefaultBrush"));
    ActualThemeChanged([line](FrameworkElement const& divider, auto&&) {
        ui::setTheme(divider.ActualTheme());
        line.Fill(ui::themeBrush(L"DividerStrokeColorDefaultBrush"));
    });
    Children().Append(line);

    PointerPressed([this](auto&&, Input::PointerRoutedEventArgs const& e) {
        const auto grid = Parent().try_as<Controls::Grid>();
        if (!grid) return;
        const auto columns = starColumns(grid);
        if (columns.size() < 3 || boundary_ < 0 || boundary_ > 1) return;
        for (size_t i = 0; i < 3; ++i) startWidths_[i] = grid.ColumnDefinitions().GetAt(columns[i]).ActualWidth();
        startX_ = e.GetCurrentPoint(grid).Position().X;
        dragging_ = CapturePointer(e.Pointer());
        e.Handled(true);
    });
    PointerMoved([this](auto&&, Input::PointerRoutedEventArgs const& e) {
        if (!dragging_) return;
        const auto left = size_t(boundary_), right = left + 1;
        const double pair = startWidths_[left] + startWidths_[right];
        if (pair < 2 * kNarrowest) return;
        auto widths = startWidths_;
        const double moved = e.GetCurrentPoint(Parent().as<UIElement>()).Position().X - startX_;
        widths[left] = std::clamp(startWidths_[left] + moved, kNarrowest, pair - kNarrowest);
        widths[right] = pair - widths[left];
        ui::trackColumns().columns({widths[0], widths[1], widths[2]});
        e.Handled(true);
    });
    const auto release = [this](auto&&, Input::PointerRoutedEventArgs const&) {
        if (!std::exchange(dragging_, false)) return;
        ReleasePointerCaptures();
        ui::trackColumns().commit();
    };
    PointerReleased(release);
    PointerCaptureLost(release);
    DoubleTapped([](auto&&, Input::DoubleTappedRoutedEventArgs const& e) {
        ui::trackColumns().columns({});
        ui::trackColumns().commit();
        e.Handled(true);
    });
}

} // namespace winrt::CamelPlayer::implementation
