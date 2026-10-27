// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "StatisticsView.g.h"
#include "LibraryCollection.h"
#include "Server/Api/ServerApi.h"

namespace winrt::CamelPlayer::implementation {

struct StatisticsView : StatisticsViewT<StatisticsView> {
    StatisticsView() = default;

    struct Handlers {
        std::function<void(camel::server::Command)> submit;
        std::function<void(int64_t album, hstring const& title)> openAlbum;
        std::function<void(hstring const& artist)> openArtist;
    };
    // Home's tiles open the page on their period.
    enum class Range : uint8_t { Today, ThisWeek, ThisMonth, ThisYear, AllTime, Last7Days, Last30Days };

    void attach(std::shared_ptr<camel::server::IServer> server, Handlers handlers);
    void show(Range range);
    void hide();

    void OnPeriodChanged(Windows::Foundation::IInspectable const&,
                         Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void OnRangeChanged(Microsoft::UI::Xaml::Controls::CalendarDatePicker const&,
                        Microsoft::UI::Xaml::Controls::CalendarDatePickerDateChangedEventArgs const&);
    void OnRankChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void OnChartSizeChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::SizeChangedEventArgs const&);
    void OnRowDoubleTapped(Windows::Foundation::IInspectable const&,
                           Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const&);
    void OnRowContextRequested(Microsoft::UI::Xaml::UIElement const&,
                               Microsoft::UI::Xaml::Input::ContextRequestedEventArgs const&);
    winrt::fire_and_forget OnResetAllClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

private:
    camel::server::PlayPeriod period();
    void load();
    void showRanking();
    void drawChart();
    winrt::fire_and_forget resetSelected(Microsoft::UI::Xaml::Controls::ListView list, Windows::Foundation::IInspectable clicked);
    void reloadWhenIdle();
    winrt::fire_and_forget background(std::function<std::function<void()>()> work);

    std::shared_ptr<camel::server::IServer> server_;
    Handlers handlers_;
    uint64_t generation_ = 0;
    bool populating_ = false;
    camel::server::PlayStatistics stats_;
    winrt::com_ptr<ui::LibraryCollection> history_;
    Microsoft::UI::Xaml::Controls::MenuFlyout rowMenu_{nullptr};
    Microsoft::UI::Xaml::Controls::ListView menuList_{nullptr};
    Windows::Foundation::IInspectable menuItem_{nullptr};
};

} // namespace winrt::CamelPlayer::implementation

namespace winrt::CamelPlayer::factory_implementation {

struct StatisticsView : StatisticsViewT<StatisticsView, implementation::StatisticsView> {};

} // namespace winrt::CamelPlayer::factory_implementation
