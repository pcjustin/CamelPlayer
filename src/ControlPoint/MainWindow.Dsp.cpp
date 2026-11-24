// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pch.h"

#include "MainWindow.xaml.h"
#include "MainWindowParts.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <utility>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>

#include "DialogPlacement.h"
#include "FilePicker.h"
#include "UiText.h"
#include "Common/settings/EqText.h"
#include "Common/settings/Settings.h"
#include "Common/util/Log.h"
#include "Common/util/Text.h"
#include "Engine/Dsp.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace camel;
using namespace camel::audio;
using ui::tr;
using namespace camel::server;

namespace winrt::CamelPlayer::implementation {

using namespace mainwindow;

namespace {

std::wstring kilohertz(uint32_t rate) {
    wchar_t text[16];
    swprintf_s(text, L"%g", rate / 1000.0);
    return text;
}

hstring impulseInfoText(const ImpulseInfo& info, uint32_t rate) {
    const wchar_t* layout = info.channels == 1   ? L"Convolution_Mono"
                            : info.channels == 2 ? L"Convolution_Stereo"
                                                 : L"Convolution_TrueStereo";
    wchar_t seconds[16];
    swprintf_s(seconds, L"%.2f", double(info.frames) / rate);
    std::wstring rates;
    for (const auto r : info.rates) rates += (rates.empty() ? L"" : L", ") + kilohertz(r);
    hstring text =
        ui::format(L"Convolution_Info", {std::wstring(tr(layout)), std::to_wstring(info.frames), seconds, kilohertz(rate)}) +
        L" " + ui::format(L"Convolution_Rates", {rates});
    if (info.converted) text = text + L" " + ui::format(L"Convolution_Converted", {kilohertz(info.sourceRate)});
    return text;
}

hstring impulseProblemText(ImpulseProblem problem) {
    switch (problem) {
    case ImpulseProblem::Layout:
        return tr(L"Convolution_Layout");
    case ImpulseProblem::TooLong:
        return tr(L"Convolution_TooLong");
    default:
        return tr(L"Convolution_Unreadable");
    }
}

} // namespace

void MainWindow::OnEqToggled(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    if (populating_) return;
    settings_.eq.enabled = EqSwitch().IsOn();
    eqEditor_->show(settings_.eq, eqRate());
    eqChanged(true);
}

uint32_t MainWindow::eqRate() const {
    if (server_)
        if (const auto output = server_->snapshot().playback.output) return output->sampleRate;
    return 48000;
}

void MainWindow::showEq() {
    if (!eqEditor_) return;
    const bool wasPopulating = std::exchange(populating_, true);
    EqSwitch().IsOn(settings_.eq.enabled);
    EqPresetCombo().Items().Clear();
    int selected = -1;
    for (size_t i = 0; i < settings_.eq.presets.size(); ++i) {
        EqPresetCombo().Items().Append(box_value(ui::utf8(settings_.eq.presets[i].name)));
        if (settings_.eq.presets[i].name == settings_.eq.preset) selected = int(i);
    }
    EqPresetCombo().SelectedIndex(selected);
    EqDeletePresetButton().IsEnabled(selected >= 0);
    eqEditor_->show(settings_.eq, eqRate());
    populating_ = wasPopulating;
    showDspSummary();
}

void MainWindow::eqChanged(bool final) {
    settings_.eq.bands = eqEditor_->settings().bands;
    showDspSummary();
    if (final) {
        eqTimer_.Stop();
        settingsChanged();
    } else if (!eqTimer_.IsRunning()) {
        eqTimer_.Start();
    }
}

void MainWindow::OnEqPresetChanged(Windows::Foundation::IInspectable const&, SelectionChangedEventArgs const&) {
    const int index = EqPresetCombo().SelectedIndex();
    if (populating_ || index < 0 || size_t(index) >= settings_.eq.presets.size()) return;
    settings_.eq.bands = settings_.eq.presets[size_t(index)].bands;
    settings_.eq.preset = settings_.eq.presets[size_t(index)].name;
    showEq();
    settingsChanged();
}

winrt::fire_and_forget MainWindow::OnEqSavePresetClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    auto strong = get_strong();
    ContentDialog dialog;
    ui::placeDialog(dialog, ContentLayout().XamlRoot());
    dialog.Title(box_value(tr(L"Eq_SaveTitle")));
    TextBox name;
    name.Header(box_value(tr(L"Eq_PresetName")));
    name.Text(ui::utf8(settings_.eq.preset));
    name.SelectAll();
    dialog.Content(name);
    dialog.PrimaryButtonText(tr(L"Eq_Save"));
    dialog.CloseButtonText(tr(L"Reset_Cancel"));
    dialog.DefaultButton(ContentDialogButton::Primary);
    if (co_await ui::showDialog(dialog) != ContentDialogResult::Primary) co_return;
    std::wstring text(name.Text());
    text.erase(0, text.find_first_not_of(L" \t"));
    text.erase(text.find_last_not_of(L" \t") + 1);
    if (text.empty()) co_return;
    const auto utf8 = camel::toUtf8(text);
    auto& presets = settings_.eq.presets;
    const auto found = std::ranges::find(presets, utf8, &camel::EqPreset::name);
    if (found != presets.end())
        found->bands = eqEditor_->settings().bands;
    else
        presets.push_back({utf8, eqEditor_->settings().bands});
    settings_.eq.preset = utf8;
    showEq();
    settingsChanged();
}

