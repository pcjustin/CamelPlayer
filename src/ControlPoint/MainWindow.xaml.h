// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "MainWindow.g.h"
#include "LibraryView.xaml.h"
#include "PlaylistView.xaml.h"
#include "QueueCollection.h"
#include "Server/Api/ServerApi.h"
#include "TrayIcon.h"
#include "GlobalHotkeys.h"
#include "MediaControls.h"

namespace winrt::CamelPlayer::implementation {

struct MainWindow : MainWindowT<MainWindow> {
    explicit MainWindow(std::shared_ptr<camel::server::IServer> server = {}) : server_(std::move(server)) {}
    // C++/WinRT calls this after construction; named XAML elements exist from here on.
    void InitializeComponent();

    // XAML event handlers
    void OnPlayPauseClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnStopClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnPreviousClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnNextClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnShuffleClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnRepeatClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget OnSearchTextChanged(Microsoft::UI::Xaml::Controls::AutoSuggestBox const&,
                                               Microsoft::UI::Xaml::Controls::AutoSuggestBoxTextChangedEventArgs const&);
    void OnSearchQuerySubmitted(Microsoft::UI::Xaml::Controls::AutoSuggestBox const&,
                                Microsoft::UI::Xaml::Controls::AutoSuggestBoxQuerySubmittedEventArgs const&);
    void OnSearchAccelerator(Microsoft::UI::Xaml::Input::KeyboardAccelerator const&,
                             Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const&);
    winrt::fire_and_forget OnResetClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnOpenDataFolderClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget OnDiagnosticBundleClick(Windows::Foundation::IInspectable const&,
                                                   Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnGuideClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnFormatClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnQueueToggleClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnQueueClearClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget OnQueueSaveClick(Windows::Foundation::IInspectable const&,
                                            Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnQueueDoubleTapped(Windows::Foundation::IInspectable const&,
                             Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const&);
    void OnQueueRemoveEntryClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnQueueKeyDown(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const&);
    void OnQueueContextRequested(Microsoft::UI::Xaml::UIElement const&,
                                 Microsoft::UI::Xaml::Input::ContextRequestedEventArgs const&);
    void OnQueueDragItemsCompleted(Microsoft::UI::Xaml::Controls::ListViewBase const&,
                                   Microsoft::UI::Xaml::Controls::DragItemsCompletedEventArgs const&);
    void OnCoverClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnPlaybackFavoriteClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void showPlaybackFavorite(bool favorite);
    void OnAlbumLinkClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnArtistLinkClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnSliderValueChanged(Windows::Foundation::IInspectable const&,
                              Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const&);
    void OnNavSelectionChanged(Microsoft::UI::Xaml::Controls::NavigationView const&,
                               Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const&);
    // Settings › Playback and the queue panel switch the same autoplay setting (docs/Plan.md §8.1).
    void OnAutoplayToggled(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnHotkeyDefaultsClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    // Shortcuts while the window is in front (docs/Plan.md §8.4); never while text is typed or a dialog or menu is open.
    void OnWindowKeyDown(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const&);
    void OnSettingChanged(Windows::Foundation::IInspectable const&,
                          Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void OnNumberChanged(Microsoft::UI::Xaml::Controls::NumberBox const&,
                         Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const&);
    void OnLanguageChanged(Windows::Foundation::IInspectable const&,
                           Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    winrt::fire_and_forget OnAsioPanelClick(Windows::Foundation::IInspectable const&,
                                            Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnRefreshDevicesClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget OnAddFolderClick(Windows::Foundation::IInspectable const&,
                                            Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnRemoveFolderClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

private:
    void populateSettings(); // fills every control from settings_
    void populateDeviceLists(const camel::server::DeviceCatalog& devices);
    void readSettingsFromUi(); // controls -> settings_
    // One decoder selector per format that has integrated decoders.
    std::array<std::pair<Microsoft::UI::Xaml::Controls::ComboBox, camel::audio::AudioFormat>, 6> decoderCombos();
    // The output-depth selector of each lossy format.
    std::array<std::pair<Microsoft::UI::Xaml::Controls::ComboBox, camel::audio::AudioFormat>, 2> pcmBitsCombos();
    void settingsChanged();
    void updateBackendGroups();
    void refreshFromServer();
    void applySnapshot(const camel::server::Snapshot& snapshot);
    void submitCommand(camel::server::Command command);
    void showPage(winrt::hstring const& tag);
    // Selects the navigation item with this tag; its page opens through the selection handler.
    void selectPage(winrt::hstring const& tag);
    void viewAll(winrt::hstring const& list);
    void openStats(winrt::hstring const& period);
    void showSearchResults(winrt::hstring const& query);
    void showSuggestions(const camel::server::LibrarySearchResults& results, winrt::hstring const& text);
    // What a suggestion opens; the list holds them in display order next to their items.
    struct Suggestion {
        enum class Kind : uint8_t { Track, Album, Artist, All } kind;
        int64_t id = 0;
        std::string value; // track path or artist name
        winrt::hstring title;
    };
    std::vector<Suggestion> suggestions_;
    winrt::Windows::Foundation::Collections::IVector<winrt::Windows::Foundation::IInspectable> suggestionItems_{nullptr};
    camel::server::CancellationToken searchToken_;
    uint64_t searchGeneration_ = 0;
    int64_t frameLast_ = 0, frameWindow_ = 0;
    std::vector<double> frameIntervals_;
    void recordFrame();
    winrt::hstring shownTag_ = L"albums";
    void removeSelectedQueueEntries();
    // Lists the library folders once pending commands have run; the Server is queried off the UI thread.
    winrt::fire_and_forget refreshRoots();
    void showRoots(Microsoft::UI::Xaml::Controls::ListView const& list);
    winrt::fire_and_forget showDataLocation();
    void applySettingsNow();
    // First-run guide (docs/Plan.md release section): folders, then output; finishing or skipping records it.
    winrt::fire_and_forget showFirstRunGuide();
    winrt::fire_and_forget showFirstRunGuideIfDue();
    Microsoft::UI::Xaml::Controls::ListView guideFolders_{nullptr};
    // Shows the running library scan in Settings; reads counters, not the database.
    void showScanProgress();

    camel::server::CancellationToken artworkToken_;
    std::string artworkPath_;
    camel::server::SettingsDto settings_;
    std::shared_ptr<camel::server::IServer> server_;
    camel::server::SnapshotCursor cursor_;
    std::vector<camel::server::DeviceInfo> wasapiDevices_, asioDrivers_, ksPins_;
    std::vector<camel::server::LibraryRootInfo> roots_;
    Microsoft::UI::Dispatching::DispatcherQueueTimer uiTimer_{nullptr};
    Microsoft::UI::Dispatching::DispatcherQueueTimer seekTimer_{nullptr};
    bool populating_ = false;     // ignore control events while filling controls
    bool sliderUpdating_ = false; // ignore slider events caused by position updates
    bool queueOpen_ = false;
    bool navCompacted_ = false;
    double pendingSeek_ = 0;
    uint64_t shownRevision_ = 0;
    uint64_t shownCommandId_ = 0;
    uint64_t shownQueueRevision_ = UINT64_MAX;
    uint64_t shownQueueMarks_ = UINT64_MAX;
    // The playing track's library file and whether the playback bar shows it as a favorite.
    int64_t playbackFile_ = 0;
    bool playbackFavorite_ = false;
    uint64_t shownQueueEntryId_ = UINT64_MAX;
    bool queueShownMeasured_ = false;
    winrt::com_ptr<ui::QueueCollection> queueItems_ = winrt::make_self<ui::QueueCollection>();
    void updateQueuePanel();
    // Notification-area residency (docs/Plan.md §8.4).
    void hideToTray();
    void showFromTray();
    std::unique_ptr<ui::TrayIcon> tray_;
    // System integration (docs/Plan.md §8.4): media keys and the Windows media flyout, and global hotkeys.
    std::unique_ptr<ui::MediaControls> media_;
    std::unique_ptr<ui::GlobalHotkeys> hotkeys_;
    std::array<bool, ui::GlobalHotkeys::kActions> hotkeysTaken_{};
    std::optional<ui::GlobalHotkeys::Action> capturing_;
    void applyHotkeys();
    void showHotkeys();
    void submitKind(camel::server::CommandKind kind);
    bool exiting_ = false;
    bool inTray_ = false;
    // Switching backend keeps the same hardware; false when the new backend lists no matching device.
    bool followDevice(camel::Backend previous);
    void showStatusNotices(const camel::server::PlaybackSnapshot& s);

    // Playlists (docs/Plan.md §7.9, §8.3).
    void extendPlaylistMenu(Microsoft::UI::Xaml::Controls::MenuFlyout const& menu,
                            const std::function<ui::SourceFetch()>& selection);
    // Creates a playlist from, or adds to `playlist`, what `fetch` collects; over 1,000 tracks are confirmed first.
    void runPlaylistAction(ui::SourceFetch fetch, std::optional<int64_t> playlist, std::string name);
    winrt::fire_and_forget applyPlaylistAction(std::vector<camel::server::PlaylistSource> sources,
                                               camel::server::SelectionPreview preview, std::optional<int64_t> playlist,
                                               std::string name);
    winrt::fire_and_forget askNewPlaylist(ui::SourceFetch fetch);
    void showPlaylistChange(const camel::server::PlaylistChange& change, bool created);
    void showPlaylistNotice(winrt::hstring const& title, winrt::hstring const& message, bool error,
                            std::optional<camel::server::PlaylistUndo> undo = std::nullopt, int64_t playlist = 0);
    void undoPlaylistNotice();
    // Lists the playlists under Playlists in the navigation pane; `then` runs once they are shown.
    winrt::fire_and_forget refreshPlaylists(std::function<void()> then = {});
    void showPlaylistsInNavigation();
    // Puts a new playlist's name in an edit box beside its navigation item, without leaving the current page.
    void renameInNavigation(int64_t playlist);
    std::vector<camel::server::PlaylistSummary> playlists_;
    std::optional<camel::server::PlaylistUndo> noticeUndo_;
    Microsoft::UI::Dispatching::DispatcherQueueTimer noticeTimer_{nullptr};
    bool navUpdating_ = false;
    Microsoft::UI::Xaml::Controls::MenuFlyout queueMenu_{nullptr};
    Windows::Foundation::IInspectable queueMenuItem_{nullptr};
    std::wstring shownNotices_;
    // Collapses the navigation pane to icons while the queue panel would leave too little content width.
    void fitNavigationPane();
    winrt::hstring shownScan_;
    uint64_t shownDevicesRevision_ = 0;
};

} // namespace winrt::CamelPlayer::implementation

namespace winrt::CamelPlayer::factory_implementation {

struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};

} // namespace winrt::CamelPlayer::factory_implementation
