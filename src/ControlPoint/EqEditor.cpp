// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pch.h"

#include "EqEditor.h"

#include <winrt/Microsoft.UI.Xaml.Input.h>

#include <algorithm>
#include <cmath>

#include "UiText.h"
#include "Engine/Dsp.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using camel::EqBand;
using camel::EqChannels;
using camel::EqKind;

namespace ui {
namespace {

constexpr double kLowHz = 20.0, kHighHz = 20000.0;
constexpr double kHandle = 18.0;
constexpr int kCurvePoints = 256;
constexpr const wchar_t* kKindKeys[] = {L"Eq_Kind_Peaking",  L"Eq_Kind_LowShelf", L"Eq_Kind_HighShelf", L"Eq_Kind_LowPass",
                                        L"Eq_Kind_HighPass", L"Eq_Kind_BandPass", L"Eq_Kind_Notch",     L"Eq_Kind_AllPass"};
constexpr const wchar_t* kChannelKeys[] = {L"Eq_Channels_All", L"Eq_Channels_Left", L"Eq_Channels_Right"};
constexpr uint32_t kSlopes[] = {6, 12, 18, 24, 36, 48};

Media::Brush brush(const wchar_t* key) {
    return Application::Current().Resources().Lookup(box_value(key)).as<Media::Brush>();
}

bool hasGain(EqKind kind) {
    return kind == EqKind::Peaking || kind == EqKind::LowShelf || kind == EqKind::HighShelf;
}

bool hasSlope(EqKind kind) {
    return kind == EqKind::LowPass || kind == EqKind::HighPass;
}

std::wstring hertz(double f) {
    wchar_t text[16];
    if (f >= 1000)
        swprintf_s(text, L"%gk", f / 1000);
    else
        swprintf_s(text, L"%g", f);
    return text;
}

NumberBox numberBox(double value, double minimum, double maximum, double step, double width) {
    NumberBox box;
    box.Minimum(minimum);
    box.Maximum(maximum);
    box.SmallChange(step);
    box.LargeChange(step * 10);
    box.Value(value);
    box.Width(width);
    box.SpinButtonPlacementMode(NumberBoxSpinButtonPlacementMode::Hidden);
    return box;
}

ComboBox comboBox(std::initializer_list<hstring> items, int selected, double width) {
    ComboBox combo;
    for (const auto& item : items) combo.Items().Append(box_value(item));
    combo.SelectedIndex(selected);
    combo.Width(width);
    return combo;
}

} // namespace

EqEditor::EqEditor(Canvas curve, StackPanel rows, Button addBand, Changed changed)
    : curve_(std::move(curve)), rows_(std::move(rows)), addBand_(std::move(addBand)), changed_(std::move(changed)) {
    frequencies_.resize(kCurvePoints);
    for (int i = 0; i < kCurvePoints; ++i)
        frequencies_[size_t(i)] = kLowHz * std::pow(kHighHz / kLowHz, double(i) / (kCurvePoints - 1));
    curve_.SizeChanged([this](auto&&, auto&&) { redraw(); });
    addBand_.Click([this](auto&&, auto&&) { this->addBand(); });
}

void EqEditor::show(const camel::EqSettings& eq, uint32_t rateHz) {
    if (dragging_ >= 0) return;
    eq_ = eq;
    rateHz_ = rateHz ? rateHz : 48000;
    rebuildRows();
    redraw();
}

double EqEditor::xOf(double frequency) const {
    const double width = curve_.ActualWidth();
    const double f = std::clamp(frequency, kLowHz, kHighHz);
    return width * std::log(f / kLowHz) / std::log(kHighHz / kLowHz);
}

double EqEditor::frequencyAt(double x) const {
    const double width = std::max(1.0, curve_.ActualWidth());
    return kLowHz * std::pow(kHighHz / kLowHz, std::clamp(x / width, 0.0, 1.0));
}

double EqEditor::yOf(double db) const {
    const double half = curve_.ActualHeight() / 2;
    return half - std::clamp(db, -rangeDb_, rangeDb_) / rangeDb_ * (half - kHandle / 2);
}

double EqEditor::dbAt(double y) const {
    const double half = curve_.ActualHeight() / 2;
    return std::clamp((half - y) / std::max(1.0, half - kHandle / 2) * rangeDb_, -rangeDb_, rangeDb_);
}

void EqEditor::changed(bool final) {
    addBand_.IsEnabled(eq_.bands.size() < camel::EqSettings::kMaxUiBands);
    if (changed_) changed_(eq_, final);
}

void EqEditor::addBand() {
    if (eq_.bands.size() >= camel::EqSettings::kMaxUiBands) return;
    eq_.bands.push_back({EqKind::Peaking, EqChannels::All, 12, 1000.0, 0.0, 0.71, true});
    rebuildRows();
    redraw();
    changed(true);
}

void EqEditor::removeBand(size_t index) {
    if (index >= eq_.bands.size()) return;
    eq_.bands.erase(eq_.bands.begin() + ptrdiff_t(index));
    // The row's own click is still being handled; rebuild once it returns.
    curve_.DispatcherQueue().TryEnqueue([this] {
        rebuildRows();
        redraw();
    });
    changed(true);
}

void EqEditor::rebuildRows() {
    populating_ = true;
    rows_.Children().Clear();
    rowControls_.clear();
    const auto header = [](const wchar_t* key, double width) {
        TextBlock text;
        text.Text(key ? tr(key) : hstring{});
        text.Width(width);
        text.Style(Application::Current().Resources().Lookup(box_value(L"CaptionTextBlockStyle")).as<Style>());
        text.Foreground(brush(L"TextFillColorSecondaryBrush"));
        return text;
    };
    StackPanel titles;
    titles.Orientation(Orientation::Horizontal);
    titles.Spacing(8);
    for (const auto& [key, width] : {std::pair<const wchar_t*, double>{nullptr, 56},
                                     {L"Eq_Column_Type", 120},
                                     {L"Eq_Column_Channels", 96},
                                     {L"Eq_Column_Frequency", 96},
                                     {L"Eq_Column_Gain", 80},
                                     {L"Eq_Column_Q", 72},
                                     {L"Eq_Column_Slope", 96}})
        titles.Children().Append(header(key, width));
    rows_.Children().Append(titles);

    for (size_t i = 0; i < eq_.bands.size(); ++i) {
        const auto& band = eq_.bands[i];
        StackPanel row;
        row.Orientation(Orientation::Horizontal);
        row.Spacing(8);

        CheckBox on;
        on.IsChecked(band.enabled);
        on.MinWidth(0);
        on.Width(56);
        on.Content(box_value(to_hstring(i + 1)));
        on.Click([this, i, on](auto&&, auto&&) {
            eq_.bands[i].enabled = on.IsChecked().Value();
            redraw();
            changed(true);
        });
        row.Children().Append(on);

        auto kind = comboBox({tr(kKindKeys[0]), tr(kKindKeys[1]), tr(kKindKeys[2]), tr(kKindKeys[3]), tr(kKindKeys[4]),
                              tr(kKindKeys[5]), tr(kKindKeys[6]), tr(kKindKeys[7])},
                             int(band.kind) - 1, 120);
        row.Children().Append(kind);
        auto channels = comboBox({tr(kChannelKeys[0]), tr(kChannelKeys[1]), tr(kChannelKeys[2])}, int(band.channels), 96);
        row.Children().Append(channels);

        Row controls;
        controls.frequency = numberBox(band.frequencyHz, 10, 40000, 1, 96);
        controls.gain = numberBox(band.gainDb, -30, 30, 0.1, 80);
        controls.q = numberBox(band.q, 0.1, 20, 0.01, 72);
        int slope = 1;
        for (size_t s = 0; s < std::size(kSlopes); ++s) {
            if (kSlopes[s] == band.slope) slope = int(s);
        }
        controls.slope = comboBox({L"6 dB", L"12 dB", L"18 dB", L"24 dB", L"36 dB", L"48 dB"}, slope, 96);
        const auto enable = [controls](const EqBand& b) {
            controls.gain.IsEnabled(hasGain(b.kind));
            controls.slope.IsEnabled(hasSlope(b.kind));
            controls.q.IsEnabled(!hasSlope(b.kind) || b.slope == 12);
        };
        enable(band);
        row.Children().Append(controls.frequency);
        row.Children().Append(controls.gain);
        row.Children().Append(controls.q);
        row.Children().Append(controls.slope);

        kind.SelectionChanged([this, i, kind, enable](auto&&, auto&&) {
            if (populating_ || kind.SelectedIndex() < 0) return;
            eq_.bands[i].kind = EqKind(kind.SelectedIndex() + 1);
            enable(eq_.bands[i]);
            redraw();
            changed(true);
        });
        channels.SelectionChanged([this, i, channels](auto&&, auto&&) {
            if (populating_ || channels.SelectedIndex() < 0) return;
            eq_.bands[i].channels = EqChannels(channels.SelectedIndex());
            redraw();
            changed(true);
        });
        controls.slope.SelectionChanged([this, i, slopeBox = controls.slope, enable](auto&&, auto&&) {
            if (populating_ || slopeBox.SelectedIndex() < 0) return;
            eq_.bands[i].slope = kSlopes[slopeBox.SelectedIndex()];
            enable(eq_.bands[i]);
            redraw();
            changed(true);
        });
        const auto onValue = [this, i](double EqBand::* field) {
            return [this, i, field](NumberBox const&, NumberBoxValueChangedEventArgs const& e) {
                if (populating_ || std::isnan(e.NewValue())) return;
                eq_.bands[i].*field = e.NewValue();
                redraw();
                changed(true);
            };
        };
        controls.frequency.ValueChanged(onValue(&EqBand::frequencyHz));
        controls.gain.ValueChanged(onValue(&EqBand::gainDb));
        controls.q.ValueChanged(onValue(&EqBand::q));

        Button remove;
        remove.Padding(ThicknessHelper::FromUniformLength(6));
        FontIcon icon;
        icon.Glyph(L"\xE711");
        icon.FontSize(12);
        remove.Content(icon);
        ToolTipService::SetToolTip(remove, box_value(tr(L"Eq_RemoveBand")));
        Automation::AutomationProperties::SetName(remove, tr(L"Eq_RemoveBand"));
        remove.Click([this, i](auto&&, auto&&) { removeBand(i); });
        row.Children().Append(remove);

        rows_.Children().Append(row);
        rowControls_.push_back(controls);
    }
    addBand_.IsEnabled(eq_.bands.size() < camel::EqSettings::kMaxUiBands);
    populating_ = false;
}

double EqEditor::pointDb(size_t band, const std::vector<double>& leftDb) const {
    const auto& b = eq_.bands[band];
    if (hasGain(b.kind)) return b.gainDb;
    // A point without a gain of its own sits on the curve.
    const double at = std::log(std::clamp(b.frequencyHz, kLowHz, kHighHz) / kLowHz) / std::log(kHighHz / kLowHz);
    return leftDb[size_t(std::lround(at * (kCurvePoints - 1)))];
}

void EqEditor::redraw() {
    const double width = curve_.ActualWidth(), height = curve_.ActualHeight();
    curve_.Children().Clear();
    handles_.clear();
    leftCurve_ = rightCurve_ = nullptr;
    if (width <= 0 || height <= 0) return;

    double largest = 0;
    for (const auto& b : eq_.bands)
        if (b.enabled && hasGain(b.kind)) largest = std::max(largest, std::abs(b.gainDb));
    rangeDb_ = std::clamp(std::ceil((largest + 3) / 3) * 3, 12.0, 30.0);

    const auto line = [&](double x1, double y1, double x2, double y2, const wchar_t* color) {
        Shapes::Line l;
        l.X1(x1);
        l.Y1(y1);
        l.X2(x2);
        l.Y2(y2);
        l.Stroke(brush(color));
        l.StrokeThickness(1);
        curve_.Children().Append(l);
    };
    const auto label = [&](const std::wstring& text, double x, double y) {
        TextBlock t;
        t.Text(text);
        t.FontSize(10);
        t.Foreground(brush(L"TextFillColorTertiaryBrush"));
        Canvas::SetLeft(t, x);
        Canvas::SetTop(t, y);
        curve_.Children().Append(t);
    };
    for (double f : {20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0}) {
        line(xOf(f), 0, xOf(f), height, L"DividerStrokeColorDefaultBrush");
        label(hertz(f), std::min(xOf(f) + 2, width - 24), height - 14);
    }
    for (double db : {-rangeDb_, -rangeDb_ / 2, 0.0, rangeDb_ / 2, rangeDb_}) {
        line(0, yOf(db), width, yOf(db), db == 0 ? L"ControlStrongStrokeColorDefaultBrush" : L"DividerStrokeColorDefaultBrush");
        wchar_t text[16];
        swprintf_s(text, L"%+g dB", db);
        label(text, 2, std::clamp(yOf(db) - 14, 0.0, height - 14));
    }

    const auto curve = [&](const wchar_t* color) {
        Shapes::Polyline p;
        p.Stroke(brush(color));
        p.StrokeThickness(2);
        p.Opacity(eq_.enabled ? 1.0 : 0.45);
        curve_.Children().Append(p);
        return p;
    };
    rightCurve_ = curve(L"TextFillColorSecondaryBrush");
    leftCurve_ = curve(L"AccentFillColorDefaultBrush");

    for (size_t i = 0; i < eq_.bands.size(); ++i) {
        const auto& band = eq_.bands[i];
        Grid handle;
        handle.Width(kHandle);
        handle.Height(kHandle);
        Shapes::Ellipse dot;
        dot.Fill(band.enabled ? brush(L"AccentFillColorDefaultBrush") : brush(L"ControlFillColorDefaultBrush"));
        dot.Stroke(brush(L"ControlStrongStrokeColorDefaultBrush"));
        dot.StrokeThickness(1);
        handle.Children().Append(dot);
        TextBlock number;
        number.Text(to_hstring(i + 1));
        number.FontSize(10);
        number.HorizontalAlignment(HorizontalAlignment::Center);
        number.VerticalAlignment(VerticalAlignment::Center);
        number.Foreground(band.enabled ? brush(L"TextOnAccentFillColorPrimaryBrush") : brush(L"TextFillColorPrimaryBrush"));
        handle.Children().Append(number);
        Automation::AutomationProperties::SetName(handle, to_hstring(i + 1));

        handle.PointerPressed([this, i, handle](auto&&, Input::PointerRoutedEventArgs const& e) {
            dragging_ = int(i);
            handle.CapturePointer(e.Pointer());
            e.Handled(true);
        });
        handle.PointerMoved([this, i](auto&&, Input::PointerRoutedEventArgs const& e) {
            if (dragging_ != int(i)) return;
            const auto at = e.GetCurrentPoint(curve_).Position();
            auto& band = eq_.bands[i];
            band.frequencyHz = std::round(frequencyAt(at.X));
            if (hasGain(band.kind)) band.gainDb = std::clamp(std::round(dbAt(at.Y) * 10) / 10, -30.0, 30.0);
            populating_ = true;
            rowControls_[i].frequency.Value(band.frequencyHz);
            rowControls_[i].gain.Value(band.gainDb);
            populating_ = false;
            refresh();
            changed(false);
            e.Handled(true);
        });
        handle.PointerReleased([this, handle](auto&&, Input::PointerRoutedEventArgs const& e) {
            if (dragging_ < 0) return;
            dragging_ = -1;
            handle.ReleasePointerCaptures();
            redraw();
            changed(true);
            e.Handled(true);
        });
        handle.PointerCaptureLost([this](auto&&, auto&&) {
            if (dragging_ < 0) return;
            dragging_ = -1;
            changed(true);
        });
        handle.PointerWheelChanged([this, i](auto&&, Input::PointerRoutedEventArgs const& e) {
            const int delta = e.GetCurrentPoint(curve_).Properties().MouseWheelDelta();
            auto& band = eq_.bands[i];
            band.q = std::clamp(std::round(band.q * std::pow(1.1, delta / 120.0) * 100) / 100, 0.1, 20.0);
            populating_ = true;
            rowControls_[i].q.Value(band.q);
            populating_ = false;
            refresh();
            changed(true);
            e.Handled(true);
        });
        curve_.Children().Append(handle);
        handles_.push_back(handle);
    }
    refresh();
}

void EqEditor::refresh() {
    if (!leftCurve_) return;
    // The bands' own response as they are set, also while the EQ is off; with the chain's preamp in it, a dragged
    // point would move away from the pointer.
    auto shown = eq_;
    shown.enabled = true;
    const auto analysis = camel::audio::analyzeDsp(camel::audio::eqChain(shown), rateHz_, frequencies_);
    const bool split = std::ranges::any_of(eq_.bands, [](const EqBand& b) { return b.enabled && b.channels != EqChannels::All; });
    const auto plot = [&](Shapes::Polyline const& line, const std::vector<double>& db, bool visible) {
        line.Points().Clear();
        if (!visible) return;
        for (size_t i = 0; i < frequencies_.size(); ++i) line.Points().Append({float(xOf(frequencies_[i])), float(yOf(db[i]))});
    };
    plot(leftCurve_, analysis.leftDb, true);
    plot(rightCurve_, analysis.rightDb, split);
    for (size_t i = 0; i < handles_.size() && i < eq_.bands.size(); ++i) {
        Canvas::SetLeft(handles_[i], xOf(eq_.bands[i].frequencyHz) - kHandle / 2);
        Canvas::SetTop(handles_[i], yOf(pointDb(i, analysis.leftDb)) - kHandle / 2);
    }
}

} // namespace ui