void MainWindow::OnEqDeletePresetClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    const int index = EqPresetCombo().SelectedIndex();
    if (index < 0 || size_t(index) >= settings_.eq.presets.size()) return;
    settings_.eq.presets.erase(settings_.eq.presets.begin() + index);
    settings_.eq.preset.clear();
    showEq();
    settingsChanged();
}

// Back to the 5 flat bands a new EQ starts with; saved presets stay.
void MainWindow::OnEqResetClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    settings_.eq.bands = camel::defaultEqBands();
    settings_.eq.preset.clear();
    showEq();
    settingsChanged();
}

// Equalizer APO and AutoEq ParametricEQ.txt: the filters become a preset named after the file, and the EQ turns on.
void MainWindow::OnEqImportClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    constexpr COMDLG_FILTERSPEC types[] = {{L"Equalizer APO / AutoEq", L"*.txt"}};
    const auto chosen = ui::pickFile(Microsoft::UI::GetWindowFromWindowId(AppWindow().Id()), types);
    if (!chosen) return;
    const std::filesystem::path& file = *chosen;
    std::ifstream in(file, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    camel::EqImportNotes notes;
    const auto bands = camel::parseEqualizerApo(text, &notes);
    if (bands.empty()) {
        ui::showDialog(textDialog(ContentLayout().XamlRoot(), tr(L"Eq_ImportTitle"), tr(L"Eq_ImportFailed")));
        return;
    }
    const auto name = camel::toUtf8(file.stem().wstring());
    auto& presets = settings_.eq.presets;
    const auto found = std::ranges::find(presets, name, &camel::EqPreset::name);
    if (found != presets.end())
        found->bands = bands;
    else
        presets.push_back({name, bands});
    settings_.eq.bands = bands;
    settings_.eq.preset = name;
    settings_.eq.enabled = true;
    showEq();
    settingsChanged();
    const auto lines = [](const std::vector<int>& numbers) {
        std::wstring text;
        for (const int number : numbers) text += (text.empty() ? L"" : L", ") + std::to_wstring(number);
        return text;
    };
    std::wstring changes;
    if (!notes.held.empty()) changes += ui::format(L"Eq_ImportHeld", {lines(notes.held)}).c_str();
    if (!notes.dropped.empty())
        changes += (changes.empty() ? L"" : L"\n\n") + std::wstring(ui::format(L"Eq_ImportDropped", {lines(notes.dropped)}));
    if (!changes.empty()) ui::showDialog(textDialog(ContentLayout().XamlRoot(), tr(L"Eq_ImportTitle"), hstring(changes)));
}

void MainWindow::OnEqExportClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    constexpr COMDLG_FILTERSPEC types[] = {{L"Equalizer APO", L"*.txt"}};
    const auto name = settings_.eq.preset.empty() ? std::wstring(L"CamelPlayer EQ") : camel::toWide(settings_.eq.preset);
    const auto chosen = ui::pickFile(Microsoft::UI::GetWindowFromWindowId(AppWindow().Id()), types, true, L"txt", name + L".txt");
    if (!chosen) return;
    const std::filesystem::path& file = *chosen;
    auto shown = settings_.eq;
    shown.enabled = true;
    const auto text = camel::formatEqualizerApo(settings_.eq.bands, analyzeDsp(eqChain(shown), eqRate()).preampDb);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(text.data(), std::streamsize(text.size()));
    if (!out) camel::log::warn("EQ export: cannot write %s", camel::toUtf8(file.wstring()).c_str());
}

