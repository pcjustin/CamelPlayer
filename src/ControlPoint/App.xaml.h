#pragma once

#include "App.xaml.g.h"
#include "Server/Api/ServerApi.h"

namespace winrt::CamelPlayer::implementation {

struct App : AppT<App> {
    explicit App(std::shared_ptr<camel::server::IServer> server = {});
    void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

private:
    std::shared_ptr<camel::server::IServer> server_;
    Microsoft::UI::Xaml::Window window_{nullptr};
};

} // namespace winrt::CamelPlayer::implementation
