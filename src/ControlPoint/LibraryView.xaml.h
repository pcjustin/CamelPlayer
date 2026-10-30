// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "LibraryView.g.h"
#include "LibraryCollection.h"
#include "PlaylistMenu.h"
#include "TrackMarks.h"
#include <unordered_map>
#include "Server/Api/ServerApi.h"

namespace winrt::CamelPlayer::implementation {

struct LibraryView : LibraryViewT<LibraryView> {
    LibraryView() = default;
    ~LibraryView() override { cancelArtwork(); }
    void InitializeComponent();

    // The window supplies the Server, the command path its player bar follows, the playlist menu entries and how a
    // smart list is saved as a playlist named after it.
    void attach(std::shared_ptr<camel::server::IServer> server, std::function<void(camel::server::Command)> submit,
                ui::PlaylistMenu playlistMenu, std::function<void(ui::SourceFetch, std::string name)> savePlaylist);
    // albums, artists, composers, genres, years, tracks, folders or a smart list (smart:added, smart:mostPlayed,
    // smart:recentlyPlayed, smart:favorites, smart:favoriteAlbums, smart:neverPlayed); starts a fresh navigation history.
    void show(hstring const& tag, hstring const& title);
    // `leave`, when set, is what Back runs: return to the page the album or artist was opened from. `keepHistory` opens
    // it over the current library view instead, so Back returns there.
    void showAlbum(int64_t album, hstring const& title, std::function<void()> leave = nullptr, bool keepHistory = false);
    void showArtist(hstring const& artist, std::function<void()> leave = nullptr, bool keepHistory = false);
    // Full results for `query`. `leave` returns to the page the search started from when it was not a library page.
    void showSearch(hstring const& query, std::function<void()> leave);
    // Home's "View all": an album wall, artist list or track list read through `fetch`; Back runs `leave`.
    void showAlbumList(hstring const& title, ui::BrowseFetch fetch, std::function<void()> leave);
    void showArtistList(hstring const& title, ui::BrowseFetch fetch, std::function<void()> leave);
    void showTrackList(hstring const& title, ui::BrowseFetch fetch, std::function<void()> leave);
    void hide();
    // Stops refreshing while the window sits in the notification area; the view keeps its place.
    void setActive(bool active);
    // Whether Back has somewhere to go; `changed` hears it after every page change. The window's title bar shows Back.
    bool canGoBack() const;
    void goBack();
    void setBackChanged(std::function<void(bool)> changed) { backChanged_ = std::move(changed); }

