// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "LibraryView.g.h"
#include "LibraryCollection.h"
#include "Server/Api/ServerApi.h"

namespace winrt::CamelPlayer::implementation {

struct LibraryView : LibraryViewT<LibraryView> {
    LibraryView() = default;
    void InitializeComponent();

    // The window supplies the Server and the command path its player bar follows.
    void attach(std::shared_ptr<camel::server::IServer> server, std::function<void(camel::server::Command)> submit);
    // albums, artists, composers, genres, years, tracks or folders; starts a fresh navigation history.
    void show(hstring const& tag, hstring const& title);
    void hide();

    void OnBackClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnSortChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void OnDescendingClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnAlbumClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::ItemClickEventArgs const&);
    void OnRowClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::ItemClickEventArgs const&);
    void OnTrailClicked(Microsoft::UI::Xaml::Controls::BreadcrumbBar const&,
                        Microsoft::UI::Xaml::Controls::BreadcrumbBarItemClickedEventArgs const&);
    void OnFolderItemClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::ItemClickEventArgs const&);

private:
    enum class Mode { Albums, Groups, Tracks, AlbumDetail, Folders };
    struct View {
        Mode mode = Mode::Albums;
        camel::server::LibraryGroupField field = camel::server::LibraryGroupField::Artist;
        std::optional<camel::server::LibraryGroupFilter> filter;
        int64_t album = 0;
        std::optional<int64_t> folder; // absent: the library roots
        hstring title, detail;
    };

    void open(View view, bool push);
    void showList(Mode mode);
    void fillSortChoices();
    void startCollection();
    // Explains an empty view: no library folder yet, or no album grouped by the scan yet.
    void showEmpty(int64_t total);
    void loadAlbumDetail();
    void loadFolder();
    void playTracks(const std::vector<camel::server::LibraryTrackRow>& tracks, size_t index);
    // Runs `work` off the UI thread and applies its result on it, unless the view moved on meanwhile.
    winrt::fire_and_forget background(std::function<std::function<void()>()> work);

    std::shared_ptr<camel::server::IServer> server_;
    std::function<void(camel::server::Command)> submit_;
    View current_;
    std::vector<View> back_;
    uint64_t generation_ = 0; // changes whenever another view opens
    camel::server::AlbumSort albumSort_ = camel::server::AlbumSort::Title;
    camel::server::TrackSort trackSort_ = camel::server::TrackSort::Title;
    bool descending_ = false;
    bool populating_ = false;
    std::optional<bool> hasRoots_;
    winrt::com_ptr<ui::LibraryCollection> collection_;
    std::vector<camel::server::LibraryTrackRow> detailTracks_, folderTracks_;
    std::vector<camel::server::LibraryFolderEntry> folderTrail_;
    Microsoft::UI::Dispatching::DispatcherQueueTimer refreshTimer_{nullptr};
};

} // namespace winrt::CamelPlayer::implementation

namespace winrt::CamelPlayer::factory_implementation {

struct LibraryView : LibraryViewT<LibraryView, implementation::LibraryView> {};

} // namespace winrt::CamelPlayer::factory_implementation
