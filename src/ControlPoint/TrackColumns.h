// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "ColumnDivider.g.h"
#include "TrackColumnWidths.g.h"

#include <array>

#include "Common/settings/Settings.h"

namespace winrt::CamelPlayer::implementation {

struct TrackColumnWidths : TrackColumnWidthsT<TrackColumnWidths> {
    TrackColumnWidths() = default;

    Microsoft::UI::Xaml::GridLength Title() const { return star(columns_.title); }
    Microsoft::UI::Xaml::GridLength Artist() const { return star(columns_.artist); }
    Microsoft::UI::Xaml::GridLength Album() const { return star(columns_.album); }
    event_token PropertyChanged(Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler) {
        return changed_.add(handler);
    }
    void PropertyChanged(event_token const& token) noexcept { changed_.remove(token); }
    event_token Committed(Windows::Foundation::EventHandler<Windows::Foundation::IInspectable> const& handler) {
        return committed_.add(handler);
    }
    void Committed(event_token const& token) noexcept { committed_.remove(token); }

    const camel::TrackColumns& columns() const { return columns_; }
    void columns(const camel::TrackColumns& columns);
    void commit() { committed_(*this, nullptr); }

private:
    static Microsoft::UI::Xaml::GridLength star(double value) { return {value, Microsoft::UI::Xaml::GridUnitType::Star}; }
    camel::TrackColumns columns_;
    event<Microsoft::UI::Xaml::Data::PropertyChangedEventHandler> changed_;
    event<Windows::Foundation::EventHandler<Windows::Foundation::IInspectable>> committed_;
};

struct ColumnDivider : ColumnDividerT<ColumnDivider> {
    ColumnDivider();

    int32_t Boundary() const { return boundary_; }
    void Boundary(int32_t boundary) { boundary_ = boundary; }

private:
    int32_t boundary_ = 0;
    bool dragging_ = false;
    double startX_ = 0;
    std::array<double, 3> startWidths_{}; // of the title, artist and album columns as the drag began
};

} // namespace winrt::CamelPlayer::implementation

namespace winrt::CamelPlayer::factory_implementation {

struct TrackColumnWidths : TrackColumnWidthsT<TrackColumnWidths, implementation::TrackColumnWidths> {};
struct ColumnDivider : ColumnDividerT<ColumnDivider, implementation::ColumnDivider> {};

} // namespace winrt::CamelPlayer::factory_implementation

namespace ui {

// The widths every track list shares, from the application's resources.
winrt::CamelPlayer::implementation::TrackColumnWidths& trackColumns();

} // namespace ui
