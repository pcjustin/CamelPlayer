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
#include <algorithm>
#include <future>
#include <iterator>

#include "TrayIcon.h"
#include "UiText.h"
#include "Server/Api/ServerApi.h"
#include "Server/Remote/Discovery.h"
#include "Server/Remote/RemoteAccess.h"
#include "Server/Remote/RemoteServer.h"
#include "Server/Remote/ServerTokens.h"
#include "Server/Support/Updates.h"
#include "Common/util/CrashDump.h"
#include "Common/util/FolderWalk.h"
#include "Common/util/Log.h"
#include "Common/util/Measure.h"
#include "Common/util/StartWithWindows.h"
#include "Common/util/Text.h"
#include "Common/Version.h"

namespace {

// Settings › Reset everything leaves this marker and restarts; the data, logs included, goes before logging starts
// and the Server opens it, so the next start is a fresh installation. Returns what could not be removed.
std::vector<std::wstring> applyPendingReset(const std::filesystem::path& data) {
    std::error_code error;
    if (!std::filesystem::exists(data / L"reset-pending", error)) return {};
    return camel::removeEntries(data);
}

// Windows names the media flyout, notifications and the taskbar group after the AUMID; an app without a Start menu
// shortcut supplies the name and icon under the current user (docs/Plan.md §8.4). Every copy shares the AUMID, so the
// icon is changed only when the one it names is gone, as after a copy was moved or cleaned.
void registerAppUserModelId(const wchar_t* id) {
    wchar_t executable[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, executable, MAX_PATH)) return;
    const std::wstring key = std::wstring(L"Software\\Classes\\AppUserModelId\\") + id;
    const std::wstring name = L"CamelPlayer";
    RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), L"DisplayName", REG_SZ, name.c_str(),
                    DWORD((name.size() + 1) * sizeof(wchar_t)));
    std::wstring stored(32768, L'\0');
    auto bytes = DWORD(stored.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), L"IconUri", RRF_RT_REG_SZ, nullptr, stored.data(), &bytes) ==
            ERROR_SUCCESS &&
        bytes >= sizeof(wchar_t)) {
        stored.resize(bytes / sizeof(wchar_t) - 1);
        if (std::error_code error; std::filesystem::exists(stored, error)) return;
    }
    const auto icon = (std::filesystem::path(executable).parent_path() / L"assets" / L"CamelPlayer.ico").wstring();
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

// One instance per data folder: the user's own, or each folder CAMELPLAYER_DATA names, so two copies with folders of
// their own run side by side (docs/Plan.md §17.8).
std::wstring instanceName() {
    std::wstring name = L"Local\\CamelPlayer.Instance";
    if (GetEnvironmentVariableW(L"CAMELPLAYER_DATA", nullptr, 0) == 0) return name;
    uint64_t hash = 1469598103934665603ULL;
    for (const wchar_t c : camel::dataDirectory().wstring()) {
        hash ^= uint64_t(towlower(c));
        hash *= 1099511628211ULL;
    }
    wchar_t suffix[24];
    swprintf(suffix, std::size(suffix), L".%016llx", static_cast<unsigned long long>(hash));
    return name + suffix;
}

enum class Unreached : uint8_t { Retry, PlayHere, Control, Close };

// Why a Control point's Server could not be used at the start.
enum class Failed : uint8_t { Unreached, NotPaired, ServerTooOld, ThisTooOld, NeedsCode };

constexpr int kFirstFound = 1000; // the buttons of the other CamelPlayers found

struct Answer {
    Unreached what = Unreached::Close;
    size_t found = 0; // with Control, which of the others
};

