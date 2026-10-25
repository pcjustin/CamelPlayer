// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Entry point: logging, crash dumps and the UI language override must be in place before XAML starts.
#include "pch.h"

#include "App.xaml.h"
#include "TrayIcon.h"
#include "Server/Api/ServerApi.h"
#include "Common/util/CrashDump.h"
#include "Common/util/Log.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const auto logs = camel::dataDirectory() / L"logs";
    camel::installCrashDumps(logs);
    camel::log::init(logs, L"CamelPlayer");
    camel::log::info("CamelPlayer starting");

    // One instance per Windows user (docs/Plan.md §8.4), decided before the Server opens the library: a second launch
    // shows the running instance's window, even from the notification area, and exits.
    const HANDLE instance = CreateMutexW(nullptr, FALSE, L"Local\\CamelPlayer.Instance");
    if (instance && GetLastError() == ERROR_ALREADY_EXISTS) {
        for (int attempt = 0; attempt < 50; ++attempt) { // the first instance may still be starting
            if (const HWND tray = FindWindowW(ui::TrayIcon::kWindowClass, nullptr)) {
                DWORD process = 0;
                GetWindowThreadProcessId(tray, &process);
                AllowSetForegroundWindow(process);
                PostMessageW(tray, ui::TrayIcon::kShowMessage, 0, 0);
                break;
            }
            Sleep(100);
        }
        camel::log::info("CamelPlayer already running; showed its window");
        CloseHandle(instance);
        return 0;
    }

    winrt::init_apartment(winrt::apartment_type::single_threaded);
    auto server = camel::server::createLocalServer();
    try {
        const auto s = server->snapshot().settings;
        if (!s.language.empty()) // "" = follow Windows (docs/Plan.md §8.5)
            winrt::Microsoft::Windows::Globalization::ApplicationLanguages::PrimaryLanguageOverride(
                winrt::to_hstring(s.language));
    } catch (const winrt::hresult_error& e) {
        camel::log::warn("language override failed: 0x%08X", static_cast<unsigned>(e.code()));
    }
    ::winrt::Microsoft::UI::Xaml::Application::Start(
        [server](auto&&) { ::winrt::make<::winrt::CamelPlayer::implementation::App>(server); });
    server->waitIdle();
    return 0;
}
