// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pch.h"

#include "GlobalHotkeys.h"

#include "Common/util/Log.h"

namespace ui {
namespace {

constexpr wchar_t kWindowClass[] = L"CamelPlayer.HotkeyWindow";

} // namespace

GlobalHotkeys::GlobalHotkeys(std::function<void(Action)> pressed) : pressed_(std::move(pressed)) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW type{sizeof type};
    type.lpfnWndProc = windowProc;
    type.hInstance = instance;
    type.lpszClassName = kWindowClass;
    RegisterClassExW(&type);
    window_ = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
}

GlobalHotkeys::~GlobalHotkeys() {
    if (!window_) return;
    suspend();
    DestroyWindow(window_);
}

std::array<bool, GlobalHotkeys::kActions> GlobalHotkeys::apply(const camel::HotkeySettings& hotkeys) {
    suspend();
    std::array<bool, kActions> taken{};
    if (!window_) return taken;
    for (size_t i = 0; i < kActions; ++i) {
        const auto hotkey = of(hotkeys, Action(i));
        if (!hotkey.key) continue;
        if (!RegisterHotKey(window_, int(i) + 1, hotkey.modifiers | MOD_NOREPEAT, hotkey.key)) {
            taken[i] = true;
            camel::log::warn("hotkey %ls is used by another program", describe(hotkey).c_str());
        }
    }
    return taken;
}

void GlobalHotkeys::suspend() {
    for (size_t i = 0; i < kActions; ++i) UnregisterHotKey(window_, int(i) + 1);
}

camel::Hotkey GlobalHotkeys::of(const camel::HotkeySettings& hotkeys, Action action) {
    switch (action) {
    case Action::PlayPause:
        return hotkeys.playPause;
    case Action::Previous:
        return hotkeys.previous;
    default:
        return hotkeys.next;
    }
}

void GlobalHotkeys::set(camel::HotkeySettings& hotkeys, Action action, camel::Hotkey hotkey) {
    switch (action) {
    case Action::PlayPause:
        hotkeys.playPause = hotkey;
        break;
    case Action::Previous:
        hotkeys.previous = hotkey;
        break;
    default:
        hotkeys.next = hotkey;
        break;
    }
}

std::wstring GlobalHotkeys::describe(camel::Hotkey hotkey) {
    if (!hotkey.key) return {};
    std::wstring text;
    if (hotkey.modifiers & MOD_CONTROL) text += L"Ctrl+";
    if (hotkey.modifiers & MOD_ALT) text += L"Alt+";
    if (hotkey.modifiers & MOD_SHIFT) text += L"Shift+";
    if (hotkey.modifiers & MOD_WIN) text += L"Win+";
    switch (hotkey.key) {
    case VK_SPACE:
        return text + L"Space";
    case VK_LEFT:
        return text + L"←";
    case VK_RIGHT:
        return text + L"→";
    case VK_UP:
        return text + L"↑";
    case VK_DOWN:
        return text + L"↓";
    default:
        break;
    }
    // The key's name on the current keyboard layout; extended keys such as Page Up need the extended bit.
    UINT scan = MapVirtualKeyW(hotkey.key, MAPVK_VK_TO_VSC);
    switch (hotkey.key) {
    case VK_PRIOR:
    case VK_NEXT:
    case VK_END:
    case VK_HOME:
    case VK_INSERT:
    case VK_DELETE:
    case VK_DIVIDE:
    case VK_NUMLOCK:
        scan |= 0x100;
        break;
    default:
        break;
    }
    wchar_t name[64] = {};
    if (GetKeyNameTextW(LONG(scan) << 16, name, int(std::size(name))) > 0) return text + name;
    return text + L"0x" + std::to_wstring(hotkey.key);
}

LRESULT CALLBACK GlobalHotkeys::windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        SetWindowLongPtrW(window, GWLP_USERDATA, LONG_PTR(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
    } else if (message == WM_HOTKEY) {
        auto* self = reinterpret_cast<GlobalHotkeys*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self && wParam >= 1 && wParam <= kActions && self->pressed_) self->pressed_(Action(wParam - 1));
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace ui
