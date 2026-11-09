// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The playing track's waveform under the playback bar's position slider (docs/Task-breakdown.md M6.6-R07): thin bars
// symmetric about the middle, the played part in the accent colour and the rest grey. The slider keeps the clicks, the
// drags and its accessible name and value; its own track is hidden while a waveform shows.
#pragma once
#include <cstdint>
#include <functional>
#include <vector>

namespace ui {

class WaveformBar {
public:
    // `layer` lies under `slider` in the same cell.
    WaveformBar(winrt::Microsoft::UI::Xaml::Controls::Grid layer, winrt::Microsoft::UI::Xaml::Controls::Slider slider);

    void show(std::vector<uint8_t> peaks); // fades in
    void clear();                          // the plain slider again
    void setPlayed(double fraction);
    void repaint(); // after the theme or the size changes
    bool shown() const { return !peaks_.empty(); }

private:
    void draw();
    void showTrack(bool show);

    winrt::Microsoft::UI::Xaml::Controls::Grid layer_;
    winrt::Microsoft::UI::Xaml::Controls::Slider slider_;
    winrt::Microsoft::UI::Xaml::Shapes::Path rest_{nullptr}, played_{nullptr};
    winrt::Microsoft::UI::Xaml::Media::RectangleGeometry clip_{nullptr};
    std::vector<uint8_t> peaks_;
    double playedFraction_ = 0.0;
};

} // namespace ui
