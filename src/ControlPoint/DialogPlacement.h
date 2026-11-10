// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Dialogs open in a layer of their own that does not inherit the theme the window content requests, one at a time.
#pragma once
#include <coroutine>
#include <deque>

namespace ui {

// Opens `dialog` over `root` in that window's light or dark theme.
inline void placeDialog(winrt::Microsoft::UI::Xaml::Controls::ContentDialog const& dialog,
                        winrt::Microsoft::UI::Xaml::XamlRoot const& root) {
    dialog.XamlRoot(root);
    if (const auto content = root.Content().try_as<winrt::Microsoft::UI::Xaml::FrameworkElement>())
        dialog.RequestedTheme(content.ActualTheme());
}

namespace detail {

struct DialogTurns {
    bool open = false;
    std::deque<std::coroutine_handle<>> waiting;
};

inline DialogTurns& dialogTurns() {
    static DialogTurns turns;
    return turns;
}

} // namespace detail

// WinUI throws for a second ContentDialog while one is open, so every dialog is shown here: one asked for while another
// is open waits until it closes, then opens. UI thread only.
inline winrt::Windows::Foundation::IAsyncOperation<winrt::Microsoft::UI::Xaml::Controls::ContentDialogResult>
showDialog(winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog) {
    auto& turns = detail::dialogTurns();
    struct Turn {
        detail::DialogTurns& turns;
        bool await_ready() const noexcept { return !turns.open; }
        void await_suspend(std::coroutine_handle<> next) const { turns.waiting.push_back(next); }
        void await_resume() const noexcept {}
    };
    co_await Turn{turns};
    turns.open = true;
    const auto queue = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
    const auto passOn = [&turns, queue] {
        if (turns.waiting.empty()) {
            turns.open = false;
            return;
        }
        const auto next = turns.waiting.front();
        turns.waiting.pop_front();
        queue.TryEnqueue([next] { next.resume(); });
    };
    try {
        const auto result = co_await dialog.ShowAsync();
        passOn();
        co_return result;
    } catch (...) {
        passOn();
        throw;
    }
}

} // namespace ui
