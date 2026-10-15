#pragma once

#include "MainWindow.g.h"
#include "audio/engine/PlaybackEngine.h"
#include "settings/Settings.h"

namespace winrt::CamelPlayer::implementation {

struct MainWindow : MainWindowT<MainWindow> {
    MainWindow() = default;
    // C++/WinRT calls this after construction; named XAML elements exist from here on.
    void InitializeComponent();

    // XAML event handlers
    winrt::fire_and_forget OnOpenClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnPlayPauseClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnStopClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnSliderValueChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const&);
    void OnNavSelectionChanged(Microsoft::UI::Xaml::Controls::NavigationView const&, Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const&);
    void OnSettingChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void OnNumberChanged(Microsoft::UI::Xaml::Controls::NumberBox const&, Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const&);
    void OnLanguageChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    winrt::fire_and_forget OnAsioPanelClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnRefreshDevicesClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

private:
    void populateSettings();     // fills every control from settings_
    void refreshDeviceLists();   // device combos only
    void readSettingsFromUi();   // controls -> settings_
    void settingsChanged();      // save + apply to the engine
    void updateBackendGroups();
    void refreshFromEngine();
    void showPage(winrt::hstring const& tag);

    camel::Settings settings_;
    std::unique_ptr<camel::audio::PlaybackEngine> engine_;
    std::vector<camel::audio::DeviceEntry> wasapiDevices_, asioDrivers_, ksPins_;
    Microsoft::UI::Dispatching::DispatcherQueueTimer uiTimer_{nullptr};
    Microsoft::UI::Dispatching::DispatcherQueueTimer seekTimer_{nullptr};
    bool populating_ = false;      // ignore control events while filling controls
    bool sliderUpdating_ = false;  // ignore slider events caused by position updates
    double pendingSeek_ = 0;
    uint64_t shownRevision_ = 0;
};

}  // namespace winrt::CamelPlayer::implementation

namespace winrt::CamelPlayer::factory_implementation {

struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};

}  // namespace winrt::CamelPlayer::factory_implementation
