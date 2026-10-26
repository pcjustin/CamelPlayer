// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "PlaylistView.g.h"
#include "PlaylistCollection.h"
#include "PlaylistMenu.h"
#include "Server/Api/ServerApi.h"

namespace winrt::CamelPlayer::implementation {

struct PlaylistView : PlaylistViewT<PlaylistView> {
    PlaylistView() = default;

    struct Handlers {
        std::function<void(camel::server::Command)> submit;
        std::function<void()> changed;          // playlists were added, renamed or deleted
        std::function<void(int64_t)> open;      // shows a playlist, or the Playlists page for 0
        std::function<void(hstring const& title, hstring const& message, bool error)> notify;
        std::function<HWND()> window;
        ui::PlaylistMenu menu;
    };
    void attach(std::shared_ptr<camel::server::IServer> server, Handlers handlers);
    void showAll();
    void showPlaylist(int64_t playlist);
    void hide();
    // Rereads the shown playlist when it is `playlist`, or the Playlists page.
    void reload(int64_t playlist);
    int64_t shown() const { return playlist_; }

    void OnPlayClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget OnRenameClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget OnExportClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget OnDeleteClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget OnImportClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget OnColumnClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnPlaylistClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::ItemClickEventArgs const&);
    void OnEntryDoubleTapped(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const&);
    void OnFavoriteClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    // Clicks on a row's heart stay its own: they neither play nor select the row.
    void OnMarksPointer(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e) {
        e.Handled(true);
    }
    void OnMarksDoubleTapped(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const& e) {
        e.Handled(true);
    }
    void OnEntryKeyDown(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const&);
    void OnEntryDragCompleted(Microsoft::UI::Xaml::Controls::ListViewBase const&,
                              Microsoft::UI::Xaml::Controls::DragItemsCompletedEventArgs const&);
    void OnEntryContextRequested(Microsoft::UI::Xaml::UIElement const&, Microsoft::UI::Xaml::Input::ContextRequestedEventArgs const&);

private:
    void load();
    void play(size_t index);
    // The selected entries in list order, or the right-clicked one when it is not selected.
    std::vector<ui::BrowseRow> selectedRows(Windows::Foundation::IInspectable const& clicked = nullptr);
    void submitMarks(camel::server::Command command);
    void queue(const std::vector<ui::BrowseRow>& rows, bool playNext);
    void remove(const std::vector<ui::BrowseRow>& rows);
    // Runs `edit` off the UI thread; a playlist changed elsewhere meanwhile is read again.
    void edit(camel::server::PlaylistEdit edit, bool reloadAfter);
    winrt::fire_and_forget background(std::function<std::function<void()>()> work);

    std::shared_ptr<camel::server::IServer> server_;
    Handlers handlers_;
    int64_t playlist_ = 0; // 0: the Playlists page
    int64_t revision_ = 0;
    hstring name_;
    uint64_t generation_ = 0;
    std::optional<camel::server::PlaylistSortKey> sortKey_;
    bool sortDescending_ = false;
    winrt::com_ptr<ui::PlaylistCollection> entries_ = winrt::make_self<ui::PlaylistCollection>();
    std::vector<camel::server::PlaylistSummary> playlists_;
    Microsoft::UI::Xaml::Controls::MenuFlyout entryMenu_{nullptr};
    Windows::Foundation::IInspectable menuItem_{nullptr};
};

} // namespace winrt::CamelPlayer::implementation

namespace winrt::CamelPlayer::factory_implementation {

struct PlaylistView : PlaylistViewT<PlaylistView, implementation::PlaylistView> {};

} // namespace winrt::CamelPlayer::factory_implementation