void MainWindow::showCrossfade() {
    const bool wasPopulating = std::exchange(populating_, true);
    CrossfadeSwitch().IsOn(settings_.crossfade.enabled);
    CrossfadeSecondsBox().Value(settings_.crossfade.seconds);
    populating_ = wasPopulating;
    enableDependentFields();
}

// Rebuilds the signal path rows only when a step changed; snapshots arrive many times a second.
void MainWindow::showSignalPath(std::vector<PathStep> steps) {
    if (steps == shownPath_) return;
    shownPath_ = std::move(steps);
    auto grid = SignalPathGrid();
    grid.Children().Clear();
    grid.RowDefinitions().Clear();
    // With nothing playing, the verdict and the route stand alone.
    const auto shown = shownPath_.empty() ? Visibility::Collapsed : Visibility::Visible;
    PathDivider().Visibility(shown);
    grid.Visibility(shown);
    if (shownPath_.empty()) return;
    const auto resources = Application::Current().Resources();
    const auto body = resources.Lookup(box_value(L"BodyTextBlockStyle")).as<Style>();
    const auto caption = resources.Lookup(box_value(L"CaptionTextBlockStyle")).as<Style>();
    const auto secondary = themeBrush(L"TextFillColorSecondaryBrush");
    const auto rail = themeBrush(L"ControlStrongStrokeColorDefaultBrush");
    // A body line is 20 px high; the 10 px dot sits in the middle of the first one.
    constexpr double dotTop = 5;
    constexpr double dotSize = 10;
    int32_t row = 0;
    const auto place = [&](FrameworkElement const& element, int32_t column, int32_t span) {
        Grid::SetRow(element, row);
        Grid::SetColumn(element, column);
        Grid::SetColumnSpan(element, span);
        grid.Children().Append(element);
    };
    const auto addRow = [&] {
        RowDefinition definition;
        definition.Height(GridLength{1, GridUnitType::Auto});
        grid.RowDefinitions().Append(definition);
    };
    for (const auto [stage, title] : {std::pair{PathStage::Source, L"SignalPath_StageSource"},
                                      {PathStage::Processing, L"SignalPath_StageProcessing"},
                                      {PathStage::Output, L"SignalPath_StageOutput"}}) {
        addRow();
        TextBlock heading;
        heading.Style(caption);
        heading.Text(tr(title));
        heading.Foreground(secondary);
        heading.Margin({0, row == 0 ? 0.0 : 12.0, 0, 6});
        Automation::AutomationProperties::SetHeadingLevel(heading, Automation::Peers::AutomationHeadingLevel::Level2);
        place(heading, 0, 3);
        ++row;
        std::vector<const PathStep*> members;
        for (const auto& step : shownPath_)
            if (step.stage == stage) members.push_back(&step);
        // A stage without steps says so, beside a dashed rail.
        if (members.empty()) {
            addRow();
            Shapes::Line dashes;
            dashes.X1(dotSize / 2);
            dashes.X2(dotSize / 2);
            dashes.Y2(20);
            dashes.Stroke(rail);
            dashes.StrokeThickness(2);
            Media::DoubleCollection pattern;
            pattern.Append(2);
            pattern.Append(2);
            dashes.StrokeDashArray(pattern);
            TextBlock text;
            text.Style(body);
            text.Text(tr(L"SignalPath_Unchanged"));
            text.Foreground(secondary);
            text.TextWrapping(TextWrapping::Wrap);
            place(dashes, 0, 1);
            place(text, 1, 2);
            ++row;
            continue;
        }
        for (size_t i = 0; i < members.size(); ++i) {
            const auto& step = *members[i];
            const bool last = i + 1 == members.size();
            addRow();
            const wchar_t* brush = step.state == PathState::BitPerfect ? L"SystemFillColorSuccessBrush"
                                   : step.state == PathState::Lossless ? L"SystemFillColorCautionBrush"
                                   : step.state == PathState::Lossy    ? L"SystemFillColorNeutralBrush"
                                                                       : L"SystemFillColorCriticalBrush";
            const wchar_t* state = step.state == PathState::BitPerfect  ? L"Lamp_BitPerfect"
                                   : step.state == PathState::Lossless  ? L"Lamp_Lossless"
                                   : step.state == PathState::Lossy     ? L"Lamp_Lossy"
                                   : step.state == PathState::Processed ? L"Lamp_Processed"
                                                                        : L"SignalPath_StateAtTrackChanges";
            // The rail joins the dots of a stage and stops short of each dot, so a ring stays hollow.
            Grid marker;
            marker.Width(dotSize);
            if (i > 0) {
                Shapes::Rectangle above;
                above.Width(2);
                above.Height(dotTop);
                above.VerticalAlignment(VerticalAlignment::Top);
                above.Fill(rail);
                marker.Children().Append(above);
            }
            if (!last) {
                Shapes::Rectangle below;
                below.Width(2);
                below.Margin({0, dotTop + dotSize, 0, 0});
                below.Fill(rail);
                marker.Children().Append(below);
            }
            // A ring marks a step that acts only when tracks change.
            Shapes::Ellipse dot;
            dot.Width(dotSize);
            dot.Height(dotSize);
            dot.Margin({0, dotTop, 0, 0});
            dot.VerticalAlignment(VerticalAlignment::Top);
            if (step.state == PathState::AtTrackChanges) {
                dot.Stroke(themeBrush(brush));
                dot.StrokeThickness(2);
            } else {
                dot.Fill(themeBrush(brush));
            }
            ToolTipService::SetToolTip(dot, box_value(tr(state)));
            marker.Children().Append(dot);
            const Thickness gap{0, 0, 0, last ? 0.0 : 10.0};
            TextBlock name;
            name.Style(body);
            name.Text(step.name);
            name.Margin(gap);
            Automation::AutomationProperties::SetName(name, step.name + L", " + tr(state));
            TextBlock detail;
            detail.Style(body);
            detail.Text(step.detail);
            detail.Margin(gap);
            detail.TextWrapping(TextWrapping::Wrap);
            detail.IsTextSelectionEnabled(true);
            detail.Foreground(secondary);
            place(marker, 0, 1);
            place(name, 1, 1);
            place(detail, 2, 1);
            ++row;
        }
    }
}

