// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "HomeView.g.h"
#include <chrono>
#include <map>
#include <unordered_map>
#include "LibraryCollection.h"
#include "Server/Api/ServerApi.h"

namespace winrt::CamelPlayer::implementation {

struct HomeView : HomeViewT<HomeView> {
    HomeView() = default;
    ~HomeView() override { cancelCovers(); }

    struct Handlers {
        std::function<void(int64_t album, hstring const& title)> openAlbum;
        std::function<void(hstring const& artist)> openArtist;
        // played and added list every album; topAlbums, topArtists, topTracks and history come with the title and
        // rows of the chosen period.
        std::function<void(hstring const& list, hstring const& title, ui::BrowseFetch fetch)> viewAll;
        std::function<void(camel::server::Command)> submit;
    };
    void attach(std::shared_ptr<camel::server::IServer> server, Handlers handlers);
    // Rereads totals and rows; called whenever Home is shown.
    winrt::fire_and_forget refresh();

    void OnAlbumClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::ItemClickEventArgs const&);
    void OnArtistClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::ItemClickEventArgs const&);
    void OnTrackClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::ItemClickEventArgs const&);
    void OnViewAllClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnStatTileTapped(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::TappedRoutedEventArgs const&);
    void OnMetricChanged(Windows::Foundation::IInspectable const&,
                         Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void OnRowSizeChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::SizeChangedEventArgs const&);
    void OnChartSizeChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::SizeChangedEventArgs const&);
    void OnCoverLoaded(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnCoverUnloaded(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

private:
    // The days, weeks or months the chart shows for the chosen period, and the listening in each.
    struct ChartData {
        std::chrono::year_month_day first, last;
        camel::server::PlayBucketSize step = camel::server::PlayBucketSize::Day;
        std::vector<camel::server::PlayBucket> buckets;
    };

    // A row shows as many of its items as fit its width.
    void fill(Microsoft::UI::Xaml::Controls::ItemsControl const& row);
    void cancelCovers();
    // Reads the chart, the most played and the history of the chosen period.
    winrt::fire_and_forget loadPeriod();
    void showTiles();
    void drawChart();
    camel::server::PlayMetric metric();
    // The rows of a period list's "View all", paged like the library lists.
    ui::BrowseFetch periodFetch(std::wstring_view list);

    std::shared_ptr<camel::server::IServer> server_;
    Handlers handlers_;
    uint64_t generation_ = 0, periodGeneration_ = 0;
    std::map<std::wstring, std::vector<Windows::Foundation::IInspectable>> rows_; // by the row's x:Name
    std::unordered_map<void*, camel::server::CancellationToken> covers_;
    hstring period_ = L"month"; // the chosen card's tag
    ChartData chart_;
};

} // namespace winrt::CamelPlayer::implementation

namespace winrt::CamelPlayer::factory_implementation {

struct HomeView : HomeViewT<HomeView, implementation::HomeView> {};

} // namespace winrt::CamelPlayer::factory_implementation
