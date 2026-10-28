// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// CamelUpdater.exe (docs/Plan.md §8.6): started by the player from %TEMP%\CamelPlayer-update with
//   --pid <player> --app <program folder> --files <new files> --data <data folder> --version <new version>
//   --from <running version>
// It waits for the player to exit, elevates only when the program folder is not writable, installs and restarts.
#include <windows.h>
#include <shlobj.h>
#include <exdisp.h>
#include <shellapi.h>
#include <shldisp.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "Updater/Install.h"
#include "Updater/UpdateFiles.h"

#undef ShellExecute // the shellapi.h macro would rename IShellDispatch2::ShellExecute

namespace {

namespace fs = std::filesystem;
namespace files = camel::updatefiles;
using Microsoft::WRL::ComPtr;

fs::path g_logFile;

void log(const std::wstring& text) {
    if (g_logFile.empty()) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[32];
    swprintf(stamp, std::size(stamp), L"%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    const std::wstring line = stamp + text + L"\n";
    std::string utf8(line.size() * 3, '\0');
    utf8.resize(
        size_t(WideCharToMultiByte(CP_UTF8, 0, line.data(), int(line.size()), utf8.data(), int(utf8.size()), nullptr, nullptr)));
    if (FILE* file = _wfsopen(g_logFile.c_str(), L"ab", _SH_DENYNO)) {
        fwrite(utf8.data(), 1, utf8.size(), file);
        fclose(file);
    }
}

void openLog(const fs::path& data) {
    std::error_code error;
    fs::create_directories(data / L"logs", error);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf(name, std::size(name), L"updater-%04u%02u%02u.log", t.wYear, t.wMonth, t.wDay);
    g_logFile = data / L"logs" / name;
}

bool writable(const fs::path& folder) {
    const HANDLE file = CreateFileW((folder / L"update-write-test").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    CloseHandle(file);
    return true;
}

bool elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool result =
        GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) && elevation.TokenIsElevated;
    CloseHandle(token);
    return result;
}

void startProcess(const fs::path& exe) {
    std::wstring command = L"\"" + exe.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, exe.parent_path().c_str(), &startup,
                        &process)) {
        log(L"could not start " + exe.wstring() + L": " + std::to_wstring(GetLastError()));
        return;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}

// An elevated updater starts the player through the desktop's Explorer, so it runs as the signed-in user again.
bool startThroughShell(const fs::path& exe) {
    ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows)))) return false;
    VARIANT empty{};
    long window = 0;
    ComPtr<IDispatch> desktop;
    if (windows->FindWindowSW(&empty, &empty, SWC_DESKTOP, &window, SWFO_NEEDDISPATCH, &desktop) != S_OK) return false;
    ComPtr<IServiceProvider> services;
    ComPtr<IShellBrowser> browser;
    ComPtr<IShellView> view;
    ComPtr<IDispatch> background;
    ComPtr<IShellFolderViewDual> folderView;
    ComPtr<IDispatch> application;
    ComPtr<IShellDispatch2> shell;
    if (FAILED(desktop.As(&services)) || FAILED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) ||
        FAILED(browser->QueryActiveShellView(&view)) ||
        FAILED(view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&background))) || FAILED(background.As(&folderView)) ||
        FAILED(folderView->get_Application(&application)) || FAILED(application.As(&shell)))
        return false;
    const BSTR file = SysAllocString(exe.c_str());
    VARIANT folder{};
    folder.vt = VT_BSTR;
    folder.bstrVal = SysAllocString(exe.parent_path().c_str());
    const HRESULT result = shell->ShellExecute(file, empty, folder, empty, empty);
    SysFreeString(file);
    VariantClear(&folder);
    return SUCCEEDED(result);
}

std::vector<HANDLE> players(const fs::path& exe) {
    std::vector<HANDLE> found;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return found;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, files::kPlayer) != 0) continue;
        const HANDLE process =
            OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, entry.th32ProcessID);
        if (!process) continue;
        wchar_t path[MAX_PATH * 4];
        auto size = DWORD(std::size(path));
        if (QueryFullProcessImageNameW(process, 0, path, &size) && _wcsicmp(path, exe.c_str()) == 0)
            found.push_back(process);
        else
            CloseHandle(process);
    }
    CloseHandle(snapshot);
    return found;
}

std::map<std::wstring, std::wstring> arguments(bool& elevatedRun) {
    std::map<std::wstring, std::wstring> values;
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count);
    for (int i = 1; i < count; ++i) {
        const std::wstring name = argv[i];
        if (name == L"--elevated")
            elevatedRun = true;
        else if (name.starts_with(L"--") && i + 1 < count)
            values[name.substr(2)] = argv[++i];
    }
    LocalFree(static_cast<HLOCAL>(static_cast<void*>(argv)));
    return values;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    bool elevatedRun = false;
    auto args = arguments(elevatedRun);
    if (args[L"app"].empty() || args[L"files"].empty() || args[L"data"].empty() || args[L"version"].empty()) return 2;
    wchar_t self[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, self, DWORD(std::size(self)));
    camel::updater::Options options{args[L"app"],     args[L"files"],
                                    args[L"data"],    fs::path(self).parent_path() / files::kDataBackup,
                                    args[L"version"], args[L"from"]};
    openLog(options.data);
    const auto player = options.app / files::kPlayer;

    if (!elevatedRun) {
        log(L"updating to " + options.version);
        if (const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, DWORD(_wtoi(args[L"pid"].c_str())))) {
            const DWORD waited = WaitForSingleObject(process, 30000);
            CloseHandle(process);
            if (waited == WAIT_TIMEOUT) {
                log(L"CamelPlayer did not exit within 30 s; update cancelled");
                camel::updater::writeResult(options.data, L"cancelled", options.version);
                return 1;
            }
        }
        if (!writable(options.app)) {
            log(L"the program folder is not writable; asking for administrator rights");
            std::wstring command = PathGetArgsW(GetCommandLineW());
            command += L" --elevated";
            SHELLEXECUTEINFOW run{};
            run.cbSize = sizeof(run);
            run.fMask = SEE_MASK_NOCLOSEPROCESS;
            run.lpVerb = L"runas";
            run.lpFile = self;
            run.lpParameters = command.c_str();
            run.nShow = SW_HIDE;
            if (!ShellExecuteExW(&run) || !run.hProcess) {
                log(L"administrator rights were not granted; update cancelled");
                camel::updater::writeResult(options.data, L"cancelled", options.version);
                startProcess(player);
                return 1;
            }
            WaitForSingleObject(run.hProcess, INFINITE);
            DWORD code = 1;
            GetExitCodeProcess(run.hProcess, &code);
            CloseHandle(run.hProcess);
            return int(code);
        }
    }

    const bool asAdministrator = elevated();
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    camel::updater::Host host;
    host.log = [](const std::wstring& text) { log(text); };
    host.startPlayer = [asAdministrator](const fs::path& exe) {
        if (!asAdministrator || !startThroughShell(exe)) startProcess(exe);
    };
    host.playerRunning = [&player] {
        const auto found = players(player);
        for (const HANDLE process : found) CloseHandle(process);
        return !found.empty();
    };
    host.endPlayer = [&player] {
        for (const HANDLE process : players(player)) {
            TerminateProcess(process, 1);
            WaitForSingleObject(process, 10000);
            CloseHandle(process);
        }
    };
    const auto outcome = camel::updater::install(options, host);
    if (SUCCEEDED(com)) CoUninitialize();
    return outcome == camel::updater::Outcome::Installed ? 0 : 1;
}
