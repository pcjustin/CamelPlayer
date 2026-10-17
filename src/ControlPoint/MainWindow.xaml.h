#pragma once

#include "MainWindow.g.h"
#include "Server/Api/ServerApi.h"

namespace winrt::CamelPlayer::implementation {

struct MainWindow : MainWindowT<MainWindow> {
    explicit MainWindow(std::shared_ptr<camel::server::IServer> server = {}) : server_(std::move(server)) {}
    // C++/WinRT calls this after construction; named XAML elements exist from here on.
    void InitializeComponent();

    // XAML event handlers
    winrt::fire_and_forget OnOpenClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnPlayPauseClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnStopClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
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

private:
    void populateSettings(); // fills every control from settings_
    void populateDeviceLists(const camel::server::DeviceCatalog& devices);
    void readSettingsFromUi(); // controls -> settings_
    // One decoder selector per format that has integrated decoders.
    std::array<std::pair<Microsoft::UI::Xaml::Controls::ComboBox, camel::audio::AudioFormat>, 4> decoderCombos();
    void settingsChanged();
    void updateBackendGroups();
    void refreshFromServer();
    void applySnapshot(const camel::server::Snapshot& snapshot);
    void submitCommand(camel::server::Command command);
    void showPage(winrt::hstring const& tag);

    camel::server::SettingsDto settings_;
    std::shared_ptr<camel::server::IServer> server_;
    camel::server::SnapshotCursor cursor_;
    std::vector<camel::server::DeviceInfo> wasapiDevices_, asioDrivers_, ksPins_;
    Microsoft::UI::Dispatching::DispatcherQueueTimer uiTimer_{nullptr};
    Microsoft::UI::Dispatching::DispatcherQueueTimer seekTimer_{nullptr};
    bool populating_ = false;     // ignore control events while filling controls
    bool sliderUpdating_ = false; // ignore slider events caused by position updates
    double pendingSeek_ = 0;
    uint64_t shownRevision_ = 0;
    uint64_t shownCommandId_ = 0;
    uint64_t shownDevicesRevision_ = 0;
};

} // namespace winrt::CamelPlayer::implementation

namespace winrt::CamelPlayer::factory_implementation {

struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};

} // namespace winrt::CamelPlayer::factory_implementation
