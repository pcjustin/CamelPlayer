// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// What the taskbar does when a window's icon is set again (OP-R60): it asks the window for it once more only for a new
// handle. Disabled, as it shows a window with a taskbar button on the desktop.
#include <gtest/gtest.h>
#include <windows.h>

#include <atomic>
#include <chrono>

namespace {

std::atomic<int> asked{0};

LRESULT CALLBACK countingProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_GETICON) ++asked;
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

// The window's messages for `seconds`, as an idle UI thread handles them.
void pump(double seconds) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    MSG msg;
    while (std::chrono::steady_clock::now() < end) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
}

} // namespace

TEST(TaskbarIcon, DISABLED_ANewHandleMakesTheTaskbarAskAgain) {
    WNDCLASSW type{};
    type.lpfnWndProc = countingProc;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"CamelPlayerTaskbarIconTest";
    RegisterClassW(&type);
    const HWND window = CreateWindowExW(WS_EX_APPWINDOW, type.lpszClassName, L"Taskbar icon test", WS_OVERLAPPEDWINDOW, 0, 0, 320,
                                        200, nullptr, nullptr, type.hInstance, nullptr);
    ASSERT_TRUE(window);
    const HICON first = CopyIcon(LoadIconW(nullptr, IDI_APPLICATION));
    const HICON second = CopyIcon(first);
    SendMessageW(window, WM_SETICON, ICON_BIG, LPARAM(first));
    ShowWindow(window, SW_SHOWNOACTIVATE);
    pump(2.0);
    const int shown = asked.exchange(0);
    SendMessageW(window, WM_SETICON, ICON_BIG, LPARAM(first));
    pump(2.0);
    const int same = asked.exchange(0);
    SendMessageW(window, WM_SETICON, ICON_BIG, LPARAM(second));
    pump(2.0);
    const int renewed = asked.exchange(0);
    DestroyWindow(window);
    DestroyIcon(first);
    DestroyIcon(second);
    UnregisterClassW(type.lpszClassName, type.hInstance);
    std::printf("asked %d times when shown, %d for the same handle, %d for a new one\n", shown, same, renewed);
    EXPECT_GT(shown, 0);
    EXPECT_EQ(same, 0);
    EXPECT_GT(renewed, 0);
}