    void OnBackClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnSortChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void OnDescendingClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnAlbumClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::ItemClickEventArgs const&);
    void OnRowClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::ItemClickEventArgs const&);
    void OnTrailClicked(Microsoft::UI::Xaml::Controls::BreadcrumbBar const&,
                        Microsoft::UI::Xaml::Controls::BreadcrumbBarItemClickedEventArgs const&);
    void OnFolderItemClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::ItemClickEventArgs const&);
    void OnAddAlbumClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnPlayNextAlbumClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnPlayAlbumClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnSearchRowScroll(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnSearchArtistClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::ItemClickEventArgs const&);
    void OnRowContextRequested(Microsoft::UI::Xaml::UIElement const&,
                               Microsoft::UI::Xaml::Input::ContextRequestedEventArgs const&);
    void OnCardContextRequested(Microsoft::UI::Xaml::UIElement const&,
                                Microsoft::UI::Xaml::Input::ContextRequestedEventArgs const&);

    void OnArtworkLoaded(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnInitialsBarSizeChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::SizeChangedEventArgs const&);
    void OnAlbumGridSizeChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::SizeChangedEventArgs const&);
    void OnRowListSizeChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::SizeChangedEventArgs const&);
    void OnRowContainerContentChanging(Microsoft::UI::Xaml::Controls::ListViewBase const&,
                                       Microsoft::UI::Xaml::Controls::ContainerContentChangingEventArgs const&);
    void OnArtworkUnloaded(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnArtworkContextChanged(Microsoft::UI::Xaml::FrameworkElement const&,
                                 Microsoft::UI::Xaml::DataContextChangedEventArgs const&);
    void OnFavoriteClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    // Clicks on a row's heart stay its own: they neither play nor select the row.
    void OnMarksPointer(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& e) {
        e.Handled(true);
    }
    void OnMarksDoubleTapped(Windows::Foundation::IInspectable const&,
                             Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const& e) {
        e.Handled(true);
    }
    void OnAlbumFavoriteClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnPlayListClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnShuffleListClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnSaveListClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

private:
    struct ArtworkSlot {
        CamelPlayer::BrowseItem::PropertyChanged_revoker changed;
        camel::server::CancellationToken token;
    };
    void bindArtwork(Microsoft::UI::Xaml::Controls::Image const& image);
    void loadRowArtwork(Microsoft::UI::Xaml::Controls::Image const& image);
    void cancelArtwork();
    std::unordered_map<void*, ArtworkSlot> artworkSlots_;
    camel::server::CancellationToken detailArtwork_;
    enum class Mode { Albums, Groups, Tracks, AlbumDetail, Folders, Search };
    struct View {
        Mode mode = Mode::Albums;
        camel::server::LibraryGroupField field = camel::server::LibraryGroupField::Artist;
        std::optional<camel::server::LibraryGroupFilter> filter;
        int64_t album = 0;
        std::optional<int64_t> folder; // absent: the library roots
        std::string query;             // search text
        ui::BrowseFetch fetch;         // replaces the browse query for Home's lists
        std::optional<camel::server::SmartList> smart;
        bool favoriteAlbums = false;
        bool smartList() const { return smart || favoriteAlbums; }
        hstring title, detail;
        double offset = 0; // where its list was scrolled when another view opened over it
    };

    void open(View view, bool push);
    void showList(Mode mode);
    void fitAlbumTracks();
    Microsoft::UI::Xaml::Controls::ListViewBase listOf(Mode mode);
    // Scrolls a list reopened by Back to where it was, once its items are there.
    void restoreOffset();
    void fillSortChoices();
    void startCollection();
    // Explains an empty view: no library folder yet, or no album grouped by the scan yet.
    void showEmpty(int64_t total);
    void showCount(int64_t total);
    void loadInitials();
    void showInitials(std::vector<camel::server::LibraryInitial> initials);
    void layoutInitials();
    void loadAlbumDetail();
    void loadFolder();
    void loadSearchGroups();
    void playTracks(const std::vector<camel::server::LibraryTrackRow>& tracks, size_t index);
    void queueItem(const ui::BrowseRow& row, bool playNext);
    void queueAlbum(int64_t album, bool playNext);
    void playAlbum(int64_t album);
    void queueFolder(int64_t folder, bool playNext, bool playNow = false);
    void queueTracks(const std::vector<camel::server::LibraryTrackRow>& tracks, bool playNext);
    // What a context menu acts on: rows of the current view, album cards or artist chips.
    enum class Target { Rows, Albums, Artists };
    static std::optional<camel::server::PlaylistSource> sourceOf(Target target, Mode mode, camel::server::LibraryGroupField field,
                                                                 const ui::BrowseRow& row);
    // The selected rows in list order, read off the UI thread; just `clicked` when it is not part of the selection.
    // A paged list reads the selected rows it has not loaded through the current listing.
    std::function<std::vector<ui::BrowseRow>()> selectionRows(Microsoft::UI::Xaml::Controls::ListViewBase const& list,
                                                              Windows::Foundation::IInspectable const& clicked, bool paged) const;
    ui::SourceFetch menuSources() const;
    void queueSelection(bool playNext);
    // Every row of the current smart list, read off the UI thread.
    std::function<std::vector<ui::BrowseRow>()> listRows() const;
    void playList(bool shuffle);
    // Marks or clears the menu's tracks or albums as favorites; paged lists reread once the Server applied the change.
    void markSelection(bool favorite);
    // Deletes the plays of the selected tracks, or of the right-clicked one, after a confirmation; not undoable.
    void resetSelection();
    winrt::fire_and_forget confirmReset(std::vector<camel::server::LibraryTrackKey> tracks);
    void markAlbums(bool favorite);
    void reloadWhenIdle();
    void showAlbumFavorite(bool favorite);
    void showAlbumFormats(const std::vector<camel::server::LibraryAlbumFormat>& formats);
    void showMenu(Microsoft::UI::Xaml::Controls::MenuFlyout const& menu, Microsoft::UI::Xaml::FrameworkElement const& element,
                  Microsoft::UI::Xaml::Input::ContextRequestedEventArgs const& e);
    // Runs `work` off the UI thread and applies its result on it, unless the view moved on meanwhile.
    winrt::fire_and_forget background(std::function<std::function<void()>()> work);

    std::shared_ptr<camel::server::IServer> server_;
    std::function<void(camel::server::Command)> submit_;
    ui::PlaylistMenu playlistMenu_;
    ui::BrowseFetch fetch_; // the current collection's listing
    View current_;
    std::vector<View> back_;
    std::optional<double> restoreOffset_;
    std::function<void()> leave_; // Back from a search that began outside the library pages
    std::function<void(bool)> backChanged_;
    std::function<void(ui::SourceFetch, std::string)> savePlaylist_;
    uint64_t generation_ = 0; // changes whenever another view opens
    camel::server::AlbumSort albumSort_ = camel::server::AlbumSort::Title;
    camel::server::TrackSort trackSort_ = camel::server::TrackSort::Title;
    bool descending_ = false;
    bool populating_ = false;
    std::optional<bool> hasRoots_;
    winrt::com_ptr<ui::LibraryCollection> collection_;
    std::vector<camel::server::LibraryInitial> initials_;
    size_t initialSlots_ = 0; // rows the jump bar shows now
    std::vector<camel::server::LibraryTrackRow> detailTracks_, folderTracks_;
    // The album page's track artists split into names, by credit; empty on other pages.
    std::unordered_map<std::string, std::vector<camel::server::LibraryNamePart>> credits_;
    std::vector<camel::server::LibraryFolderEntry> folderTrail_;
    Microsoft::UI::Dispatching::DispatcherQueueTimer refreshTimer_{nullptr};
    Microsoft::UI::Xaml::Controls::MenuFlyout rowMenu_{nullptr}, cardMenu_{nullptr};
    std::vector<Microsoft::UI::Xaml::Controls::MenuFlyoutItemBase> rowMarks_; // favorites and statistics, for tracks only
    uint32_t cardMarksFirst_ = 0;                                             // album entries, hidden for artists
    ui::BrowseRow menuRow_;
    Windows::Foundation::IInspectable menuItem_{nullptr};
    Microsoft::UI::Xaml::Controls::ListViewBase menuList_{nullptr};
    Target menuTarget_ = Target::Rows;
    bool menuPaged_ = false;
};

} // namespace winrt::CamelPlayer::implementation

namespace winrt::CamelPlayer::factory_implementation {

struct LibraryView : LibraryViewT<LibraryView, implementation::LibraryView> {};

} // namespace winrt::CamelPlayer::factory_implementation
