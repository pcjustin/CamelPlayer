// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Global hotkeys (docs/Plan.md §8.4), registered with RegisterHotKey on a message-only window of the UI thread.
#pragma once

#include <array>
#include <functional>
#include <string>

#include "Common/settings/Settings.h"

namespace ui {

class GlobalHotkeys {
public:
    enum class Action : uint8_t { PlayPause, Previous, Next };
    static constexpr size_t kActions = 3;

    explicit GlobalHotkeys(std::function<void(Action)> pressed);
    ~GlobalHotkeys();
    GlobalHotkeys(const GlobalHotkeys&) = delete;
    GlobalHotkeys& operator=(const GlobalHotkeys&) = delete;

    // Registers the hotkeys, replacing the previous ones; returns which actions another program already holds.
    std::array<bool, kActions> apply(const camel::HotkeySettings& hotkeys);
    // Releases every hotkey, so a new combination can be pressed without triggering one.
    void suspend();

    static camel::Hotkey of(const camel::HotkeySettings& hotkeys, Action action);
    static void set(camel::HotkeySettings& hotkeys, Action action, camel::Hotkey hotkey);
    // "Ctrl+Alt+Space"; empty for no hotkey.
    static std::wstring describe(camel::Hotkey hotkey);

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    std::function<void(Action)> pressed_;
    HWND window_ = nullptr;
};

} // namespace ui
