// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "HomeView.g.h"
#include <map>
#include <unordered_map>
#include "Server/Api/ServerApi.h"

namespace winrt::CamelPlayer::implementation {

struct HomeView : HomeViewT<HomeView> {
    HomeView() = default;
    ~HomeView() override { cancelCovers(); }

    struct Handlers {
        std::function<void(int64_t album, hstring const& title)> openAlbum;
        std::function<void(hstring const& artist)> openArtist;
        std::function<void(hstring const& list)> viewAll;     // played, added, topAlbums or topArtists
        std::function<void(hstring const& period)> openStats; // today, week, month, year or all
    };
    void attach(std::shared_ptr<camel::server::IServer> server, Handlers handlers);
    // Rereads totals and rows; called whenever Home is shown.
    winrt::fire_and_forget refresh();

    void OnAlbumClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::ItemClickEventArgs const&);
    void OnArtistClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::ItemClickEventArgs const&);
    void OnViewAllClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnStatTileTapped(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::TappedRoutedEventArgs const&);
    void OnRowSizeChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::SizeChangedEventArgs const&);
    void OnCoverLoaded(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnCoverUnloaded(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

private:
    // A row shows as many of its items as fit its width.
    void fill(Microsoft::UI::Xaml::Controls::ItemsControl const& row);
    void cancelCovers();

    std::shared_ptr<camel::server::IServer> server_;
    Handlers handlers_;
    uint64_t generation_ = 0;
    std::map<std::wstring, std::vector<Windows::Foundation::IInspectable>> rows_; // by the row's x:Name
    std::unordered_map<void*, camel::server::CancellationToken> covers_;
};

} // namespace winrt::CamelPlayer::implementation

namespace winrt::CamelPlayer::factory_implementation {

struct HomeView : HomeViewT<HomeView, implementation::HomeView> {};

} // namespace winrt::CamelPlayer::factory_implementation