// A Server that cannot be reached, no longer knows this computer or shares no API version with it, at the start of a
// Control point; only one that cannot be reached is worth trying again. The other CamelPlayers found may be controlled
// instead (docs/Plan.md §17.8).
Answer askUnreached(Failed why, const std::string& server, const std::string& version,
                    const std::vector<camel::server::FoundServer>& others) {
    const wchar_t* titles[] = {L"Control_UnreachedTitle", L"Control_NotPairedTitle", L"Control_ServerTooOldTitle",
                               L"Control_ThisTooOldTitle", L"Control_NeedsCodeTitle"};
    const wchar_t* texts[] = {L"Control_UnreachedText", L"Control_NotPairedText", L"Control_ServerTooOldText",
                              L"Control_ThisTooOldText", L"Control_NeedsCodeText"};
    const auto title = ui::format(titles[size_t(why)], {camel::toWide(server)});
    const auto text = ui::format(texts[size_t(why)], {camel::toWide(version), L"" CAMEL_VERSION_STRING});
    const bool notPaired = why != Failed::Unreached;
    const auto retry = ui::tr(L"Error_LibraryRetry");
    const auto play = ui::tr(L"Control_PlayHere");
    const auto close = ui::tr(L"Error_LibraryClose");
    std::vector<std::wstring> control;
    control.reserve(others.size());
    for (const auto& other : others)
        control.emplace_back(ui::format(L"Control_ControlFound", {camel::toWide(other.announced.name)}));
    std::vector<TASKDIALOG_BUTTON> buttons;
    if (!notPaired) buttons.push_back({IDRETRY, retry.c_str()});
    buttons.push_back({IDYES, play.c_str()});
    for (size_t i = 0; i < control.size(); ++i) buttons.push_back({kFirstFound + int(i), control[i].c_str()});
    buttons.push_back({IDCLOSE, close.c_str()});
    TASKDIALOGCONFIG config{.cbSize = sizeof(TASKDIALOGCONFIG)};
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    config.pszWindowTitle = L"CamelPlayer";
    config.pszMainIcon = TD_WARNING_ICON;
    config.pszMainInstruction = title.c_str();
    config.pszContent = text.c_str();
    config.cButtons = UINT(buttons.size());
    config.pButtons = buttons.data();
    config.nDefaultButton = notPaired ? IDYES : IDRETRY;
    int chosen = IDCLOSE;
    if (FAILED(TaskDialogIndirect(&config, &chosen, nullptr, nullptr))) return {};
    if (chosen >= kFirstFound && size_t(chosen - kFirstFound) < others.size())
        return {Unreached::Control, size_t(chosen - kFirstFound)};
    return {chosen == IDRETRY ? Unreached::Retry : chosen == IDYES ? Unreached::PlayHere : Unreached::Close};
}

// The CamelPlayers on the LAN other than the one this Control point controls, by name.
std::vector<camel::server::FoundServer> otherServers(const std::string& controlled) {
    std::vector<camel::server::FoundServer> found;
    try {
        found = camel::server::findServers(std::chrono::milliseconds(1500));
    } catch (const std::exception& e) {
        camel::log::warn("control: finding other CamelPlayers: %s", e.what());
    }
    std::erase_if(found, [&](const camel::server::FoundServer& s) { return s.announced.id == controlled; });
    std::ranges::sort(found, {}, [](const camel::server::FoundServer& s) { return s.announced.name; });
    return found;
}

// The CamelPlayer this Control point controls, looked for by its ID when it no longer answers at its address.
std::shared_ptr<camel::server::IServer> reachControlled(camel::Settings& own, const std::string& token) {
    camel::server::RemoteServer::Options options;
    options.host = own.controlled.host;
    options.port = own.controlled.port;
    options.cache = camel::dataDirectory() / L"remote-cache";
    options.token = token;
    options.ownSettings = camel::settingsPath();
    try {
        return std::make_shared<camel::server::RemoteServer>(options);
    } catch (const camel::server::NotPaired&) {
        throw;
    } catch (const camel::server::VersionMismatch&) {
        throw;
    } catch (const std::exception& e) {
        camel::log::warn("control: %s:%u not reached: %s", own.controlled.host.c_str(), unsigned(own.controlled.port), e.what());
        for (const auto& found : camel::server::findServers(std::chrono::milliseconds(1500))) {
            if (found.announced.id != own.controlled.id) continue;
            options.host = found.address;
            options.port = found.announced.port;
            auto server = std::make_shared<camel::server::RemoteServer>(options);
            own.controlled.host = options.host;
            own.controlled.port = options.port;
            camel::saveSettings(own);
            return server;
        }
        throw;
    }
}

