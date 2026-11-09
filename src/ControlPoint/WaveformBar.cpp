// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pch.h"

#include "WaveformBar.h"

#include <algorithm>

#include "ThemeBrush.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;
using namespace Microsoft::UI::Xaml::Shapes;

namespace ui {

namespace {

constexpr double kPitch = 3.0; // a 2 px bar every 3 px
constexpr double kBar = 2.0;

// The slider template's parts that draw its track.
void forTrack(DependencyObject const& element, const std::function<void(FrameworkElement const&)>& each) {
    const int32_t count = VisualTreeHelper::GetChildrenCount(element);
    for (int32_t i = 0; i < count; ++i) {
        const auto child = VisualTreeHelper::GetChild(element, i);
        if (const auto framework = child.try_as<FrameworkElement>()) {
            const auto name = framework.Name();
            if (name == L"HorizontalTrackRect" || name == L"HorizontalDecreaseRect") each(framework);
        }
        forTrack(child, each);
    }
}

} // namespace

WaveformBar::WaveformBar(Grid layer, Slider slider) : layer_(std::move(layer)), slider_(std::move(slider)) {
    layer_.IsHitTestVisible(false);
    layer_.Opacity(0.0);
    layer_.OpacityTransition(ScalarTransition());
    rest_ = Path();
    played_ = Path();
    clip_ = RectangleGeometry();
    played_.Clip(clip_);
    layer_.Children().Append(rest_);
    layer_.Children().Append(played_);
    layer_.SizeChanged([this](auto&&, auto&&) { draw(); });
}

void WaveformBar::show(std::vector<uint8_t> peaks) {
    peaks_ = std::move(peaks);
    draw();
    showTrack(false);
    layer_.Opacity(1.0);
}

void WaveformBar::clear() {
    peaks_.clear();
    layer_.Opacity(0.0);
    showTrack(true);
}

void WaveformBar::setPlayed(double fraction) {
    playedFraction_ = std::clamp(fraction, 0.0, 1.0);
    const double width = layer_.ActualWidth(), height = layer_.ActualHeight();
    clip_.Rect({0.0f, 0.0f, float(width * playedFraction_), float(height)});
}

void WaveformBar::repaint() {
    draw();
}

void WaveformBar::showTrack(bool show) {
    forTrack(slider_, [show](FrameworkElement const& part) { part.Opacity(show ? 1.0 : 0.0); });
}

void WaveformBar::draw() {
    const double width = layer_.ActualWidth(), height = layer_.ActualHeight();
    if (peaks_.empty() || width < kPitch || height <= 0) return;
    const auto bars = size_t(width / kPitch);
    for (const auto& path : {rest_, played_}) {
        GeometryGroup group;
        for (size_t i = 0; i < bars; ++i) {
            const size_t from = i * peaks_.size() / bars;
            const size_t to = std::max(from + 1, (i + 1) * peaks_.size() / bars);
            uint8_t peak = 0;
            for (size_t p = from; p < to && p < peaks_.size(); ++p) peak = std::max(peak, peaks_[p]);
            const double barHeight = std::max(1.0, double(peak) / 255.0 * height);
            RectangleGeometry bar;
            bar.Rect({float(double(i) * kPitch), float((height - barHeight) / 2.0), float(kBar), float(barHeight)});
            group.Children().Append(bar);
        }
        path.Data(group);
    }
    rest_.Fill(themeBrush(L"ControlStrongFillColorDisabledBrush"));
    played_.Fill(themeBrush(L"AccentFillColorDefaultBrush"));
    setPlayed(playedFraction_);
}

} // namespace ui
