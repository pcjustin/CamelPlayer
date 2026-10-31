// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "App.xaml.g.h"
#include "Server/Api/ServerApi.h"
#include "Common/util/CrashDump.h"

namespace winrt::CamelPlayer::implementation {

struct App : AppT<App> {
    explicit App(std::shared_ptr<camel::server::IServer> server = {});
    void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

private:
    std::shared_ptr<camel::server::IServer> server_;
    Microsoft::UI::Xaml::Window window_{nullptr};
    std::optional<camel::HangWatch> hangWatch_;
};

} // namespace winrt::CamelPlayer::implementation
