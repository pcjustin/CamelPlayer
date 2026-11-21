// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Windows.Foundation.h>

#include <exception>
#include <functional>

namespace ui {

// Runs `work` off the UI thread, then on `queue` the function it returns while `current()` still holds. What `work` throws
// goes to `failed`, called off the UI thread.
inline winrt::fire_and_forget background(winrt::Microsoft::UI::Dispatching::DispatcherQueue queue,
                                         std::function<std::function<void()>()> work, std::function<bool()> current,
                                         std::function<void(std::exception_ptr)> failed) {
    co_await winrt::resume_background();
    std::function<void()> apply;
    try {
        apply = work();
    } catch (...) {
        failed(std::current_exception());
        co_return;
    }
    queue.TryEnqueue([current = std::move(current), apply = std::move(apply)] {
        if (apply && current()) apply();
    });
}

} // namespace ui
