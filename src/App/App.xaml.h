#pragma once

#include "App.xaml.g.h"

namespace winrt::CamelPlayer::implementation {

struct App : AppT<App> {
    App();
    void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

private:
    Microsoft::UI::Xaml::Window window_{nullptr};
};

} // namespace winrt::CamelPlayer::implementation