void MainWindow::OnCrossfadeToggled(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    if (populating_) return;
    enableDependentFields();
    settingsChanged();
}

void MainWindow::OnCrossfadeSecondsChanged(NumberBox const&, NumberBoxValueChangedEventArgs const&) {
    if (populating_) return;
    settingsChanged();
    // A typed length shows as the half second it is kept at.
    showCrossfade();
}

// The DSP card (docs/Plan.md §5.9): every node from settings_, the order rows and the chain's headroom.
void MainWindow::showDsp() {
    const bool wasPopulating = std::exchange(populating_, true);
    CrossfeedSwitch().IsOn(settings_.crossfeed.enabled);
    CrossfeedPresetCombo().SelectedIndex(int(settings_.crossfeed.preset));
    const auto [cutoff, feed] = camel::crossfeedValues(settings_.crossfeed);
    CrossfeedCutoffBox().Value(cutoff);
    CrossfeedFeedBox().Value(feed);
    const bool custom = settings_.crossfeed.preset == camel::CrossfeedPreset::Custom;
    CrossfeedCutoffBox().IsEnabled(custom);
    CrossfeedFeedBox().IsEnabled(custom);
    const bool hasFile = !settings_.convolution.file.empty();
    ConvolutionSwitch().IsOn(settings_.convolution.enabled && hasFile);
    ConvolutionSwitch().IsEnabled(hasFile);
    ConvolutionRemoveButton().IsEnabled(hasFile);
    ConvolutionFileText().Text(hasFile ? ui::utf8(settings_.convolution.file) : tr(L"Convolution_NoFile"));
    populating_ = wasPopulating;
    showDspOrder();
    showEq();
    if (settings_.convolution.file != impulseFile_ || eqRate() != impulseRate_) loadConvolution();
}

