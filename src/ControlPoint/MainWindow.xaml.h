// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "MainWindow.g.h"
#include "LibraryView.xaml.h"
#include "Server/Api/ServerApi.h"

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
    void OnFormatClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnQueueToggleClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnQueueClearClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget OnQueueSaveClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnQueueDoubleTapped(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const&);
    void OnQueueRemoveEntryClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnQueueKeyDown(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const&);
    void OnQueueDragItemsCompleted(Microsoft::UI::Xaml::Controls::ListViewBase const&,
                                   Microsoft::UI::Xaml::Controls::DragItemsCompletedEventArgs const&);
    void OnCoverClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnAlbumLinkClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnArtistLinkClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnSliderValueChanged(Windows::Foundation::IInspectable const&,
                              Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const&);
    void OnNavSelectionChanged(Microsoft::UI::Xaml::Controls::NavigationView const&,
                               Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const&);
    void OnSettingChanged(Windows::Foundation::IInspectable const&,
                          Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void OnNumberChanged(Microsoft::UI::Xaml::Controls::NumberBox const&,
                         Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const&);
    void OnLanguageChanged(Windows::Foundation::IInspectable const&,
                           Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    winrt::fire_and_forget OnAsioPanelClick(Windows::Foundation::IInspectable const&,
                                            Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnRefreshDevicesClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    winrt::fire_and_forget OnAddFolderClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
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
    void removeSelectedQueueEntries();
    // Lists the library folders once pending commands have run; the Server is queried off the UI thread.
    winrt::fire_and_forget refreshRoots();
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
    uint64_t shownQueueEntryId_ = UINT64_MAX;
    void updateQueuePanel();
    // Switching backend keeps the same hardware; false when the new backend lists no matching device.
    bool followDevice(camel::Backend previous);
    void showStatusNotices(const camel::server::PlaybackSnapshot& s);
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
