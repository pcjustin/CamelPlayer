// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pch.h"

#include "TrayIcon.h"

#include <commctrl.h>
#include <shellapi.h>

#include "Common/util/Log.h"

namespace ui {
namespace {

constexpr UINT kIconId = 1;
constexpr UINT kCallbackMessage = WM_APP + 1;
constexpr UINT kOpenCommand = 1, kExitCommand = 2, kPlayPauseCommand = 3, kPauseAllCommand = 4;

// Windows draws a process's popup menus light unless the process asks for dark ones, which uxtheme exports only by
// ordinal: SetPreferredAppMode (135) and FlushMenuThemes (136), since Windows 10 1903.
void drawMenusDark(bool dark) {
    enum class AppMode : int { Default, AllowDark, ForceDark, ForceLight };
    using SetPreferredAppMode = AppMode(WINAPI*)(AppMode);
    using FlushMenuThemes = void(WINAPI*)();
    static const HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!uxtheme) return;
    const auto setMode = reinterpret_cast<SetPreferredAppMode>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
    const auto flush = reinterpret_cast<FlushMenuThemes>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));
    if (!setMode || !flush) return;
    setMode(dark ? AppMode::ForceDark : AppMode::ForceLight);
    flush();
}

} // namespace

TrayIcon::TrayIcon(Handlers handlers, Texts texts) : handlers_(std::move(handlers)), texts_(std::move(texts)) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{sizeof windowClass};
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClass;
    RegisterClassExW(&windowClass);
    window_ =
        CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"CamelPlayer", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, this);
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    // A second launch runs unelevated like this one; let its show request through UIPI either way.
    ChangeWindowMessageFilterEx(window_, kShowMessage, MSGFLT_ALLOW, nullptr);
    // The executable's own icon (App.rc), at the notification area's size for the display's DPI.
    if (FAILED(LoadIconMetric(instance, MAKEINTRESOURCEW(1), LIM_SMALL, &icon_))) icon_ = LoadIconW(nullptr, IDI_APPLICATION);
    add();
}

TrayIcon::~TrayIcon() {
    NOTIFYICONDATAW data{sizeof data};
    data.hWnd = window_;
    data.uID = kIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
    if (window_) DestroyWindow(window_);
    if (icon_) DestroyIcon(icon_);
}

void TrayIcon::add() {
    NOTIFYICONDATAW data{sizeof data};
    data.hWnd = window_;
    data.uID = kIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kCallbackMessage;
    data.hIcon = icon_;
    wcscpy_s(data.szTip, L"CamelPlayer");
    if (!Shell_NotifyIconW(NIM_ADD, &data)) camel::log::warn("tray: adding the notification icon failed");
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
}

void TrayIcon::notify(const std::wstring& title, const std::wstring& text) {
    NOTIFYICONDATAW data{sizeof data};
    data.hWnd = window_;
    data.uID = kIconId;
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
    data.hBalloonIcon = icon_;
    wcsncpy_s(data.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(data.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void TrayIcon::showMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kOpenCommand, texts_.open.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const bool playing = handlers_.playing && handlers_.playing();
    AppendMenuW(menu, MF_STRING, kPlayPauseCommand, (playing ? texts_.pause : texts_.play).c_str());
    AppendMenuW(menu, MF_STRING | (handlers_.anyPlaying && handlers_.anyPlaying() ? 0 : MF_GRAYED), kPauseAllCommand,
                texts_.pauseAll.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kExitCommand, texts_.exit.c_str());
    SetMenuDefaultItem(menu, kOpenCommand, FALSE);
    drawMenusDark(handlers_.dark && handlers_.dark());
    POINT point{};
    GetCursorPos(&point);
    SetForegroundWindow(window_); // otherwise the menu stays open after clicking elsewhere
    const UINT command =
        UINT(TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, point.x, point.y, window_, nullptr));
    DestroyMenu(menu);
    if (command == kOpenCommand && handlers_.open) handlers_.open();
    if (command == kPlayPauseCommand && handlers_.playPause) handlers_.playPause();
    if (command == kPauseAllCommand && handlers_.pauseAll) handlers_.pauseAll();
    if (command == kExitCommand && handlers_.exit) handlers_.exit();
}

LRESULT TrayIcon::handle(UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == kCallbackMessage) {
        switch (LOWORD(lParam)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            if (handlers_.open) handlers_.open();
            break;
        case WM_CONTEXTMENU:
            showMenu();
            break;
        default:
            break;
        }
        return 0;
    }
    if (message == kShowMessage) {
        if (handlers_.open) handlers_.open();
        return 0;
    }
    if (message == WM_COMMAND && LOWORD(wParam) == kExitCommand) { // startup measurements leave as the menu does
        if (handlers_.exit) handlers_.exit();
        return 0;
    }
    if (message == taskbarCreated_ && taskbarCreated_) {
        add(); // Explorer restarted and lost every icon
        return 0;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

LRESULT CALLBACK TrayIcon::windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* self = static_cast<TrayIcon*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, LONG_PTR(self));
    }
    if (auto* self = reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(window, GWLP_USERDATA)))
        return self->handle(message, wParam, lParam);
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace ui