void MainWindow::showDspOrder() {
    auto rows = DspOrderList().Children();
    rows.Clear();
    const auto& order = settings_.dspOrder;
    for (size_t i = 0; i < order.size(); ++i) {
        Grid row;
        row.ColumnSpacing(8);
        for (const auto width : {GridLengthHelper::FromPixels(24), GridLengthHelper::FromPixels(200),
                                 GridLength{1, GridUnitType::Auto}, GridLength{1, GridUnitType::Auto}}) {
            ColumnDefinition column;
            column.Width(width);
            row.ColumnDefinitions().Append(column);
        }
        TextBlock number;
        number.Text(to_hstring(i + 1) + L".");
        number.VerticalAlignment(VerticalAlignment::Center);
        row.Children().Append(number);
        TextBlock name;
        name.Text(tr(dspNodeKey(order[i])));
        name.VerticalAlignment(VerticalAlignment::Center);
        Grid::SetColumn(name, 1);
        row.Children().Append(name);
        const auto move = [&](const wchar_t* glyph, const wchar_t* key, size_t to, int column) {
            Button button;
            button.Padding(ThicknessHelper::FromUniformLength(6));
            FontIcon icon;
            icon.Glyph(glyph);
            icon.FontSize(12);
            button.Content(icon);
            button.IsEnabled(to < order.size());
            ToolTipService::SetToolTip(button, box_value(tr(key)));
            Automation::AutomationProperties::SetName(button, tr(key) + L" " + name.Text());
            button.Click([this, from = i, to](auto&&, auto&&) {
                std::swap(settings_.dspOrder[from], settings_.dspOrder[to]);
                // The row's own click is still being handled; rebuild once it returns.
                DispatcherQueue().TryEnqueue([this] { showDspOrder(); });
                showDspSummary();
                settingsChanged();
            });
            Grid::SetColumn(button, column);
            row.Children().Append(button);
        };
        move(L"\xE70E", L"Dsp_MoveUp", i - 1, 2); // wraps past the end for the first row, which disables it
        move(L"\xE70D", L"Dsp_MoveDown", i + 1, 3);
        rows.Append(row);
    }
    DspRestoreOrderButton().IsEnabled(order != camel::audio::kDefaultDspOrder);
}

void MainWindow::showDspSummary() {
    const uint32_t rate = eqRate();
    const auto chain = camel::audio::dspChain(settings_.eq, settings_.crossfeed, settings_.convolution, settings_.dspOrder,
                                              impulseRate_ == rate ? impulse_ : nullptr);
    const bool anyOn = settings_.eq.enabled || settings_.crossfeed.enabled ||
                       (settings_.convolution.enabled && !settings_.convolution.file.empty());
    const auto analysis = camel::audio::analyzeDsp(chain, rate);
    // The software volume and ReplayGain change the level whatever the chain does.
    const bool levelled = camel::volumeDb(settings_.volume) != 0.0 || settings_.replayGain.mode != camel::ReplayGainMode::Off;
    if (!anyOn) {
        DspSummaryText().Text(tr(levelled ? L"Dsp_OffLevelled" : L"Dsp_Off"));
    } else if (!analysis.active) {
        DspSummaryText().Text(tr(levelled ? L"Dsp_FlatLevelled" : L"Dsp_Flat"));
    } else {
        wchar_t db[16];
        swprintf_s(db, L"%.1f", -analysis.preampDb);
        DspSummaryText().Text(ui::format(L"Dsp_Headroom", {db}));
    }
}

// Reads the impulse response for the playing rate off the UI thread, then shows what it holds.
winrt::fire_and_forget MainWindow::loadConvolution() {
    const auto file = settings_.convolution.file;
    const uint32_t rate = eqRate();
    const uint64_t load = ++impulseLoads_;
    impulse_ = nullptr;
    impulseFile_ = file;
    impulseRate_ = rate;
    ConvolutionInfoText().Text(file.empty() ? hstring{} : tr(L"Convolution_Loading"));
    showDspSummary();
    if (file.empty()) co_return;
    auto weak = get_weak();
    auto queue = DispatcherQueue();
    const auto server = server_;
    co_await winrt::resume_background();
    camel::audio::LoadedImpulse loaded;
    std::optional<camel::audio::ImpulseProblem> problem;
    try {
        loaded = camel::audio::loadImpulse(server->readableFile(file), rate);
    } catch (const camel::audio::ImpulseError& e) {
        problem = e.problem();
        camel::log::warn("impulse response: %s", e.what());
    }
    queue.TryEnqueue([weak, load, rate, loaded, problem] {
        auto self = weak.get();
        if (!self || load != self->impulseLoads_) return;
        self->impulse_ = loaded.impulse;
        self->ConvolutionInfoText().Text(problem ? impulseProblemText(*problem) : impulseInfoText(loaded.info, rate));
        self->showDspSummary();
    });
}

