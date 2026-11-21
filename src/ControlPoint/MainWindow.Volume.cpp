// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pch.h"

#include "MainWindow.xaml.h"
#include "MainWindowParts.h"

#include <cmath>
#include <utility>

#include "UiText.h"
#include "Common/settings/Settings.h"

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

constexpr auto kPlayerVolumeSettles = std::chrono::milliseconds(2000);

} // namespace

// Software volume (docs/Plan.md §5.9): the mode on the DSP card, and the playback bar's volume button, which opens the
// slider and mute while it is Software and says who sets the level while it is Fixed.
void MainWindow::showVolume() {
    const bool wasPopulating = std::exchange(populating_, true);
    const bool network = camel::isNetworkPlayer(settings_.backend);
    const bool hqplayer = settings_.backend == Backend::HQPlayer;
    const bool software = settings_.volume.mode == camel::VolumeMode::Software;
    const bool wantsPlayer = settings_.volume.mode == camel::VolumeMode::Player;
    const bool player = wantsPlayer && playerVolume_.has_value();
    VolumeModeOwnItem().Content(box_value(tr(network ? L"Volume_ModePlayer" : L"Volume_ModeSoftware")));
    VolumeModeHQPlayerItem().Visibility(hqplayer ? Visibility::Visible : Visibility::Collapsed);
    VolumeModeCombo().SelectedIndex(software ? 1 : wantsPlayer ? (hqplayer ? 2 : 1) : 0);
    VolumeNote().Text(tr(settings_.backend == Backend::AirPlay ? L"Volume_NoteAirPlay"
                         : hqplayer                            ? L"Volume_NoteHQPlayer"
                         : network                             ? L"Volume_NotePlayer"
                                                               : L"Volume_NoteLocal"));
    FixedVolumePanel().Visibility(software || player ? Visibility::Collapsed : Visibility::Visible);
    SoftwareVolumePanel().Visibility(software ? Visibility::Visible : Visibility::Collapsed);
    PlayerVolumePanel().Visibility(player ? Visibility::Visible : Visibility::Collapsed);
    VolumeLock().Visibility(software || player ? Visibility::Collapsed : Visibility::Visible);
    FixedVolumeTitle().Text(tr(!wantsPlayer ? L"Volume_Fixed" : hqplayer ? L"Volume_HQPlayerUnknown" : L"Volume_PlayerUnknown"));
    VolumeSlider().Value(settings_.volume.db);
    if (player && std::chrono::steady_clock::now() - playerVolumeMovedAt_ > kPlayerVolumeSettles)
        PlayerVolumeSlider().Value(*playerVolume_);
    PlayerMuteIcon().Glyph(playerMuted_ ? L"\xE74F" : L"\xE767");
    const auto playerMute = tr(playerMuted_ ? L"Volume_Unmute" : L"Volume_Mute");
    ToolTipService::SetToolTip(PlayerMuteButton(), box_value(playerMute));
    Automation::AutomationProperties::SetName(PlayerMuteButton(), playerMute);
    MuteIcon().Glyph(settings_.volume.muted ? L"\xE74F" : L"\xE767");
    const auto mute = tr(settings_.volume.muted ? L"Volume_Unmute" : L"Volume_Mute");
    ToolTipService::SetToolTip(MuteButton(), box_value(mute));
    Automation::AutomationProperties::SetName(MuteButton(), mute);
    populating_ = wasPopulating;
    showVolumeLevel();
    showDspSummary();
}

// The volume button's icon, name and the level above the slider follow the level as it moves.
void MainWindow::showVolumeLevel() {
    const auto& volume = settings_.volume;
    wchar_t level[32];
    swprintf_s(level, L"%.1f dB", volume.db);
    VolumeText().Text(level);
    hstring name;
    if (volume.mode == camel::VolumeMode::Player && playerVolume_) {
        const auto shown = uint32_t(PlayerVolumeSlider().Value());
        const auto text = playerVolumeDb_ ? decibels(*playerVolumeDb_) : to_hstring(shown); // HQPlayer's in dB
        PlayerVolumeText().Text(text);
        VolumeIcon().Glyph(playerMuted_ ? L"\xE74F" : shown > 66 ? L"\xE995" : shown > 33 ? L"\xE994" : L"\xE993");
        name = ui::format(L"Volume_Level", {playerMuted_ ? std::wstring(tr(L"SignalPath_Muted")) : std::wstring(text)});
    } else if (volume.mode != camel::VolumeMode::Software) {
        VolumeIcon().Glyph(L"\xE767");
        name = tr(L"Volume_Fixed");
    } else {
        VolumeIcon().Glyph(volume.muted ? L"\xE74F" : volume.db > -10 ? L"\xE995" : volume.db > -30 ? L"\xE994" : L"\xE993");
        name = ui::format(L"Volume_Level", {volume.muted ? std::wstring(tr(L"SignalPath_Muted")) : std::wstring(level)});
    }
    setBadgeTip(VolumeButton(), name, name);
}

