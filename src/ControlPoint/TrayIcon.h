// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Notification-area icon (docs/Plan.md §8.4). A hidden top-level window receives its clicks, Explorer's restart
// message and the request a second launch sends; a popup menu needs such a window to become foreground and close.
#pragma once

#include <functional>
#include <string>

namespace ui {

class TrayIcon {
public:
    static constexpr wchar_t kWindowClass[] = L"CamelPlayer.TrayWindow";
    // Posted by a second launch: show the window of the running instance.
    static constexpr UINT kShowMessage = WM_APP + 2;

    struct Handlers {
        std::function<void()> open, exit;
    };
    TrayIcon(Handlers handlers, std::wstring openText, std::wstring exitText);
    ~TrayIcon();
    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    // A notification from the icon, shown once by Windows.
    void notify(const std::wstring& title, const std::wstring& text);

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);
    void add();
    void showMenu();

    Handlers handlers_;
    std::wstring openText_, exitText_;
    HWND window_ = nullptr;
    HICON icon_ = nullptr;
    UINT taskbarCreated_ = 0;
};

} // namespace ui