void MainWindow::OnDspRestoreOrderClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    settings_.dspOrder = camel::audio::kDefaultDspOrder;
    showDspOrder();
    showDspSummary();
    settingsChanged();
}

void MainWindow::OnCrossfeedToggled(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    if (populating_) return;
    settings_.crossfeed.enabled = CrossfeedSwitch().IsOn();
    showDspSummary();
    settingsChanged();
}

void MainWindow::OnCrossfeedPresetChanged(Windows::Foundation::IInspectable const&, SelectionChangedEventArgs const&) {
    const int index = CrossfeedPresetCombo().SelectedIndex();
    if (populating_ || index < 0) return;
    const auto preset = camel::CrossfeedPreset(index);
    if (preset == camel::CrossfeedPreset::Custom && settings_.crossfeed.preset != preset) {
        // Custom starts from the preset it replaces.
        std::tie(settings_.crossfeed.cutoffHz, settings_.crossfeed.feedDb) = camel::crossfeedValues(settings_.crossfeed);
    }
    settings_.crossfeed.preset = preset;
    showDsp();
    settingsChanged();
}

void MainWindow::OnCrossfeedValueChanged(NumberBox const&, NumberBoxValueChangedEventArgs const&) {
    const double cutoff = CrossfeedCutoffBox().Value(), feed = CrossfeedFeedBox().Value();
    if (populating_ || std::isnan(cutoff) || std::isnan(feed)) return;
    settings_.crossfeed.cutoffHz = uint32_t(std::clamp(std::lround(cutoff), long(camel::CrossfeedSettings::kMinCutoffHz),
                                                       long(camel::CrossfeedSettings::kMaxCutoffHz)));
    settings_.crossfeed.feedDb =
        std::clamp(std::round(feed * 10) / 10, camel::CrossfeedSettings::kMinFeedDb, camel::CrossfeedSettings::kMaxFeedDb);
    showDspSummary();
    settingsChanged();
}

void MainWindow::OnConvolutionToggled(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    if (populating_) return;
    settings_.convolution.enabled = ConvolutionSwitch().IsOn();
    showDspSummary();
    settingsChanged();
}

// A Control point sends the file to the Server, which keeps it in its data folder (docs/Plan.md §17.8).
winrt::fire_and_forget MainWindow::OnConvolutionChooseClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    auto strong = get_strong();
    const hstring types = tr(L"Convolution_FileTypes");
    const COMDLG_FILTERSPEC filter[] = {{types.c_str(), L"*.wav;*.zip"}};
    const auto chosen = ui::pickFile(Microsoft::UI::GetWindowFromWindowId(AppWindow().Id()), filter);
    if (!chosen) co_return;
    const auto server = server_;
    const auto& file = *chosen; // in this coroutine's frame, so it outlives the co_await
    winrt::apartment_context foreground;
    ConvolutionInfoText().Text(tr(L"Convolution_Loading"));
    co_await winrt::resume_background();
    std::string kept;
    try {
        kept = server->keepFile(file);
    } catch (const std::exception& e) {
        camel::log::warn("impulse response: %s", e.what());
    }
    co_await foreground;
    if (kept.empty()) {
        ConvolutionInfoText().Text(tr(L"Convolution_NotSent"));
        co_return;
    }
    settings_.convolution.file = kept;
    settings_.convolution.enabled = true;
    showDsp();
    settingsChanged();
}

void MainWindow::OnConvolutionRemoveClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    settings_.convolution = {};
    showDsp();
    settingsChanged();
}