void MainWindow::OnVolumeSettingsClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    VolumeFlyout().Hide();
    showSettingsCard(VolumeCard());
}

void MainWindow::showReplayGain() {
    const bool wasPopulating = std::exchange(populating_, true);
    const auto& rg = settings_.replayGain;
    ReplayGainModeCombo().SelectedIndex(int(rg.mode));
    ReplayGainPreampBox().Value(rg.preampDb);
    ReplayGainNoValueBox().Value(rg.noValuePreampDb);
    ReplayGainClipSwitch().IsOn(rg.preventClipping);
    populating_ = wasPopulating;
    enableDependentFields();
    showLoudnessProgress();
    showDspSummary();
}

// A feature's own fields are disabled while it is off, so what depends on what shows.
void MainWindow::enableDependentFields() {
    const bool replayGain = ReplayGainModeCombo().SelectedIndex() > 0;
    for (const Control& control :
         {Control(ReplayGainPreampBox()), Control(ReplayGainNoValueBox()), Control(ReplayGainClipSwitch())})
        control.IsEnabled(replayGain);
    CrossfadeSecondsBox().IsEnabled(CrossfadeSwitch().IsOn());
}

void MainWindow::showLoudnessProgress() {
    if (!server_ || SettingsPanel().Visibility() != Visibility::Visible) return;
    const auto progress = server_->loudnessProgress();
    std::wstring text;
    if (progress.total > 0) {
        text = ui::format(L"Loudness_Progress", {ui::count(progress.analyzed), ui::count(progress.total)});
        // A sentence after a full-width full stop needs no space.
        const std::wstring separator = text.ends_with(L'。') ? L"" : L" ";
        if (settings_.replayGain.mode == camel::ReplayGainMode::Off && progress.analyzed < progress.total)
            text += separator + std::wstring(tr(L"Loudness_Off"));
        else if (progress.running)
            text += separator + std::wstring(tr(L"Loudness_Analyzing"));
    }
    if (LoudnessProgressText().Text() != text) LoudnessProgressText().Text(text);
}

camel::VolumeMode MainWindow::chosenVolumeMode() {
    const int chosen = VolumeModeCombo().SelectedIndex();
    if (chosen == 2) return camel::VolumeMode::Player; // HQPlayer's
    if (chosen != 1) return camel::VolumeMode::Fixed;
    return camel::isNetworkPlayer(settings_.backend) ? camel::VolumeMode::Player : camel::VolumeMode::Software;
}

void MainWindow::OnVolumeModeChanged(Windows::Foundation::IInspectable const&, SelectionChangedEventArgs const&) {
    if (populating_) return;
    settings_.volume.mode = chosenVolumeMode();
    showVolume();
    settingsChanged();
}

void MainWindow::OnVolumeChanged(Windows::Foundation::IInspectable const&, Primitives::RangeBaseValueChangedEventArgs const& e) {
    if (populating_) return;
    settings_.volume.db = std::round(e.NewValue() * 2) / 2;
    showVolumeLevel();
    showDspSummary();
    // A slider being dragged applies at most every 100 ms.
    if (!eqTimer_.IsRunning()) eqTimer_.Start();
}

void MainWindow::OnVolumeWheel(Windows::Foundation::IInspectable const&, Input::PointerRoutedEventArgs const& e) {
    const bool player = settings_.volume.mode == camel::VolumeMode::Player && playerVolume_;
    if (settings_.volume.mode != camel::VolumeMode::Software && !player) return;
    // A notch moves 1 dB; one event may carry several notches, and a touchpad sends parts of one.
    volumeWheel_ += e.GetCurrentPoint(nullptr).Properties().MouseWheelDelta();
    const int notches = volumeWheel_ / WHEEL_DELTA;
    volumeWheel_ %= WHEEL_DELTA;
    if (notches != 0 && player)
        PlayerVolumeSlider().Value(std::clamp(PlayerVolumeSlider().Value() + notches, 0.0, 100.0));
    else if (notches != 0)
        VolumeSlider().Value(std::clamp(VolumeSlider().Value() + notches, camel::VolumeSettings::kMinDb, 0.0));
    e.Handled(true);
}

