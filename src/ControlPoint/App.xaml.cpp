// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pch.h"

#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "Common/util/Log.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::CamelPlayer::implementation {

App::App(std::shared_ptr<camel::server::IServer> server) : server_(std::move(server)) {
    if (!server_) server_ = camel::server::createLocalServer();
    UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e) {
        camel::log::error("unhandled XAML exception: %s", winrt::to_string(e.Message()).c_str());
#if defined _DEBUG
        if (IsDebuggerPresent()) __debugbreak();
#endif
    });
}

void App::OnLaunched(LaunchActivatedEventArgs const&) {
    window_ = make<MainWindow>(std::move(server_));
    window_.Activate();
    hangWatch_.emplace([queue = window_.DispatcherQueue()](
                           std::function<void()> ping) { return queue.TryEnqueue([ping = std::move(ping)] { ping(); }); },
                       std::chrono::seconds(10));
}

} // namespace winrt::CamelPlayer::implementation
