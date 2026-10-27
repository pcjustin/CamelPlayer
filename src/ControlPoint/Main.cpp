// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Entry point: logging, crash dumps and the UI language override must be in place before XAML starts.
#include "pch.h"

#include "App.xaml.h"

#include <shobjidl.h>

#include <filesystem>

#include "TrayIcon.h"
#include "UiText.h"
#include "Server/Api/ServerApi.h"
#include "Common/util/CrashDump.h"
#include "Common/util/Log.h"
#include "Common/util/Measure.h"
#include "Common/util/Text.h"

namespace {

// Settings › Reset everything leaves this marker and restarts; the data, logs included, goes before logging starts
// and the Server opens it, so the next start is a fresh installation. Returns what could not be removed.
std::vector<std::wstring> applyPendingReset(const std::filesystem::path& data) {
    std::vector<std::wstring> kept;
    std::error_code error;
    if (!std::filesystem::exists(data / L"reset-pending", error)) return kept;
    for (const auto& entry : std::filesystem::directory_iterator(data, error)) {
        std::filesystem::remove_all(entry.path(), error);
        if (error) kept.push_back(entry.path().filename().wstring());
    }
    return kept;
}

// Windows names the media flyout, notifications and the taskbar group after the AUMID; an app without a Start menu
// shortcut supplies the name and icon under the current user (docs/Plan.md §8.4).
void registerAppUserModelId(const wchar_t* id) {
    wchar_t executable[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, executable, MAX_PATH)) return;
    const auto icon = (std::filesystem::path(executable).parent_path() / L"assets" / L"CamelPlayer.ico").wstring();
    const std::wstring key = std::wstring(L"Software\\Classes\\AppUserModelId\\") + id;
    const std::wstring name = L"CamelPlayer";
    RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), L"DisplayName", REG_SZ, name.c_str(),
                    DWORD((name.size() + 1) * sizeof(wchar_t)));
    RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), L"IconUri", REG_SZ, icon.c_str(), DWORD((icon.size() + 1) * sizeof(wchar_t)));
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR commandLine, int) {
    // Data lives only in %LOCALAPPDATA%\CamelPlayer; an unzipped release must never write next to the program.
    if (camel::dataDirectory().empty()) {
        MessageBoxW(nullptr, ui::tr(L"Error_NoDataFolder").c_str(), L"CamelPlayer", MB_ICONERROR | MB_OK);
        return 1;
    }
    // One instance per Windows user (docs/Plan.md §8.4), decided before the Server opens the library: a second launch
    // shows the running instance's window, even from the notification area, and exits.
    HANDLE instance = CreateMutexW(nullptr, FALSE, L"Local\\CamelPlayer.Instance");
    // After a full reset the previous instance is still exiting; wait for it rather than activating it.
    if (instance && GetLastError() == ERROR_ALREADY_EXISTS && wcsstr(commandLine, L"--after-reset")) {
        for (int attempt = 0; attempt < 150 && instance && GetLastError() == ERROR_ALREADY_EXISTS; ++attempt) {
            CloseHandle(instance);
            Sleep(200);
            instance = CreateMutexW(nullptr, FALSE, L"Local\\CamelPlayer.Instance");
        }
    }
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
        CloseHandle(instance);
        return 0;
    }

    const auto notRemoved = applyPendingReset(camel::dataDirectory());
    const auto logs = camel::dataDirectory() / L"logs";
    camel::installCrashDumps(camel::dataDirectory() / L"crashdumps");
    camel::log::init(logs, L"CamelPlayer");
    camel::log::info("CamelPlayer starting");
    for (const auto& name : notRemoved) camel::log::warn("reset: could not remove %s", camel::toUtf8(name).c_str());

    SetCurrentProcessExplicitAppUserModelID(L"CamelPlayer.CamelPlayer");
    registerAppUserModelId(L"CamelPlayer.CamelPlayer");
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    if (camel::measure::measuring())
        camel::log::info("measure: opening the server %.0f ms after process start", camel::measure::sinceProcessStart());
    auto server = camel::server::createLocalServer();
    if (camel::measure::measuring())
        camel::log::info("measure: server opened %.0f ms after process start", camel::measure::sinceProcessStart());
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
    // Parts of the UI, such as artwork loads waiting for the stopped UI thread, may still hold the Server, so it is shut
    // down explicitly: the queue is saved and the library closed cleanly.
    server->shutdown();
    camel::log::info("CamelPlayer stopped");
    return 0;
}
