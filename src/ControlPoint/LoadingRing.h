// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <chrono>

namespace ui {

// A ring turning while a page's content is on its way, shown once that takes longer than a moment so a quick page does
// not flash it.
class LoadingRing {
public:
    void attach(winrt::Microsoft::UI::Xaml::Controls::ProgressRing const& ring) {
        ring_ = ring;
        timer_ = ring.DispatcherQueue().CreateTimer();
        timer_.Interval(std::chrono::milliseconds(300));
        timer_.IsRepeating(false);
        timer_.Tick([ring](auto&&, auto&&) {
            ring.IsActive(true);
            ring.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
        });
    }
    void start() const {
        if (timer_) timer_.Start();
    }
    void stop() const {
        if (!timer_) return;
        timer_.Stop();
        ring_.IsActive(false);
        ring_.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
    }

private:
    winrt::Microsoft::UI::Xaml::Controls::ProgressRing ring_{nullptr};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer timer_{nullptr};
};

} // namespace ui
