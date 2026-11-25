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
#include <optional>
#include <utility>
#include <vector>

#include "UiText.h"

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

// The widths with the boundary after the column `left` moved by `by`, each of the pair kept at least kNarrowest wide.
std::optional<camel::TrackColumns> resized(const std::array<double, 3>& widths, size_t left, double by) {
    const size_t right = left + 1;
    const double pair = widths[left] + widths[right];
    if (pair < 2 * kNarrowest) return std::nullopt;
    auto out = widths;
    out[left] = std::clamp(widths[left] + by, kNarrowest, pair - kNarrowest);
    out[right] = pair - out[left];
    return camel::TrackColumns{out[0], out[1], out[2]};
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
    // In the theme the window shows, which a change in Settings gives the content alone, not the Application: known once
    // the divider is in the window.
    // Focused from the keyboard, the line is drawn wider in the accent colour.
    const auto paint = [line](Windows::Foundation::IInspectable const& divider, auto&&) {
        const auto self = divider.as<FrameworkElement>();
        const bool focused = self.FocusState() == FocusState::Keyboard;
        line.Width(focused ? 3 : 1);
        line.Fill(ui::shapeBrush(self, focused ? L"AccentFillColorDefaultBrush" : L"DividerStrokeColorDefaultBrush"));
    };
    Loaded([this, paint](Windows::Foundation::IInspectable const& divider, RoutedEventArgs const& e) {
        const wchar_t* names[] = {L"Library_SortTitle", L"Library_SortArtist", L"Library_SortAlbum"};
        if (boundary_ >= 0 && boundary_ <= 1)
            Automation::AutomationProperties::SetName(
                *this, ui::format(L"Columns_Divider",
                                  {std::wstring(ui::tr(names[boundary_])), std::wstring(ui::tr(names[boundary_ + 1]))}));
        Automation::AutomationProperties::SetHelpText(*this, ui::tr(L"Columns_DividerHelp"));
        paint(divider, e);
    });
    ui::followTheme(*this, [this, paint] { paint(*this, nullptr); });
    GotFocus(paint);
    LostFocus(paint);
    Children().Append(line);
    IsTabStop(true);
    // Left and Right move the boundary as dragging does; Home returns every column to 3:2:2, as a double-click does.
    KeyDown([this](auto&&, Input::KeyRoutedEventArgs const& e) {
        using Windows::System::VirtualKey;
        const auto key = e.Key();
        if (key == VirtualKey::Home) {
            ui::trackColumns().columns({});
            ui::trackColumns().commit();
            e.Handled(true);
            return;
        }
        if (key != VirtualKey::Left && key != VirtualKey::Right) return;
        e.Handled(true);
        const auto grid = Parent().try_as<Controls::Grid>();
        if (!grid || boundary_ < 0 || boundary_ > 1) return;
        const auto columns = starColumns(grid);
        if (columns.size() < 3) return;
        std::array<double, 3> widths{};
        for (size_t i = 0; i < 3; ++i) widths[i] = grid.ColumnDefinitions().GetAt(columns[i]).ActualWidth();
        if (const auto moved = resized(widths, size_t(boundary_), key == VirtualKey::Left ? -16.0 : 16.0)) {
            ui::trackColumns().columns(*moved);
            ui::trackColumns().commit();
        }
    });

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
        const double by = e.GetCurrentPoint(Parent().as<UIElement>()).Position().X - startX_;
        const auto moved = resized(startWidths_, size_t(boundary_), by);
        if (!moved) return;
        ui::trackColumns().columns(*moved);
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