// Nothing when the user chose to play here instead, `closing` when they chose to close. Another CamelPlayer chosen is
// paired with when it needs no code, a computer this one paired with or one still ownerless, and controlled instead.
std::shared_ptr<camel::server::IServer> openControlled(camel::Settings& own, bool& closing) {
    camel::server::ServerTokens tokens(camel::dataDirectory() / L"server-tokens");
    Failed why = Failed::Unreached;
    std::string name, version;
    bool reach = true;
    for (;;) {
        if (reach) {
            why = Failed::Unreached;
            name = own.controlled.name;
            version.clear();
            try {
                return reachControlled(own, tokens.token(own.controlled.id).value_or(""));
            } catch (const camel::server::NotPaired&) {
                camel::log::warn("control: no longer paired with %s", own.controlled.name.c_str());
                why = Failed::NotPaired;
            } catch (const camel::server::VersionMismatch& e) {
                camel::log::warn("control: %s (it runs %s)", e.what(), e.serverVersion.c_str());
                why = e.serverOlder ? Failed::ServerTooOld : Failed::ThisTooOld;
                version = e.serverVersion;
            } catch (const std::exception& e) {
                camel::log::warn("control: %s", e.what());
            }
        }
        reach = true;
        const auto others = otherServers(own.controlled.id);
        const auto answer = askUnreached(why, name, version, others);
        switch (answer.what) {
        case Unreached::Retry:
            continue;
        case Unreached::PlayHere:
            own.role = camel::Role::Play;
            camel::saveSettings(own);
            return nullptr;
        case Unreached::Control: {
            const auto& found = others[answer.found];
            const camel::ControlledServer target{found.announced.id, found.announced.name, found.address, found.announced.port};
            if (!tokens.token(target.id)) {
                try {
                    const auto paired = camel::server::RemoteServer::pair(
                        target.host, target.port, own.ownName.empty() ? camel::server::computerName() : own.ownName, "",
                        camel::server::RemoteServer::introduction(camel::dataDirectory(), target.id));
                    tokens.set(target.id, paired.token);
                } catch (const camel::server::PairingRefused& e) {
                    camel::log::warn("control: %s asks for a code (%s)", target.name.c_str(), e.code.c_str());
                    why = Failed::NeedsCode;
                    name = target.name;
                    reach = false;
                    continue;
                } catch (const std::exception& e) {
                    camel::log::warn("control: pairing with %s: %s", target.name.c_str(), e.what());
                    why = Failed::Unreached;
                    name = target.name;
                    reach = false;
                    continue;
                }
            }
            own.controlled = target;
            camel::saveSettings(own);
            continue;
        }
        case Unreached::Close:
            closing = true;
            return nullptr;
        }
    }
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
    const auto instanceMutex = instanceName();
    HANDLE instance = CreateMutexW(nullptr, FALSE, instanceMutex.c_str());
    // After a full reset or a change of role the previous instance is still exiting; wait for it rather than activating it.
    if (instance && GetLastError() == ERROR_ALREADY_EXISTS &&
        (wcsstr(commandLine, L"--after-reset") || wcsstr(commandLine, L"--restart"))) {
        for (int attempt = 0; attempt < 150 && instance && GetLastError() == ERROR_ALREADY_EXISTS; ++attempt) {
            CloseHandle(instance);
            Sleep(200);
            instance = CreateMutexW(nullptr, FALSE, instanceMutex.c_str());
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
    std::shared_ptr<camel::server::RemoteAccess> access;
    while (!server) {
        try {
            // A Control point runs no library, output or background work of its own (docs/Plan.md §17.8).
            if (auto own = camel::readSettingsForTool(camel::settingsPath());
                own.role == camel::Role::Control && !own.controlled.id.empty()) {
                applySavedLanguage();
                bool closing = false;
                server = openControlled(own, closing);
                if (closing) return 0;
                if (server) break;
            }
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
    if (const auto local = std::dynamic_pointer_cast<camel::server::LocalServer>(server)) {
        camel::server::RemoteAccess::Options options;
        options.data = camel::dataDirectory();
        options.name = camel::server::computerName();
        options.appVersion = CAMEL_VERSION_STRING;
        if (wchar_t program[MAX_PATH]; GetModuleFileNameW(nullptr, program, MAX_PATH))
            options.programDirectory = std::filesystem::path(program).parent_path();
        access = std::make_shared<camel::server::RemoteAccess>(*local, options);
    }
    try {
        const auto s = server->snapshot().settings;
        if (!s.language.empty()) // "" = follow Windows (docs/Plan.md §8.5)
            winrt::Microsoft::Windows::Globalization::ApplicationLanguages::PrimaryLanguageOverride(
                winrt::to_hstring(s.language));
    } catch (const winrt::hresult_error& e) {
        camel::log::warn("language override failed: 0x%08X", static_cast<unsigned>(e.code()));
    }
    ::winrt::Microsoft::UI::Xaml::Application::Start([server, background, access](auto&&) {
        ::winrt::make<::winrt::CamelPlayer::implementation::App>(server, background, access);
    });
    if (access) access->stop();
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
