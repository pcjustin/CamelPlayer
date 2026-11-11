// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Entry point: logging, crash dumps and the UI language override must be in place before XAML starts.
#include "pch.h"

#include "App.xaml.h"

#include <CommCtrl.h>
#include <shobjidl.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>

#include "TrayIcon.h"
#include "UiText.h"
#include "Server/Api/ServerApi.h"
#include "Server/Support/Updates.h"
#include "Common/util/CrashDump.h"
#include "Common/util/FolderWalk.h"
#include "Common/util/Log.h"
#include "Common/util/Measure.h"
#include "Common/util/StartWithWindows.h"
#include "Common/util/Text.h"

namespace {

// Settings › Reset everything leaves this marker and restarts; the data, logs included, goes before logging starts
// and the Server opens it, so the next start is a fresh installation. Returns what could not be removed.
std::vector<std::wstring> applyPendingReset(const std::filesystem::path& data) {
    std::error_code error;
    if (!std::filesystem::exists(data / L"reset-pending", error)) return {};
    return camel::removeEntries(data);
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

// The interface language saved in settings.json, applied to the text shown before the Server opens; the file is only
// read, never repaired or migrated.
void applySavedLanguage() {
    std::ifstream in(camel::settingsPath(), std::ios::binary);
    if (!in) return;
    try {
        const auto language = camel::fromJson(std::string(std::istreambuf_iterator<char>(in), {})).language;
        if (language.empty()) return;
        winrt::Microsoft::Windows::Globalization::ApplicationLanguages::PrimaryLanguageOverride(winrt::to_hstring(language));
        ui::reloadText();
    } catch (const std::exception& e) {
        camel::log::warn("saved language unreadable: %s", e.what());
    } catch (const winrt::hresult_error& e) {
        camel::log::warn("language override failed: 0x%08X", static_cast<unsigned>(e.code()));
    }
}

// Asks whether to try opening the library or the settings again, with buttons in the interface language rather than
// Windows'.
bool askToRetry(const wchar_t* titleKey, const wchar_t* textKey, const std::wstring& details) {
    const auto title = ui::tr(titleKey);
    const auto text = ui::format(textKey, {details});
    const auto retry = ui::tr(L"Error_LibraryRetry");
    const auto close = ui::tr(L"Error_LibraryClose");
    const TASKDIALOG_BUTTON buttons[] = {{IDRETRY, retry.c_str()}, {IDCLOSE, close.c_str()}};
    TASKDIALOGCONFIG config{.cbSize = sizeof(TASKDIALOGCONFIG)};
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    config.pszWindowTitle = L"CamelPlayer";
    config.pszMainIcon = TD_WARNING_ICON;
    config.pszMainInstruction = title.c_str();
    config.pszContent = text.c_str();
    config.cButtons = UINT(std::size(buttons));
    config.pButtons = buttons;
    config.nDefaultButton = IDRETRY;
    int chosen = 0;
    return SUCCEEDED(TaskDialogIndirect(&config, &chosen, nullptr, nullptr)) && chosen == IDRETRY;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR commandLine, int) {
    // Data lives only in %LOCALAPPDATA%\CamelPlayer; an unzipped release must never write next to the program.
    if (camel::dataDirectory().empty()) {
        MessageBoxW(nullptr, ui::tr(L"Error_NoDataFolder").c_str(), L"CamelPlayer", MB_ICONERROR | MB_OK);
        return 1;
    }
    // Started by Windows at sign-in (Settings › Interface): the window stays in the notification area.
    const bool background = camel::startup::isBackgroundStart(commandLine);
    // One instance per Windows user (docs/Plan.md §8.4), decided before the Server opens the library: a second launch
    // shows the running instance's window, even from the notification area, and exits; a background start just exits.
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
        for (int attempt = 0; attempt < 50 && camel::startup::launchFor(background, true) == camel::startup::Launch::ShowRunning;
             ++attempt) { // the first instance may still be starting
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
    camel::server::update::signalStarting(camel::dataDirectory()); // before the Server opens, and may migrate, the data

    SetCurrentProcessExplicitAppUserModelID(L"CamelPlayer.CamelPlayer");
    registerAppUserModelId(L"CamelPlayer.CamelPlayer");
    // An unzipped copy moved to another folder keeps starting with Windows.
    if (wchar_t program[MAX_PATH]; GetModuleFileNameW(nullptr, program, MAX_PATH)) camel::startup::follow(program);
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    if (camel::measure::measuring())
        camel::log::info("measure: opening the server %.0f ms after process start", camel::measure::sinceProcessStart());
    // Data that cannot be opened is left as it is, and the user learns why instead of the program vanishing.
    std::shared_ptr<camel::server::IServer> server;
    while (!server) {
        try {
            server = camel::server::createLocalServer();
        } catch (const camel::server::LibraryUnavailableError& e) {
            // Often a moment's hold by another program: the user can let it go and try again without restarting.
            camel::log::error("cannot open the library: %s", e.what());
            applySavedLanguage();
            if (!askToRetry(L"Error_LibraryUnavailableTitle", L"Error_LibraryUnavailable", camel::toWide(e.what()))) return 1;
        } catch (const camel::SettingsUnavailableError& e) {
            camel::log::error("cannot open the settings: %s", e.what());
            applySavedLanguage();
            if (!askToRetry(L"Error_SettingsUnavailableTitle", L"Error_SettingsUnavailable", camel::toWide(e.what()))) return 1;
        } catch (const std::exception& e) {
            camel::log::error("cannot open the data: %s", e.what());
            const bool newer =
                dynamic_cast<const camel::NewerSettingsError*>(&e) || dynamic_cast<const camel::server::NewerLibraryError*>(&e);
            const auto text =
                newer ? ui::tr(L"Error_NewerData") : ui::format(L"Error_DataFailed", {camel::toWide(e.what()), logs.wstring()});
            MessageBoxW(nullptr, text.c_str(), L"CamelPlayer", MB_ICONERROR | MB_OK);
            return 1;
        }
    }
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
        [server, background](auto&&) { ::winrt::make<::winrt::CamelPlayer::implementation::App>(server, background); });
    // Parts of the UI, such as artwork loads waiting for the stopped UI thread, may still hold the Server, so it is shut
    // down explicitly: the queue is saved and the library closed cleanly. The next start waits for this process, so
    // background work a busy machine leaves no CPU for is abandoned; the library recovers at that start.
    auto stopping = std::async(std::launch::async, [&server] { server->shutdown(); });
    if (stopping.wait_for(std::chrono::seconds(5)) == std::future_status::timeout) {
        camel::log::warn("CamelPlayer stopped before its background work");
        TerminateProcess(GetCurrentProcess(), 0);
    }
    stopping.get();
    camel::log::info("CamelPlayer stopped");
    return 0;
}
