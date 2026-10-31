// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The EQ editor of Settings › DSP (docs/Plan.md §5.9): the response curve, whose numbered points drag to change a
// band's frequency and gain (the wheel changes its Q), above one row of exact values per band and Add band.
#pragma once
#include <functional>
#include <vector>

#include "Common/settings/Settings.h"

namespace ui {

class EqEditor {
public:
    // `final` is false while a point is dragged, so the caller can apply the EQ often and save it once.
    using Changed = std::function<void(const camel::EqSettings&, bool final)>;

    EqEditor(winrt::Microsoft::UI::Xaml::Controls::Canvas curve, winrt::Microsoft::UI::Xaml::Controls::StackPanel rows,
             winrt::Microsoft::UI::Xaml::Controls::Button addBand, Changed changed);

    // Shows `eq`, drawn at `rateHz` (the playing output rate, else 48 kHz).
    void show(const camel::EqSettings& eq, uint32_t rateHz);
    const camel::EqSettings& settings() const { return eq_; }

private:
    // One band's controls, kept so a dragged point updates its row and a type change enables the right fields.
    struct Row {
        winrt::Microsoft::UI::Xaml::Controls::NumberBox frequency{nullptr}, gain{nullptr}, q{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::ComboBox slope{nullptr};
    };

    void rebuildRows();
    // Rebuilds the grid, the curves and the points.
    void redraw();
    // Moves the curves and the points only, so a point being dragged keeps its pointer.
    void refresh();
    double pointDb(size_t band, const std::vector<double>& leftDb) const;
    void changed(bool final);
    void addBand();
    void removeBand(size_t index);
    double xOf(double frequency) const;
    double yOf(double db) const;
    double frequencyAt(double x) const;
    double dbAt(double y) const;

    winrt::Microsoft::UI::Xaml::Controls::Canvas curve_;
    winrt::Microsoft::UI::Xaml::Controls::StackPanel rows_;
    winrt::Microsoft::UI::Xaml::Controls::Button addBand_;
    Changed changed_;
    std::vector<Row> rowControls_;
    std::vector<winrt::Microsoft::UI::Xaml::FrameworkElement> handles_;
    winrt::Microsoft::UI::Xaml::Shapes::Polyline leftCurve_{nullptr}, rightCurve_{nullptr};
    std::vector<double> frequencies_;
    camel::EqSettings eq_;
    uint32_t rateHz_ = 48000;
    double rangeDb_ = 15.0;
    int dragging_ = -1;
    bool populating_ = false;
};

} // namespace ui
