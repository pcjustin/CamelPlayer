// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The executable's own icon (its first icon resource, App.rc) for the windows, so none depends on a file beside it.
#pragma once
#include <map>
#include <tuple>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Microsoft.UI.Windowing.h>

namespace ui {

// The icon at the size `widthMetric` (SM_CXICON or SM_CXSMICON) has at `dpi`, loaded once and kept, as windows hold it.
// Two copies of each, so that a window can be given a handle it does not hold, none it may hold being destroyed.
inline HICON appIcon(int widthMetric, UINT dpi, int copy = 0) {
    static std::map<std::tuple<int, UINT, int>, HICON> loaded;
    auto& icon = loaded[{widthMetric, dpi, copy}];
    if (!icon) {
        const int size = GetSystemMetricsForDpi(widthMetric, dpi);
        icon = HICON(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
    }
    return icon;
}

inline void setAppIcon(winrt::Microsoft::UI::Windowing::AppWindow const& window, int copy = 0) {
    const HWND hwnd = winrt::Microsoft::UI::GetWindowFromWindowId(window.Id());
    const UINT dpi = GetDpiForWindow(hwnd);
    window.SetIcon(winrt::Microsoft::UI::GetIconIdFromIcon(appIcon(SM_CXICON, dpi, copy)));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, LPARAM(appIcon(SM_CXSMICON, dpi, copy)));
}

} // namespace ui