// Only a move of the user's reaches the player; what it reports by itself is shown with `populating_` set.
void MainWindow::OnPlayerVolumeChanged(Windows::Foundation::IInspectable const&,
                                       Primitives::RangeBaseValueChangedEventArgs const&) {
    if (populating_ || !server_) return;
    showVolumeLevel();
    playerVolumeMovedAt_ = std::chrono::steady_clock::now();
    Command command;
    command.kind = CommandKind::SetPlayerVolume;
    command.playerVolume = uint32_t(PlayerVolumeSlider().Value());
    submitCommand(std::move(command));
}

// Under the group's slider, one for each receiver of an AirPlay group; a slider the user moves stays where it was left
// until the level settles.
void MainWindow::showReceiverVolumes() {
    const bool group = receivers_.size() > 1;
    ReceiverVolumes().Visibility(group ? Visibility::Visible : Visibility::Collapsed);
    auto rows = ReceiverVolumes().Children();
    const bool same = group && rows.Size() == receivers_.size() &&
                      std::ranges::all_of(std::views::iota(size_t{0}, receivers_.size()), [&](size_t i) {
                          return unbox_value_or<hstring>(rows.GetAt(uint32_t(i)).as<FrameworkElement>().Tag(), L"") ==
                                 ui::utf8(receivers_[i].id);
                      });
    if (!same) rows.Clear();
    if (!group) return;
    const auto now = std::chrono::steady_clock::now();
    for (size_t i = 0; i < receivers_.size(); ++i) {
        const auto& receiver = receivers_[i];
        if (same) {
            const auto moved = receiverMovedAt_.find(receiver.id);
            if (moved != receiverMovedAt_.end() && now - moved->second < kPlayerVolumeSettles) continue;
            const auto slider = rows.GetAt(uint32_t(i)).as<StackPanel>().Children().GetAt(1).as<Slider>();
            const bool wasPopulating = std::exchange(populating_, true);
            slider.Value(receiver.level);
            populating_ = wasPopulating;
            continue;
        }
        StackPanel row;
        row.Tag(box_value(ui::utf8(receiver.id)));
        TextBlock name;
        name.Text(ui::utf8(receiver.name));
        name.Style(unbox_value<Microsoft::UI::Xaml::Style>(
            Application::Current().Resources().Lookup(box_value(L"CaptionTextBlockStyle"))));
        Slider slider;
        slider.Width(160);
        slider.Minimum(0);
        slider.Maximum(100);
        slider.StepFrequency(1);
        slider.Value(receiver.level);
        Automation::AutomationProperties::SetName(slider, ui::format(L"Volume_ReceiverSlider", {std::wstring(name.Text())}));
        slider.ValueChanged([this, id = receiver.id](auto&&, Primitives::RangeBaseValueChangedEventArgs const& e) {
            if (populating_ || !server_) return;
            receiverMovedAt_[id] = std::chrono::steady_clock::now();
            Command command;
            command.kind = CommandKind::SetReceiverVolume;
            command.receiver = id;
            command.playerVolume = uint32_t(e.NewValue());
            submitCommand(std::move(command));
        });
        row.Children().Append(name);
        row.Children().Append(slider);
        rows.Append(row);
    }
}

void MainWindow::OnPlayerMuteClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    if (!server_) return;
    Command command;
    command.kind = CommandKind::SetPlayerMute;
    command.playerMute = !playerMuted_;
    submitCommand(std::move(command));
}

void MainWindow::OnMuteClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    settings_.volume.muted = !settings_.volume.muted;
    showVolume();
    settingsChanged();
}

void MainWindow::OnReplayGainChanged(Windows::Foundation::IInspectable const&, SelectionChangedEventArgs const&) {
    if (populating_) return;
    enableDependentFields();
    settingsChanged();
    showLoudnessProgress();
    showDspSummary();
}

void MainWindow::OnReplayGainValueChanged(NumberBox const&, NumberBoxValueChangedEventArgs const&) {
    if (populating_) return;
    settingsChanged();
}

void MainWindow::OnReplayGainToggled(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    if (populating_) return;
    settingsChanged();
}

} // namespace winrt::CamelPlayer::implementation