// The playback-bar DSP button: each node on or off, the EQ presets, and the DSP settings.
void MainWindow::OnDspMenuOpening(Windows::Foundation::IInspectable const&, Windows::Foundation::IInspectable const&) {
    auto items = DspMenu().Items();
    items.Clear();
    // Not a command, so not enabled, but read as text: the disabled colour is about 3:1.
    MenuFlyoutItem forOutput;
    forOutput.Text(ui::format(L"Dsp_MenuForOutput", {std::wstring(dspOutputLabel_)}));
    forOutput.IsEnabled(false);
    forOutput.Resources().Insert(box_value(L"MenuFlyoutItemForegroundDisabled"), themeBrush(L"TextFillColorSecondaryBrush"));
    items.Append(forOutput);
    items.Append(MenuFlyoutSeparator());
    const auto toggle = [&](camel::audio::DspNode node, bool on, bool available, auto flip) {
        ToggleMenuFlyoutItem item;
        item.Text(tr(dspNodeKey(node)));
        item.IsChecked(on);
        item.IsEnabled(available);
        item.Click([this, flip](auto&&, auto&&) {
            flip();
            showDsp();
            settingsChanged();
        });
        items.Append(item);
    };
    for (const auto node : settings_.dspOrder) {
        switch (node) {
        case camel::audio::DspNode::Eq:
            toggle(node, settings_.eq.enabled, true, [this] { settings_.eq.enabled = !settings_.eq.enabled; });
            break;
        case camel::audio::DspNode::Crossfeed:
            toggle(node, settings_.crossfeed.enabled, true,
                   [this] { settings_.crossfeed.enabled = !settings_.crossfeed.enabled; });
            break;
        case camel::audio::DspNode::Convolution:
            toggle(node, settings_.convolution.enabled && !settings_.convolution.file.empty(),
                   !settings_.convolution.file.empty(),
                   [this] { settings_.convolution.enabled = !settings_.convolution.enabled; });
            break;
        }
    }
    MenuFlyoutSubItem replayGain;
    replayGain.Text(tr(L"Settings_ReplayGain/Text"));
    constexpr const wchar_t* kModes[] = {L"Settings_ReplayGainOff/Content", L"Settings_ReplayGainTrack/Content",
                                         L"Settings_ReplayGainAlbum/Content", L"Settings_ReplayGainAuto/Content"};
    for (int mode = 0; std::cmp_less(mode, std::size(kModes)); ++mode) {
        RadioMenuFlyoutItem item;
        item.Text(tr(kModes[mode]));
        item.GroupName(L"ReplayGainModes");
        item.IsChecked(int(settings_.replayGain.mode) == mode);
        item.Click([this, mode](auto&&, auto&&) {
            settings_.replayGain.mode = camel::ReplayGainMode(mode);
            showReplayGain();
            settingsChanged();
        });
        replayGain.Items().Append(item);
    }
    ToggleMenuFlyoutItem crossfade;
    crossfade.Text(tr(L"Settings_Crossfade/Text"));
    crossfade.IsChecked(settings_.crossfade.enabled);
    crossfade.Click([this](auto&&, auto&&) {
        settings_.crossfade.enabled = !settings_.crossfade.enabled;
        showCrossfade();
        settingsChanged();
    });
    items.Append(MenuFlyoutSeparator());
    items.Append(crossfade);
    items.Append(replayGain);
    if (!settings_.eq.presets.empty()) {
        MenuFlyoutSubItem presets;
        presets.Text(tr(L"Dsp_MenuEqPresets"));
        for (const auto& preset : settings_.eq.presets) {
            RadioMenuFlyoutItem item;
            item.Text(ui::utf8(preset.name));
            item.GroupName(L"EqPresets");
            item.IsChecked(preset.name == settings_.eq.preset);
            item.Click([this, name = preset.name](auto&&, auto&&) {
                const auto found = std::ranges::find(settings_.eq.presets, name, &camel::EqPreset::name);
                if (found == settings_.eq.presets.end()) return;
                settings_.eq.bands = found->bands;
                settings_.eq.preset = name;
                showDsp();
                settingsChanged();
            });
            presets.Items().Append(item);
        }
        items.Append(MenuFlyoutSeparator());
        items.Append(presets);
    }
    items.Append(MenuFlyoutSeparator());
    MenuFlyoutItem open;
    open.Text(tr(L"Dsp_MenuSettings"));
    open.Click([this](auto&&, auto&&) { showSettingsCard(DspCard()); });
    items.Append(open);
}

} // namespace winrt::CamelPlayer::implementation
