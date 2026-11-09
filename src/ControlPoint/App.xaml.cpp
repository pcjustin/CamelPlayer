// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pch.h"

#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "TrackColumns.h"
#include "Common/util/Log.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::CamelPlayer::implementation {

App::App(std::shared_ptr<camel::server::IServer> server, bool startHidden)
    : server_(std::move(server)), startHidden_(startHidden) {
    if (!server_) server_ = camel::server::createLocalServer();
    // Colors set from code come from the theme dictionaries, of which only the application's own is loaded at the start.
    const auto theme = server_->snapshot().settings.theme;
    if (theme != camel::Theme::System)
        RequestedTheme(theme == camel::Theme::Light ? ApplicationTheme::Light : ApplicationTheme::Dark);
    UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e) {
        camel::log::error("unhandled XAML exception: %s", winrt::to_string(e.Message()).c_str());
#if defined _DEBUG
        if (IsDebuggerPresent()) __debugbreak();
#endif
    });
}

void App::OnLaunched(LaunchActivatedEventArgs const&) {
    // The column widths every track list shares (docs/Plan.md §8.2), for the window's lists to bind to.
    Resources().Insert(box_value(L"TrackColumns"), make<TrackColumnWidths>());
    openWindow(std::move(server_));
    if (startHidden_)
        get_self<MainWindow>(window_.as<CamelPlayer::MainWindow>())->startInTray();
    else
        window_.Activate();
    hangWatch_.emplace([queue = window_.DispatcherQueue()](
                           std::function<void()> ping) { return queue.TryEnqueue([ping = std::move(ping)] { ping(); }); },
                       std::chrono::seconds(10));
}

void App::openWindow(std::shared_ptr<camel::server::IServer> server) {
    window_ = make<MainWindow>(std::move(server));
    get_self<MainWindow>(window_.as<CamelPlayer::MainWindow>())->onReplace([this](auto next) { replaceWindow(std::move(next)); });
}

void App::replaceWindow(std::shared_ptr<camel::server::IServer> server) {
    const auto old = window_;
    openWindow(std::move(server));
    get_self<MainWindow>(window_.as<CamelPlayer::MainWindow>())
        ->takeOverFrom(*get_self<MainWindow>(old.as<CamelPlayer::MainWindow>()));
    window_.Activate();
    old.Close();
}

} // namespace winrt::CamelPlayer::implementation
