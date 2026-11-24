// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pch.h"

#include "MainWindow.xaml.h"
#include "MainWindowParts.h"

#include <filesystem>
#include <utility>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>

#include "DialogPlacement.h"
#include "UiText.h"
#include "Common/settings/Settings.h"
#include "Common/util/Log.h"
#include "Common/util/Text.h"
#include "Server/Network/Firewall.h"
#include "Server/Network/HQPlayer.h"
#include "Server/Playback/Outputs.h"
#include "Server/Remote/RemoteServer.h"

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

hstring outputLabel(const camel::server::OutputRow& row) {
    if (!row.alias.empty()) return ui::utf8(row.alias);
    if (row.id.empty()) return tr(L"Device_Default");
    // An HQPlayer is known by its address, which its name may not tell.
    if (row.backend == Backend::HQPlayer)
        return row.name.empty() || row.name == row.id ? ui::utf8(row.id) : ui::utf8(row.name + " · " + row.id);
    return row.name.empty() ? tr(L"Device_NotConnected") : ui::utf8(row.name);
}

// The same DAC through WASAPI and through ASIO are two entries, told apart by the backend.
hstring outputMenuLabel(const camel::server::OutputRow& row) {
    hstring label = outputLabel(row);
    if (!row.alias.empty() || row.id.empty()) return label;
    return label + L" (" + ui::utf8(camel::backendName(row.backend)) + L")";
}

// A receiver that asks for a password or a code, which the user can give it.
bool pairable(const camel::server::OutputRow& row) {
    return row.unsupported == "password" || row.unsupported == "code";
}

const wchar_t* unsupportedReason(const std::string& reason) {
    if (reason == "encrypted") return L"Output_AirPlayEncrypted";
    if (reason == "password") return L"Output_AirPlayPassword";
    if (reason == "code") return L"Output_AirPlayCode";
    if (reason == "format") return L"Output_AirPlayFormat";
    if (reason == "home") return L"Output_AirPlayHomeOnly";
    return L"Output_AirPlayRefused";
}

// What a row of the output menu says: a receiver not connected, or one that refuses this computer, says so.
hstring outputMenuText(const camel::server::OutputRow& row) {
    if (!row.connected) return ui::format(L"Output_Offline", {std::wstring(outputMenuLabel(row))});
    if (pairable(row)) return ui::format(L"Output_MenuPairing", {std::wstring(outputMenuLabel(row))});
    if (!row.unsupported.empty()) return ui::format(L"Output_Unsupported", {std::wstring(outputMenuLabel(row))});
    return outputMenuLabel(row);
}

// What an output's DSP has on: its nodes in order, then ReplayGain.
hstring dspSummary(const camel::server::SettingsDto& p) {
    std::wstring text;
    for (const auto node : p.dspOrder) {
        const bool on = node == DspNode::Eq          ? p.eq.enabled
                        : node == DspNode::Crossfeed ? p.crossfeed.enabled
                                                     : p.convolution.enabled && !p.convolution.file.empty();
        if (!on) continue;
        if (!text.empty()) text += L" → ";
        text += tr(dspNodeKey(node));
    }
    if (p.replayGain.mode != camel::ReplayGainMode::Off) {
        constexpr const wchar_t* kModes[] = {L"Settings_ReplayGainOff/Content", L"Settings_ReplayGainTrack/Content",
                                             L"Settings_ReplayGainAlbum/Content", L"Settings_ReplayGainAuto/Content"};
        if (!text.empty()) text += L" · ";
        text += ui::format(L"Output_DspReplayGain", {std::wstring(tr(kModes[int(p.replayGain.mode)]))});
    }
    return text.empty() ? tr(L"Output_DspNone") : hstring(text);
}

// What kind of device a zone plays to: a local one, a network player, AirPlay or HQPlayer.
const wchar_t* zoneGlyph(Backend backend) {
    switch (backend) {
    case Backend::Network:
        return L"\xE839";
    case Backend::AirPlay:
        return L"\xEC15";
    case Backend::HQPlayer:
        return L"\xE9E9";
    default:
        return L"\xE7F5";
    }
}

FontIcon glyph(const wchar_t* code) {
    FontIcon icon;
    icon.Glyph(code);
    icon.FontSize(16);
    return icon;
}

// A row of the zone picker, laid out as a menu item: a mark or an icon, its text and what follows it.
Button pickerRow(FrameworkElement const& lead, TextBlock const& text, FrameworkElement const& trail) {
    Grid face;
    face.ColumnSpacing(12);
    for (const auto width : {GridLengthHelper::FromPixels(16), GridLengthHelper::FromValueAndType(1, GridUnitType::Star),
                             GridLengthHelper::Auto()}) {
        ColumnDefinition column;
        column.Width(width);
        face.ColumnDefinitions().Append(column);
    }
    if (lead) {
        lead.VerticalAlignment(VerticalAlignment::Center);
        face.Children().Append(lead);
    }
    text.TextTrimming(TextTrimming::CharacterEllipsis);
    text.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(text, 1);
    face.Children().Append(text);
    if (trail) {
        trail.VerticalAlignment(VerticalAlignment::Center);
        Grid::SetColumn(trail, 2);
        face.Children().Append(trail);
    }
    Button row;
    row.Content(face);
    row.HorizontalAlignment(HorizontalAlignment::Stretch);
    row.HorizontalContentAlignment(HorizontalAlignment::Stretch);
    row.BorderThickness(ThicknessHelper::FromUniformLength(0));
    row.Padding(ThicknessHelper::FromLengths(11, 8, 11, 9));
    return row;
}

} // namespace

// The zone button says which zone the window follows, an AirPlay group by its receivers.
void MainWindow::nameZoneButton(const Snapshot& snapshot) {
    const auto rows = camel::server::outputRows(nativeSettings(settings_), snapshot.devices);
    const auto inUse = std::ranges::find_if(rows, &camel::server::OutputRow::selected);
    if (inUse == rows.end()) return;
    const auto zone = std::ranges::find(snapshot.zones, snapshot.selectedZone, &ZoneSnapshot::id);
    const hstring label = zone != snapshot.zones.end() && zone->playback.receivers.size() > 1
                              ? ui::utf8(zone->name) + L" (AirPlay)"
                              : outputMenuLabel(*inUse);
    ToolTipService::SetToolTip(OutputButton(), box_value(label));
    OutputIcon().Glyph(zoneGlyph(inUse->backend));
    OutputName().Text(label);
    Automation::AutomationProperties::SetName(OutputButton(), ui::format(L"Output_ButtonName", {std::wstring(label)}));
}

void MainWindow::populateOutputs() {
    const auto rows = camel::server::outputRows(nativeSettings(settings_), server_->snapshot().devices);
    const auto inUse = std::ranges::find_if(rows, &camel::server::OutputRow::selected);
    if (inUse != rows.end()) {
        nameZoneButton(server_->snapshot());
        dspOutputLabel_ = outputMenuLabel(*inUse);
        const hstring forOutput = ui::format(L"Dsp_ForOutput", {std::wstring(dspOutputLabel_)});
        DspOutputText().Text(forOutput);
        VolumeOutputText().Text(forOutput);
        // Autoplay and crossfade are the selected zone's, as its output keeps them.
        const hstring forZone = ui::format(L"Zone_SettingsFor", {std::wstring(dspOutputLabel_)});
        PlaybackZoneText().Text(forZone);
        CrossfadeZoneText().Text(forZone);
        const hstring dspName = ui::format(L"Dsp_ButtonForOutput", {std::wstring(dspOutputLabel_)});
        setBadgeTip(DspButton(), dspName, dspName);
    }
    auto list = OutputList().Children();
    list.Clear();
    StackPanel groupRows{nullptr}, hqplayerRows{nullptr};
    std::optional<Backend> group;
    const auto addGroup = [&](Backend rowGroup, const hstring& header) {
        // A group in use stays open; one the user opened or closed stays so until the program restarts.
        const bool used = std::ranges::any_of(rows, [&](const camel::server::OutputRow& r) {
            return (camel::isNetworkPlayer(r.backend) ? Backend::Network : r.backend) == rowGroup && (r.enabled || r.selected);
        });
        const auto remembered = outputGroupsOpen_.find(rowGroup);
        Expander expander;
        expander.Header(box_value(header));
        expander.HorizontalAlignment(HorizontalAlignment::Stretch);
        expander.HorizontalContentAlignment(HorizontalAlignment::Stretch);
        expander.IsExpanded(remembered == outputGroupsOpen_.end() ? used : remembered->second);
        expander.Expanding([this, rowGroup](auto&&, auto&&) { outputGroupsOpen_[rowGroup] = true; });
        expander.Collapsed([this, rowGroup](auto&&, auto&&) { outputGroupsOpen_[rowGroup] = false; });
        StackPanel panel;
        panel.Spacing(4);
        expander.Content(panel);
        list.Append(expander);
        return panel;
    };
    for (const auto& row : rows) {
        const bool player = camel::isNetworkPlayer(row.backend);
        if (const Backend rowGroup = player ? Backend::Network : row.backend; rowGroup != group) {
            group = rowGroup;
            groupRows = addGroup(rowGroup, player ? tr(L"Output_NetworkGroup") : ui::utf8(camel::backendName(row.backend)));
            if (rowGroup == Backend::HQPlayer) hqplayerRows = groupRows;
        }
        const hstring label = row.backend == Backend::AirPlay ? outputMenuLabel(row) : outputLabel(row);
        Grid line;
        line.ColumnSpacing(8);
        for (const GridLength width : {GridLength{1, GridUnitType::Star}, GridLength{1, GridUnitType::Auto},
                                       GridLength{1, GridUnitType::Auto}, GridLength{1, GridUnitType::Auto}}) {
            ColumnDefinition column;
            column.Width(width);
            line.ColumnDefinitions().Append(column);
        }
        if (row.selected) { // the output in use stands out of a long list
            line.Background(themeBrush(L"SubtleFillColorSecondaryBrush"));
            line.CornerRadius(CornerRadiusHelper::FromUniformRadius(4));
            line.Padding(ThicknessHelper::FromLengths(8, 4, 0, 4));
        }
        StackPanel names;
        names.VerticalAlignment(VerticalAlignment::Center);
        TextBlock name;
        name.Text(label);
        name.TextTrimming(TextTrimming::CharacterEllipsis);
        if (row.selected) name.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
        names.Children().Append(name);
        if (row.selected || !row.connected || !row.unsupported.empty()) {
            TextBlock state;
            // One in use that its receiver now refuses says why, as its stream ends.
            state.Text(row.selected && !row.connected
                           ? ui::format(L"Output_InUseUnsupported", {std::wstring(tr(L"Device_NotConnected"))})
                       : row.selected && !row.unsupported.empty()
                           ? ui::format(L"Output_InUseUnsupported", {std::wstring(tr(unsupportedReason(row.unsupported)))})
                           : tr(row.selected     ? L"Output_InUse"
                                : !row.connected ? L"Device_NotConnected"
                                                 : unsupportedReason(row.unsupported)));
            state.TextWrapping(TextWrapping::Wrap);
            state.Style(unbox_value<Microsoft::UI::Xaml::Style>(
                Application::Current().Resources().Lookup(box_value(L"CaptionTextBlockStyle"))));
            names.Children().Append(state);
        }
        line.Children().Append(names);

        MenuFlyout actions;
        MenuFlyoutItem rename;
        rename.Text(tr(L"Output_Rename"));
        rename.Click([this, row, label](auto&&, auto&&) { askRenameOutput(row.backend, row.id, row.alias, label); });
        actions.Items().Append(rename);
        if (pairable(row)) {
            MenuFlyoutItem pair;
            pair.Text(tr(L"Output_Pair"));
            pair.Click([this, row, label](auto&&, auto&&) {
                askAirPlayPairing(row, label, [this, row] {
                    if (row.selected) return;
                    changeOutputs([&](camel::Settings& s, const DeviceCatalog& devices) {
                        camel::server::setOutputEnabled(s, row.backend, row.id, true, devices);
                    });
                });
            });
            actions.Items().Append(pair);
        }
        if (row.saved) {
            MenuFlyoutItem remove;
            remove.Text(tr(L"Output_Delete"));
            remove.Click([this, row, label](auto&&, auto&&) { askDeleteOutput(row.backend, row.id, label); });
            actions.Items().Append(remove);
        }
        if (row.backend != Backend::AirPlay || !row.pair) { // a stereo pair has nothing to set
            Button gear;
            FontIcon gearIcon;
            gearIcon.Glyph(L"\xE713");
            gearIcon.FontSize(14);
            gear.Content(gearIcon);
            gear.VerticalAlignment(VerticalAlignment::Center);
            ToolTipService::SetToolTip(gear, box_value(tr(L"Output_Settings")));
            Automation::AutomationProperties::SetName(
                gear, ui::format(L"Output_ButtonOn", {std::wstring(tr(L"Output_Settings")), std::wstring(label)}));
            gear.Click([this, row, label](auto&&, auto&&) { showOutputSettings(row.backend, row.id, label); });
            Grid::SetColumn(gear, 1);
            line.Children().Append(gear);
        }
        Button more;
        FontIcon moreIcon;
        moreIcon.Glyph(L"\xE712");
        moreIcon.FontSize(14);
        more.Content(moreIcon);
        more.Flyout(actions);
        more.VerticalAlignment(VerticalAlignment::Center);
        ToolTipService::SetToolTip(more, box_value(tr(L"Output_More")));
        Automation::AutomationProperties::SetName(
            more, ui::format(L"Output_ButtonOn", {std::wstring(tr(L"Output_More")), std::wstring(label)}));
        Grid::SetColumn(more, 2);
        line.Children().Append(more);

        ToggleSwitch enabled;
        enabled.IsOn(row.enabled);
        enabled.IsEnabled(!row.selected && (row.unsupported.empty() || pairable(row))); // the output in use stays in the menu
        enabled.OnContent(box_value(L""));
        enabled.OffContent(box_value(L""));
        enabled.MinWidth(0);
        enabled.VerticalAlignment(VerticalAlignment::Center);
        ToolTipService::SetToolTip(enabled, box_value(tr(L"Output_Enabled")));
        Automation::AutomationProperties::SetName(
            enabled, ui::format(L"Output_ButtonOn", {std::wstring(tr(L"Output_Enabled")), std::wstring(label)}));
        enabled.Toggled([this, row, label](Windows::Foundation::IInspectable const& sender, auto&&) {
            const bool on = sender.as<ToggleSwitch>().IsOn(); // not the switch itself, which the handler would keep
            if (on && pairable(row)) {
                askAirPlayPairing(row, label, [this, row] {
                    changeOutputs([&](camel::Settings& s, const DeviceCatalog& devices) {
                        camel::server::setOutputEnabled(s, row.backend, row.id, true, devices);
                    });
                });
                return;
            }
            if (!on && zonePlays(row)) {
                askTurnOffOutput(row, label);
                return;
            }
            if (on) {
                auto enabledRow = row;
                enabledRow.enabled = true;
                if (camel::server::nameTaken(camel::server::outputRows(nativeSettings(settings_), server_->snapshot().devices),
                                             enabledRow)) {
                    askOutputName(row, true, {});
                    return;
                }
            }
            changeOutputs([&](camel::Settings& s, const DeviceCatalog& devices) {
                camel::server::setOutputEnabled(s, row.backend, row.id, on, devices);
            });
            if (camel::server::needsFirewallRule(row) && on) askFirewallRule();
        });
        Grid::SetColumn(enabled, 3);
        line.Children().Append(enabled);
        groupRows.Children().Append(line);
    }
    if (!hqplayerRows) hqplayerRows = addGroup(Backend::HQPlayer, ui::utf8(camel::backendName(Backend::HQPlayer)));
    HyperlinkButton addHQPlayer;
    addHQPlayer.Content(box_value(tr(L"Output_AddHQPlayer")));
    addHQPlayer.Click([this](auto&&, auto&&) { askAddHQPlayer(); });
    hqplayerRows.Children().Append(addHQPlayer);
    const bool players = std::ranges::any_of(
        rows, [](const camel::server::OutputRow& r) { return camel::server::needsFirewallRule(r) && r.enabled; });
    if (players && !firewallRule_) readFirewallRule(); // the notice opens once it is known
    FirewallNotice().IsOpen(players && firewallRule_ == false);
}

winrt::fire_and_forget MainWindow::readFirewallRule() {
    if (std::exchange(firewallReading_, true)) co_return;
    auto weak = get_weak();
    auto queue = DispatcherQueue();
    const auto server = server_;
    co_await winrt::resume_background();
    bool found = false;
    try {
        found = server->playerFirewallRule();
    } catch (const std::exception& e) {
        camel::log::warn("firewall rule: %s", e.what());
    }
    queue.TryEnqueue([weak, found] {
        const auto self = weak.get();
        if (!self) return;
        self->firewallReading_ = false;
        self->firewallRule_ = found;
        self->populateOutputs();
        if (std::exchange(self->firewallAsk_, false)) self->askFirewallRule();
    });
}

winrt::fire_and_forget MainWindow::addFirewallRule() {
    auto strong = get_strong();
    if (!server_->filesHere()) {
        co_await ui::showDialog(textDialog(ContentLayout().XamlRoot(), tr(L"Firewall_Title"),
                                           ui::format(L"Firewall_ElsewhereText", {camel::toWide(settings_.controlled.name)})));
        co_return;
    }
    auto weak = get_weak();
    auto queue = DispatcherQueue();
    const auto server = server_;
    co_await winrt::resume_background();
    camel::server::addFirewallRule();
    const bool found = server->playerFirewallRule();
    queue.TryEnqueue([weak, found] {
        const auto self = weak.get();
        if (!self) return;
        self->firewallRule_ = found;
        self->populateOutputs();
    });
}

// The first player enabled says why before Windows asks for an administrator; declining leaves the notice with its fix.
winrt::fire_and_forget MainWindow::askFirewallRule() {
    auto strong = get_strong();
    if (!firewallRule_) {
        firewallAsk_ = true; // asked again once the rule is known
        readFirewallRule();
        co_return;
    }
    if (*firewallRule_) co_return;
    if (!server_->filesHere()) {
        addFirewallRule();
        co_return;
    }
    ContentDialog dialog;
    ui::placeDialog(dialog, ContentLayout().XamlRoot());
    dialog.Title(box_value(tr(L"Firewall_Title")));
    TextBlock explanation;
    explanation.Text(tr(L"Firewall_Text"));
    explanation.TextWrapping(TextWrapping::Wrap);
    dialog.Content(explanation);
    dialog.PrimaryButtonText(tr(L"Firewall_AddButton/Content"));
    dialog.CloseButtonText(tr(L"Reset_Cancel"));
    dialog.DefaultButton(ContentDialogButton::Primary);
    if (co_await ui::showDialog(dialog) == ContentDialogResult::Primary)
        addFirewallRule();
    else
        populateOutputs();
}

void MainWindow::OnAddFirewallRuleClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    addFirewallRule();
}

void MainWindow::changeOutputs(std::function<void(camel::Settings&, const DeviceCatalog&)> const& change) {
    auto settings = nativeSettings(settings_);
    change(settings, server_->snapshot().devices);
    settings_ = settingsDto(settings);
    Command command;
    command.kind = CommandKind::ApplySettings;
    command.settings = settings_;
    submitCommand(std::move(command));
}

// Any output's parameters, in use or not, in the controls of the settings page; changes apply as they are made.
winrt::fire_and_forget MainWindow::showOutputSettings(Backend backend, std::string id, hstring label) {
    auto strong = get_strong();
    const auto real = nativeSettings(settings_);
    const auto rows = camel::server::outputRows(real, server_->snapshot().devices);
    const bool inUse = std::ranges::any_of(
        rows, [&](const camel::server::OutputRow& r) { return r.selected && r.backend == backend && r.id == id; });
    const OutputEdit& editing = editingOutput_.emplace(OutputEdit{
        backend, id, settingsDto(camel::server::withOutputParameters(real, backend, id, server_->snapshot().devices))});
    populating_ = true;
    showOutputParameters(editing.parameters);
    populating_ = false;
    updateBackendGroups();
    AsioPanelButton().IsEnabled(inUse); // the driver's panel opens for the driver in use
    const auto panel = OutputSettingsPanel();
    OutputSettingsHost().Child(nullptr);
    ScrollViewer scroller;
    StackPanel content;
    content.Children().Append(panel);
    content.Children().Append(outputZoneSection(editing));
    content.Children().Append(outputDspSection(editing, rows));
    scroller.Content(content);
    panel.Padding(ThicknessHelper::FromLengths(0, 0, 16, 0));
    ContentDialog dialog;
    ui::placeDialog(dialog, ContentLayout().XamlRoot());
    dialog.Title(box_value(label));
    dialog.Content(scroller);
    dialog.CloseButtonText(tr(L"Output_SettingsClose"));
    try {
        co_await ui::showDialog(dialog);
    } catch (const hresult_error& e) { // the page shows the settings' own parameters again all the same
        camel::log::warn("output settings dialog: %ls", e.message().c_str());
    }
    scroller.Content(nullptr);
    content.Children().Clear();
    outputDspText_ = nullptr;
    OutputSettingsHost().Child(panel);
    editingOutput_.reset();
    AsioPanelButton().IsEnabled(true);
    populateSettings();
    updateBackendGroups();
}

StackPanel MainWindow::outputDspSection(const OutputEdit& editing, const std::vector<camel::server::OutputRow>& rows) {
    StackPanel section;
    section.Spacing(8);
    section.Margin(ThicknessHelper::FromLengths(0, 16, 16, 0));
    TextBlock header;
    header.Text(tr(L"Output_Dsp"));
    header.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
    section.Children().Append(header);
    outputDspText_ = TextBlock{};
    outputDspText_.Text(dspSummary(editing.parameters));
    outputDspText_.TextWrapping(TextWrapping::Wrap);
    section.Children().Append(outputDspText_);

    ComboBox copy;
    copy.Header(box_value(tr(L"Output_CopyDspFrom")));
    copy.MinWidth(240);
    std::vector<std::pair<Backend, std::string>> sources;
    for (const auto& row : rows)
        if ((row.enabled || row.selected) && (row.backend != editing.backend || row.id != editing.id)) {
            copy.Items().Append(box_value(outputMenuLabel(row)));
            sources.emplace_back(row.backend, row.id);
        }
    copy.IsEnabled(!sources.empty());
    copy.SelectionChanged([this, sources](Windows::Foundation::IInspectable const& sender, auto&&) {
        const auto box = sender.as<ComboBox>();
        const int index = box.SelectedIndex();
        if (index < 0 || size_t(index) >= sources.size()) return;
        const auto& [backend, id] = sources[size_t(index)];
        changeOutputDsp(camel::outputDsp(
            camel::server::withOutputParameters(nativeSettings(settings_), backend, id, server_->snapshot().devices)));
        box.SelectedIndex(-1);
    });
    section.Children().Append(copy);

    Button allOff;
    allOff.Content(box_value(tr(L"Output_DspAllOff")));
    allOff.Click([this](auto&&, auto&&) { changeOutputDsp({}); });
    section.Children().Append(allOff);
    return section;
}

// The dialog's zone: what its output's zone does when the queue ends and at track changes (MZ-R07).
StackPanel MainWindow::outputZoneSection(const OutputEdit& editing) {
    StackPanel section;
    section.Spacing(8);
    section.Margin(ThicknessHelper::FromLengths(0, 16, 16, 0));
    TextBlock header;
    header.Text(tr(L"Output_Zone"));
    header.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
    section.Children().Append(header);
    ToggleSwitch autoplay;
    autoplay.Header(box_value(tr(L"Output_ZoneAutoplay")));
    autoplay.OnContent(box_value(tr(L"Output_ZoneAutoplayOn")));
    autoplay.OffContent(box_value(tr(L"Output_ZoneAutoplayOff")));
    autoplay.IsOn(editing.parameters.autoplay);
    autoplay.Toggled([this](Windows::Foundation::IInspectable const& sender, auto&&) {
        if (!editingOutput_) return;
        editingOutput_->parameters.autoplay = sender.as<ToggleSwitch>().IsOn();
        settingsChanged();
    });
    section.Children().Append(autoplay);
    ToggleSwitch crossfade;
    crossfade.Header(box_value(tr(L"Output_ZoneCrossfade")));
    crossfade.OnContent(box_value(tr(L"Output_ZoneCrossfadeOn")));
    crossfade.OffContent(box_value(tr(L"Output_ZoneCrossfadeOn")));
    crossfade.IsOn(editing.parameters.crossfade.enabled);
    NumberBox seconds;
    seconds.Header(box_value(tr(L"Output_ZoneCrossfadeSeconds")));
    seconds.Minimum(camel::CrossfadeSettings::kMinSeconds);
    seconds.Maximum(camel::CrossfadeSettings::kMaxSeconds);
    seconds.SmallChange(0.5);
    seconds.SpinButtonPlacementMode(NumberBoxSpinButtonPlacementMode::Inline);
    seconds.Width(170);
    seconds.HorizontalAlignment(HorizontalAlignment::Left);
    seconds.Value(editing.parameters.crossfade.seconds);
    seconds.IsEnabled(editing.parameters.crossfade.enabled);
    crossfade.Toggled([this, weak = winrt::make_weak(seconds)](Windows::Foundation::IInspectable const& sender, auto&&) {
        if (!editingOutput_) return;
        editingOutput_->parameters.crossfade.enabled = sender.as<ToggleSwitch>().IsOn();
        if (const auto box = weak.get()) box.IsEnabled(editingOutput_->parameters.crossfade.enabled);
        settingsChanged();
    });
    seconds.ValueChanged([this](NumberBox const& box, auto&&) {
        if (!editingOutput_ || std::isnan(box.Value())) return;
        editingOutput_->parameters.crossfade.seconds = std::clamp(
            std::round(box.Value() * 2) / 2, camel::CrossfadeSettings::kMinSeconds, camel::CrossfadeSettings::kMaxSeconds);
        settingsChanged();
    });
    section.Children().Append(crossfade);
    section.Children().Append(seconds);
    return section;
}

void MainWindow::changeOutputDsp(const camel::OutputDsp& dsp) {
    if (!editingOutput_) return;
    auto edited = nativeSettings(editingOutput_->parameters);
    camel::setOutputDsp(edited, dsp);
    editingOutput_->parameters = settingsDto(edited);
    if (outputDspText_) outputDspText_.Text(dspSummary(editingOutput_->parameters));
    settingsChanged();
}

winrt::fire_and_forget MainWindow::askRenameOutput(Backend backend, std::string id, std::string alias, hstring label) {
    auto strong = get_strong();
    ContentDialog dialog;
    ui::placeDialog(dialog, ContentLayout().XamlRoot());
    dialog.Title(box_value(ui::format(L"Output_RenameTitle", {std::wstring(label)})));
    TextBox name;
    name.Text(ui::utf8(alias));
    name.Header(box_value(tr(L"Output_RenameNote")));
    dialog.Content(name);
    dialog.PrimaryButtonText(tr(L"Output_Rename"));
    dialog.CloseButtonText(tr(L"Reset_Cancel"));
    dialog.DefaultButton(ContentDialogButton::Primary);
    if (co_await ui::showDialog(dialog) != ContentDialogResult::Primary) co_return;
    std::wstring text(name.Text());
    text.erase(0, text.find_first_not_of(L" \t"));
    text.erase(text.find_last_not_of(L" \t") + 1);
    changeOutputs([&](camel::Settings& s, const DeviceCatalog& devices) {
        camel::server::renameOutput(s, backend, id, toUtf8(text), devices);
    });
    populateOutputs();
}

// The address typed, "host:port", and the host for the HQPlayer's name until it is found.
std::optional<std::pair<std::string, std::string>> hqplayerAddress(std::wstring host, const std::wstring& port) {
    host.erase(0, host.find_first_not_of(L" \t"));
    host.erase(host.find_last_not_of(L" \t") + 1);
    if (host.empty()) host = L"localhost";
    std::string typed = toUtf8(host);
    if (typed.find(':') != std::string::npos && !typed.starts_with('[')) typed = "[" + typed + "]";
    std::string parsedHost;
    uint16_t parsedPort = 0;
    if (!camel::server::hqplayer::splitAddress(typed + ":" + toUtf8(port), parsedHost, parsedPort)) return std::nullopt;
    return std::pair(camel::server::hqplayer::addressOf(parsedHost, parsedPort), parsedHost);
}

bool hqplayerPort(const std::wstring& port) {
    std::string host;
    uint16_t parsed = 0;
    return camel::server::hqplayer::splitAddress("localhost:" + toUtf8(port), host, parsed);
}

winrt::fire_and_forget MainWindow::askAddHQPlayer() {
    auto strong = get_strong();
    ContentDialog dialog;
    ui::placeDialog(dialog, ContentLayout().XamlRoot());
    dialog.Title(box_value(tr(L"Output_AddHQPlayer")));
    StackPanel fields;
    fields.Spacing(12);
    TextBox host;
    host.Header(box_value(tr(L"Output_HQPlayerHost")));
    host.PlaceholderText(L"localhost");
    TextBox port;
    port.Header(box_value(tr(L"Output_HQPlayerPort")));
    port.Text(L"4321");
    TextBlock note;
    note.Text(tr(L"Output_HQPlayerNote"));
    note.TextWrapping(TextWrapping::Wrap);
    note.Style(
        unbox_value<Microsoft::UI::Xaml::Style>(Application::Current().Resources().Lookup(box_value(L"CaptionTextBlockStyle"))));
    fields.Children().Append(host);
    fields.Children().Append(port);
    fields.Children().Append(note);
    dialog.Content(fields);
    dialog.PrimaryButtonText(tr(L"Output_HQPlayerAdd"));
    dialog.CloseButtonText(tr(L"Reset_Cancel"));
    dialog.DefaultButton(ContentDialogButton::Primary);
    // What is wrong shows under its field, and a screen reader reads it with the field.
    const auto problem = [critical = themeBrush(L"SystemFillColorCriticalBrush")](TextBox const& field, hstring const& text) {
        Automation::AutomationProperties::SetHelpText(field, text);
        if (text.empty()) {
            field.Description(nullptr);
            return;
        }
        TextBlock said;
        said.Text(text);
        said.TextWrapping(TextWrapping::Wrap);
        said.Foreground(critical);
        field.Description(said);
    };
    const auto check = [dialog, host, port, problem](auto&&, auto&&) {
        const std::wstring typedPort(port.Text());
        const bool portOk = hqplayerPort(typedPort);
        const bool ok = hqplayerAddress(std::wstring(host.Text()), typedPort).has_value();
        problem(port, portOk ? hstring{} : tr(L"Output_HQPlayerBadPort"));
        problem(host, portOk && !ok ? tr(L"Output_HQPlayerBadHost") : hstring{});
        dialog.IsPrimaryButtonEnabled(ok);
    };
    host.TextChanged(check);
    port.TextChanged(check);
    if (co_await ui::showDialog(dialog) != ContentDialogResult::Primary) co_return;
    const auto chosen = hqplayerAddress(std::wstring(host.Text()), std::wstring(port.Text()));
    if (!chosen) co_return;
    changeOutputs([&](camel::Settings& s, const DeviceCatalog& devices) {
        camel::server::addHQPlayer(s, chosen->first, chosen->second, devices);
    });
    Command refresh;
    refresh.kind = CommandKind::RefreshDevices;
    server_->submit(std::move(refresh));
    populateOutputs();
    camel::server::OutputRow added;
    added.backend = Backend::HQPlayer;
    added.id = chosen->first;
    if (camel::server::needsFirewallRule(added)) askFirewallRule();
}

// What the user types is sent to the receiver by the Server, which keeps it protected for this Windows account; the
// dialog stays open until the receiver has answered. `then` runs once it is paired.
winrt::fire_and_forget MainWindow::askAirPlayPairing(camel::server::OutputRow row, hstring label, std::function<void()> then) {
    auto strong = get_strong();
    const bool code = row.unsupported == "code";
    if (code) {
        Command show;
        show.kind = CommandKind::ShowAirPlayCode;
        show.receiver = row.id;
        submitCommand(std::move(show));
    }
    ContentDialog dialog;
    ui::placeDialog(dialog, ContentLayout().XamlRoot());
    dialog.Title(box_value(ui::format(L"AirPlay_PairTitle", {std::wstring(label)})));
    StackPanel content;
    content.Spacing(8);
    TextBlock note;
    note.Text(ui::format(code ? L"AirPlay_PairCode" : L"AirPlay_PairPassword", {std::wstring(label)}));
    note.TextWrapping(TextWrapping::Wrap);
    content.Children().Append(note);
    PasswordBox secret;
    Automation::AutomationProperties::SetName(secret, tr(code ? L"AirPlay_PairCodeName" : L"AirPlay_PairPasswordName"));
    content.Children().Append(secret);
    // Told to screen readers as it appears: what the receiver answered, and that it is being waited for.
    TextBlock problem;
    problem.TextWrapping(TextWrapping::Wrap);
    problem.Foreground(themeBrush(L"SystemFillColorCriticalBrush"));
    problem.Visibility(Visibility::Collapsed);
    Automation::AutomationProperties::SetLiveSetting(problem, Automation::Peers::AutomationLiveSetting::Assertive);
    content.Children().Append(problem);
    ProgressBar progress;
    progress.IsIndeterminate(true);
    progress.Visibility(Visibility::Collapsed);
    Automation::AutomationProperties::SetName(progress, ui::format(L"AirPlay_PairWaiting", {std::wstring(label)}));
    content.Children().Append(progress);
    TextBlock kept;
    kept.Text(tr(L"AirPlay_PairKept"));
    kept.TextWrapping(TextWrapping::Wrap);
    kept.Style(
        unbox_value<Microsoft::UI::Xaml::Style>(Application::Current().Resources().Lookup(box_value(L"CaptionTextBlockStyle"))));
    content.Children().Append(kept);
    dialog.Content(content);
    dialog.PrimaryButtonText(tr(L"AirPlay_PairButton"));
    dialog.CloseButtonText(tr(L"Reset_Cancel"));
    dialog.DefaultButton(ContentDialogButton::Primary);
    const auto paired = std::make_shared<bool>(false);
    const winrt::weak_ref<ContentDialog> weakDialog = dialog;
    const winrt::weak_ref<PasswordBox> weakSecret = secret;
    const winrt::weak_ref<TextBlock> weakProblem = problem;
    const winrt::weak_ref<ProgressBar> weakProgress = progress;
    dialog.PrimaryButtonClick([this, row, label, paired, weakDialog, weakSecret, weakProblem,
                               weakProgress](ContentDialog const&, ContentDialogButtonClickEventArgs const& args) {
        args.Cancel(true);
        const auto shown = weakDialog.get();
        const auto typed = weakSecret.get();
        if (!shown || !typed || typed.Password().empty() || pairing_ || !server_) return;
        shown.IsPrimaryButtonEnabled(false);
        if (const auto bar = weakProgress.get()) bar.Visibility(Visibility::Visible);
        if (const auto text = weakProblem.get()) {
            text.Visibility(Visibility::Collapsed);
            text.Text({}); // the same answer again is told again
        }
        Command pair;
        pair.kind = CommandKind::PairAirPlay;
        pair.receiver = row.id;
        pair.code = toUtf8(std::wstring(typed.Password()));
        const auto stamp = server_->submit(std::move(pair));
        cursor_.expect(stamp);
        pairing_ =
            PendingPairing{stamp.id, [label, paired, weakDialog, weakProblem,
                                      weakProgress](const std::optional<camel::server::Failure>& failure) {
                               const auto open = weakDialog.get();
                               if (!open) return;
                               open.IsPrimaryButtonEnabled(true);
                               if (const auto bar = weakProgress.get()) bar.Visibility(Visibility::Collapsed);
                               if (!failure) {
                                   *paired = true;
                                   open.Hide();
                                   return;
                               }
                               if (const auto text = weakProblem.get()) {
                                   text.Visibility(Visibility::Visible);
                                   setAnnounced(text, failure->code == ErrorCode::AccessDenied
                                                          ? tr(L"AirPlay_PairWrong")
                                                          : ui::format(L"AirPlay_PairFailed",
                                                                       {std::wstring(label), ui::utf8(failure->detail).c_str()}));
                               }
                           }};
    });
    co_await ui::showDialog(dialog);
    pairing_.reset();
    if (*paired && then)
        then();
    else
        populateOutputs(); // a switch turned on goes back off
}

// Whether the zone of `row` plays now, which turning the output off stops.
bool MainWindow::zonePlays(const camel::server::OutputRow& row) const {
    const auto snapshot = server_->snapshot();
    const auto zone = std::ranges::find(snapshot.zones, zoneId(row.backend, row.id), &ZoneSnapshot::id);
    return zone != snapshot.zones.end() && zone->playback.state == PlaybackState::Playing;
}

// Turning off an output whose zone plays asks once; its queue stays for when it is turned on again.
winrt::fire_and_forget MainWindow::askTurnOffOutput(camel::server::OutputRow row, hstring label) {
    auto strong = get_strong();
    ContentDialog dialog;
    ui::placeDialog(dialog, ContentLayout().XamlRoot());
    dialog.Title(box_value(ui::format(L"Output_TurnOffTitle", {std::wstring(label)})));
    TextBlock explanation;
    explanation.Text(tr(L"Output_TurnOffText"));
    explanation.TextWrapping(TextWrapping::Wrap);
    dialog.Content(explanation);
    dialog.PrimaryButtonText(tr(L"Output_TurnOff"));
    dialog.CloseButtonText(tr(L"Reset_Cancel"));
    dialog.DefaultButton(ContentDialogButton::Close);
    if (co_await ui::showDialog(dialog) != ContentDialogResult::Primary) {
        populateOutputs(); // the switch goes back on
        co_return;
    }
    changeOutputs([&](camel::Settings& s, const DeviceCatalog& devices) {
        camel::server::setOutputEnabled(s, row.backend, row.id, false, devices);
    });
}

// Deleting an output deletes its zone's queue too, which the dialog offers to keep as a playlist first.
winrt::fire_and_forget MainWindow::askDeleteOutput(Backend backend, std::string id, hstring label) {
    auto strong = get_strong();
    const std::string zone = zoneId(backend, id);
    const auto snapshot = server_->snapshot();
    const auto found = std::ranges::find(snapshot.zones, zone, &ZoneSnapshot::id);
    const bool queued = found != snapshot.zones.end() && !found->queue.entries.empty();
    ContentDialog dialog;
    ui::placeDialog(dialog, ContentLayout().XamlRoot());
    dialog.Title(box_value(ui::format(L"Output_DeleteTitle", {std::wstring(label)})));
    StackPanel content;
    content.Spacing(12);
    TextBlock explanation;
    explanation.Text(tr(queued ? L"Output_DeleteQueueText" : L"Output_DeleteText"));
    explanation.TextWrapping(TextWrapping::Wrap);
    content.Children().Append(explanation);
    CheckBox keep;
    keep.Content(box_value(tr(L"Output_DeleteKeepQueue")));
    if (queued) content.Children().Append(keep);
    dialog.Content(content);
    dialog.PrimaryButtonText(tr(L"Output_Delete"));
    dialog.CloseButtonText(tr(L"Reset_Cancel"));
    dialog.DefaultButton(ContentDialogButton::Close);
    if (co_await ui::showDialog(dialog) != ContentDialogResult::Primary) co_return;
    if (queued && keep.IsChecked().Value()) {
        const auto name = toUtf8(std::wstring(ui::format(L"Output_DeletedQueueName", {std::wstring(label)})));
        auto server = server_;
        winrt::apartment_context ui;
        co_await resume_background();
        std::optional<PlaylistChange> change;
        try {
            change = server->saveQueueAsPlaylist(name, zone);
        } catch (const std::exception& e) {
            camel::log::warn("save queue as playlist: %s", e.what());
        }
        co_await ui;
        if (!change) {
            showPlaylistNotice(tr(L"Queue_SaveFailed"), {}, true);
            co_return; // nothing is deleted with its queue unsaved
        }
        showPlaylistChange(*change, true);
    }
    Command remove;
    remove.kind = CommandKind::DeleteOutput;
    remove.zone = zone;
    submitCommand(std::move(remove));
}

// The zone picker (docs/Plan.md §5.12, §8.1): a row for each zone in the output menu's order, with its cover, track,
// state, play or pause and volume; a click on a row selects its zone, which the whole window follows. Under the rows, the
// receivers of the selected AirPlay group join and leave it, and Pause all pauses every zone.
void MainWindow::OnOutputButtonClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    if (askSameNamedOutputs([this] { openZonePicker(); })) return;
    openZonePicker();
}

// Two enabled outputs showing one name, as 0.5.1 allowed or a player renamed on itself, are named apart when the zone
// picker or Settings › Audio outputs opens; false when none share a name. `then` runs once the question closes.
bool MainWindow::askSameNamedOutputs(std::function<void()> then) {
    if (askingName_) return true;
    const auto rows = camel::server::outputRows(nativeSettings(settings_), server_->snapshot().devices);
    for (size_t i = 0; i < rows.size(); ++i) {
        if (!rows[i].enabled) continue;
        const bool earlier = std::ranges::any_of(rows.begin(), rows.begin() + std::ptrdiff_t(i), [&](const auto& other) {
            return other.enabled && other.backend == rows[i].backend && other.name == rows[i].name;
        });
        if (!earlier) continue;
        askOutputName(rows[i], false, std::move(then));
        return true;
    }
    return false;
}

// Asks for a name of its own for `row`, "DMP-A6 (2)" offered; `enable` turns it on with it, and Cancel leaves it off.
winrt::fire_and_forget MainWindow::askOutputName(camel::server::OutputRow row, bool enable, std::function<void()> then) {
    auto strong = get_strong();
    askingName_ = true;
    const auto rows = camel::server::outputRows(nativeSettings(settings_), server_->snapshot().devices);
    auto candidate = row;
    candidate.enabled = true;
    ContentDialog dialog;
    ui::placeDialog(dialog, ContentLayout().XamlRoot());
    dialog.Title(box_value(ui::format(L"Output_SameNameTitle", {std::wstring(outputMenuLabel(row))})));
    StackPanel content;
    content.Spacing(12);
    TextBlock explanation;
    explanation.Text(tr(L"Output_SameNameText"));
    explanation.TextWrapping(TextWrapping::Wrap);
    content.Children().Append(explanation);
    TextBox name;
    name.Text(ui::utf8(camel::server::freeName(rows, candidate)));
    name.SelectAll();
    Automation::AutomationProperties::SetName(name, tr(L"Output_SameNameBox"));
    content.Children().Append(name);
    TextBlock taken;
    taken.Text(tr(L"Output_SameNameTaken"));
    taken.Visibility(Visibility::Collapsed);
    taken.Foreground(themeBrush(L"SystemFillColorCriticalBrush"));
    content.Children().Append(taken);
    dialog.Content(content);
    dialog.PrimaryButtonText(tr(enable ? L"Output_SameNameEnable" : L"Output_Rename"));
    dialog.CloseButtonText(tr(L"Reset_Cancel"));
    dialog.DefaultButton(ContentDialogButton::Primary);
    // A name another enabled output shows keeps the question open.
    dialog.PrimaryButtonClick([rows, candidate, weak = winrt::make_weak(name),
                               weakTaken = winrt::make_weak(taken)](auto&&, ContentDialogButtonClickEventArgs const& e) {
        const auto box = weak.get();
        if (!box) return;
        auto named = candidate;
        std::wstring text(box.Text());
        text.erase(0, text.find_first_not_of(L" \t"));
        text.erase(text.find_last_not_of(L" \t") + 1);
        named.name = toUtf8(text);
        if (!text.empty() && !camel::server::nameTaken(rows, named)) return;
        e.Cancel(true);
        if (const auto shown = weakTaken.get()) shown.Visibility(Visibility::Visible);
    });
    const auto answer = co_await ui::showDialog(dialog);
    askingName_ = false;
    if (answer == ContentDialogResult::Primary) {
        std::wstring text(name.Text());
        text.erase(0, text.find_first_not_of(L" \t"));
        text.erase(text.find_last_not_of(L" \t") + 1);
        changeOutputs([&](camel::Settings& s, const DeviceCatalog& devices) {
            camel::server::renameOutput(s, row.backend, row.id, toUtf8(text), devices);
            if (enable) camel::server::setOutputEnabled(s, row.backend, row.id, true, devices);
        });
        if (enable && camel::server::needsFirewallRule(row)) askFirewallRule();
    }
    populateOutputs(); // a switch turned on and cancelled goes back off
    if (then) then();
}

void MainWindow::openZonePicker() {
    if (zonePicker_) zonePicker_.Hide();
    Flyout picker;
    picker.Placement(Primitives::FlyoutPlacementMode::Top);
    Style presenter(xaml_typename<FlyoutPresenter>());
    presenter.Setters().Append(Setter(Control::PaddingProperty(), box_value(ThicknessHelper::FromUniformLength(4))));
    picker.FlyoutPresenterStyle(presenter);
    zonePicker_ = picker;
    zoneRows_.clear();
    StackPanel content;
    content.MinWidth(320);
    content.MaxWidth(400);
    content.XYFocusKeyboardNavigation(Input::XYFocusKeyboardNavigationMode::Enabled);
    // Rows look like menu items: no fill or border, a disabled one's included.
    const Media::SolidColorBrush clear(Microsoft::UI::Colors::Transparent());
    for (const auto* key : {L"ButtonBackground", L"ButtonBackgroundDisabled", L"ButtonBorderBrush",
                            L"ButtonBorderBrushPointerOver", L"ButtonBorderBrushPressed", L"ButtonBorderBrushDisabled"})
        content.Resources().Insert(box_value(key), clear);
    Automation::AutomationProperties::SetName(content, tr(L"Zone_PickerName"));
    if (std::dynamic_pointer_cast<RemoteServer>(server_)) {
        TextBlock computer;
        computer.Text(ui::format(L"Title_On", {toWide(settings_.controlled.name)}));
        computer.Style(Application::Current().Resources().Lookup(box_value(L"BodyStrongTextBlockStyle")).as<Style>());
        computer.Margin(ThicknessHelper::FromLengths(11, 4, 11, 0));
        content.Children().Append(computer);
    }
    const auto snapshot = server_->snapshot();
    const auto outputs = camel::server::outputRows(nativeSettings(settings_), snapshot.devices);
    const auto heading = [&](const wchar_t* key) {
        TextBlock text;
        text.Text(tr(key));
        text.Style(Application::Current().Resources().Lookup(box_value(L"CaptionTextBlockStyle")).as<Style>());
        text.Foreground(themeBrush(L"TextFillColorSecondaryBrush"));
        text.Margin(ThicknessHelper::FromLengths(11, content.Children().Size() ? 8 : 4, 11, 4));
        content.Children().Append(text);
    };
    // The zone the window follows comes first, then the others in the output menu's order.
    std::vector<const ZoneSnapshot*> zones;
    for (const auto& zone : snapshot.zones)
        if (zone.id == snapshot.selectedZone)
            zones.insert(zones.begin(), &zone);
        else
            zones.push_back(&zone);
    for (const ZoneSnapshot* zone : zones) {
        const auto output = std::ranges::find_if(
            outputs, [&](const camel::server::OutputRow& o) { return o.backend == zone->backend && o.id == zone->output; });
        if (output == outputs.end()) continue;
        const bool selected = zone->id == snapshot.selectedZone;
        if (selected)
            heading(L"Zone_Controlling");
        else if (zoneRows_.empty() || zoneRows_.back().zone == snapshot.selectedZone)
            heading(L"Zone_Others");
        ZoneRow row;
        row.zone = zone->id;
        row.output = *output;
        Grid line;
        line.CornerRadius(CornerRadiusHelper::FromUniformRadius(4));
        for (const auto width : {GridLengthHelper::FromValueAndType(1, GridUnitType::Star), GridLengthHelper::Auto()}) {
            ColumnDefinition column;
            column.Width(width);
            line.ColumnDefinitions().Append(column);
        }
        if (selected) line.Background(themeBrush(L"SubtleFillColorSecondaryBrush"));
        Grid face;
        face.ColumnSpacing(12);
        for (const auto width : {GridLengthHelper::FromPixels(20), GridLengthHelper::FromValueAndType(1, GridUnitType::Star)}) {
            ColumnDefinition column;
            column.Width(width);
            face.ColumnDefinitions().Append(column);
        }
        row.icon = glyph(zoneGlyph(output->backend));
        row.icon.VerticalAlignment(VerticalAlignment::Center);
        face.Children().Append(row.icon);
        StackPanel texts;
        texts.VerticalAlignment(VerticalAlignment::Center);
        row.title = TextBlock();
        row.title.TextTrimming(TextTrimming::CharacterEllipsis);
        row.detail = TextBlock();
        row.detail.TextTrimming(TextTrimming::CharacterEllipsis);
        row.detail.Style(Application::Current().Resources().Lookup(box_value(L"CaptionTextBlockStyle")).as<Style>());
        texts.Children().Append(row.title);
        texts.Children().Append(row.detail);
        Grid::SetColumn(texts, 1);
        face.Children().Append(texts);
        row.select = Button();
        row.select.Content(face);
        row.select.HorizontalAlignment(HorizontalAlignment::Stretch);
        row.select.HorizontalContentAlignment(HorizontalAlignment::Stretch);
        row.select.BorderThickness(ThicknessHelper::FromUniformLength(0));
        row.select.Padding(ThicknessHelper::FromLengths(11, 7, 11, 7));
        row.select.Click([this, id = zone->id](auto&&, auto&&) { selectZone(id); });
        MenuFlyout actions;
        actions.Opening([this, id = zone->id](Windows::Foundation::IInspectable const& sender, auto&&) {
            fillZoneActions(sender.as<MenuFlyout>(), id);
        });
        row.select.ContextFlyout(actions);
        line.Children().Append(row.select);
        // Pause for a zone playing that the window does not follow, shown while the pointer is over its row.
        FontIcon pauseIcon = glyph(L"\xE769");
        pauseIcon.FontSize(14);
        row.pause = Button();
        row.pause.Content(pauseIcon);
        row.pause.Padding(ThicknessHelper::FromUniformLength(8));
        row.pause.Margin(ThicknessHelper::FromLengths(0, 0, 4, 0));
        row.pause.VerticalAlignment(VerticalAlignment::Center);
        row.pause.Opacity(0);
        ToolTipService::SetToolTip(row.pause, box_value(tr(L"Tip_Pause")));
        row.pause.Click([this, id = zone->id](auto&&, auto&&) {
            Command command;
            command.kind = CommandKind::Pause;
            command.zone = id;
            submitCommand(std::move(command));
        });
        const auto reveal = [weak = winrt::make_weak(row.pause)](bool shown) {
            if (const auto pause = weak.get()) pause.Opacity(shown && pause.IsEnabled() ? 1.0 : 0.0);
        };
        // The row's buttons handle the pointer, so the row hears it among handled events; leaving one of them for the
        // other is not leaving the row.
        line.AddHandler(UIElement::PointerEnteredEvent(),
                        box_value(Input::PointerEventHandler([reveal](auto&&, auto&&) { reveal(true); })), true);
        line.AddHandler(UIElement::PointerExitedEvent(),
                        box_value(Input::PointerEventHandler(
                            [reveal](Windows::Foundation::IInspectable const& sender, Input::PointerRoutedEventArgs const& e) {
                                const auto row = sender.as<FrameworkElement>();
                                const auto at = e.GetCurrentPoint(row).Position();
                                reveal(at.X >= 0 && at.Y >= 0 && at.X < row.ActualWidth() && at.Y < row.ActualHeight());
                            })),
                        true);
        row.pause.GotFocus([reveal](auto&&, auto&&) { reveal(true); });
        row.pause.LostFocus([reveal](auto&&, auto&&) { reveal(false); });
        Grid::SetColumn(row.pause, 1);
        line.Children().Append(row.pause);
        content.Children().Append(line);
        zoneRows_.push_back(std::move(row));
        if (selected) addReceiversRow(content, snapshot, outputs);
    }
    Border divider;
    divider.Height(1);
    divider.Margin(ThicknessHelper::FromLengths(-4, 4, -4, 4));
    divider.Background(themeBrush(L"DividerStrokeColorDefaultBrush"));
    content.Children().Append(divider);
    Grid foot;
    for (int i = 0; i < 2; ++i) {
        ColumnDefinition column;
        column.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        foot.ColumnDefinitions().Append(column);
    }
    TextBlock pauseAll;
    pauseAll.Text(tr(L"Zone_PauseAll"));
    pauseAllButton_ = pickerRow(glyph(L"\xE769"), pauseAll, nullptr);
    pauseAllButton_.Click([this](auto&&, auto&&) {
        Command command;
        command.kind = CommandKind::PauseAll;
        submitCommand(std::move(command));
    });
    foot.Children().Append(pauseAllButton_);
    TextBlock settings;
    settings.Text(tr(L"Output_MenuSettings"));
    auto open = pickerRow(glyph(L"\xE713"), settings, nullptr);
    open.Click([this](auto&&, auto&&) {
        if (zonePicker_) zonePicker_.Hide();
        showSettingsCard(OutputsCard());
    });
    Grid::SetColumn(open, 1);
    foot.Children().Append(open);
    content.Children().Append(foot);
    otherComputerRows_ = StackPanel();
    content.Children().Append(otherComputerRows_);
    showOtherComputers();
    findOtherComputers();
    picker.Content(content);
    // One closing as the next opens leaves the next one's rows.
    picker.Closed([this, weak = winrt::make_weak(picker)](auto&&, auto&&) {
        if (zonePicker_ != weak.get()) return;
        zonePicker_ = nullptr;
        zoneRows_.clear();
        pauseAllButton_ = nullptr;
        otherComputerRows_ = nullptr;
    });
    showZoneRows(snapshot);
    picker.ShowAt(OutputButton());
}

// The zone picker's foot: the other CamelPlayers found to control and, on a Control point, playing here again
// (docs/Plan.md §17.8).
void MainWindow::showOtherComputers() {
    if (!otherComputerRows_) return;
    const bool remote = std::dynamic_pointer_cast<RemoteServer>(server_) != nullptr;
    auto rows = otherComputerRows_.Children();
    rows.Clear();
    if (otherComputers_.empty() && !remote) return;
    Border divider;
    divider.Height(1);
    divider.Margin(ThicknessHelper::FromLengths(-4, 4, -4, 4));
    divider.Background(themeBrush(L"DividerStrokeColorDefaultBrush"));
    rows.Append(divider);
    if (!otherComputers_.empty()) {
        TextBlock heading;
        heading.Text(tr(L"Zone_OtherComputers"));
        heading.Style(Application::Current().Resources().Lookup(box_value(L"CaptionTextBlockStyle")).as<Style>());
        heading.Foreground(themeBrush(L"TextFillColorSecondaryBrush"));
        heading.Margin(ThicknessHelper::FromLengths(11, 4, 11, 4));
        rows.Append(heading);
    }
    for (const auto& found : otherComputers_) {
        TextBlock name;
        name.Text(toWide(found.announced.name));
        auto row = pickerRow(glyph(L"\xE977"), name, nullptr);
        Automation::AutomationProperties::SetName(row, ui::format(L"Zone_ControlComputer", {toWide(found.announced.name)}));
        row.Click([this, id = found.announced.id](auto&&, auto&&) {
            if (zonePicker_) zonePicker_.Hide();
            chooseServerToControl(id);
        });
        rows.Append(row);
    }
    if (remote) {
        TextBlock here;
        here.Text(tr(L"Control_PlayHere"));
        auto row = pickerRow(glyph(L"\xE80F"), here, nullptr);
        Automation::AutomationProperties::SetName(row, here.Text());
        row.Click([this](auto&&, auto&&) {
            if (zonePicker_) zonePicker_.Hide();
            playHere();
        });
        rows.Append(row);
    }
}

// Looked for each time the zone picker opens, the ones found last shown meanwhile; neither this computer's own Server
// nor the one it controls is offered.
winrt::fire_and_forget MainWindow::findOtherComputers() {
    auto strong = get_strong();
    if (findingComputers_) co_return;
    findingComputers_ = true;
    std::vector<std::string> skipped;
    if (access_) skipped.push_back(access_->serverId());
    if (std::dynamic_pointer_cast<RemoteServer>(server_)) skipped.push_back(settings_.controlled.id);
    winrt::apartment_context foreground;
    co_await winrt::resume_background();
    std::vector<FoundServer> found;
    try {
        found = findServers(std::chrono::milliseconds(1500));
    } catch (const std::exception& e) {
        log::warn("finding other CamelPlayers: %s", e.what());
    }
    co_await foreground;
    findingComputers_ = false;
    std::erase_if(found, [&](const FoundServer& s) { return std::ranges::find(skipped, s.announced.id) != skipped.end(); });
    std::ranges::sort(found, {}, [](const FoundServer& s) { return s.announced.name; });
    otherComputers_ = std::move(found);
    showOtherComputers();
}

// The selected AirPlay group's receivers, in a submenu under its row, which join and leave it; the last one stays. One
// another group plays through is greyed and says which; one of a group that does not play moves from it into this one
// (MZ-R08).
void MainWindow::addReceiversRow(StackPanel const& content, const Snapshot& snapshot,
                                 const std::vector<camel::server::OutputRow>& outputs) {
    if (settings_.backend != Backend::AirPlay) return;
    MenuFlyout receivers;
    receivers.Placement(Primitives::FlyoutPlacementMode::RightEdgeAlignedTop);
    const auto grouped = std::ranges::count_if(outputs, &camel::server::OutputRow::grouped);
    for (const auto& output : outputs) {
        if (!output.enabled || output.backend != Backend::AirPlay) continue;
        const auto holder = std::ranges::find_if(snapshot.zones, [&](const ZoneSnapshot& z) {
            return z.id != snapshot.selectedZone && z.backend == Backend::AirPlay &&
                   (z.output == output.id ||
                    std::ranges::find(z.playback.receivers, output.id, &ReceiverState::id) != z.playback.receivers.end());
        });
        const std::string from = holder == snapshot.zones.end() ? std::string() : holder->output;
        const bool held = holder != snapshot.zones.end() && holder->playback.state == PlaybackState::Playing;
        ToggleMenuFlyoutItem join;
        join.Text(held ? ui::format(L"Zone_ReceiverInUse", {std::wstring(outputMenuText(output)), ui::utf8(holder->name).c_str()})
                       : outputMenuText(output));
        join.IsChecked(output.grouped);
        join.IsEnabled(output.connected && !held && (!output.grouped || grouped >= 2));
        join.Click([this, output, from](auto&&, auto&&) {
            const auto change = [this, output, from] {
                changeOutputs([&](camel::Settings& s, const DeviceCatalog& devices) {
                    if (!output.grouped && from == output.id)
                        camel::server::passAirPlayGroup(s, output.id);
                    else if (!output.grouped && !from.empty())
                        camel::server::leaveAirPlayGroup(s, from, output.id);
                    camel::server::setInAirPlayGroup(s, output.id, !output.grouped, devices);
                });
                populateSettings();
                if (zonePicker_) zonePicker_.Hide();
            };
            if (pairable(output) && !output.grouped)
                askAirPlayPairing(output, outputLabel(output), change);
            else
                change();
        });
        receivers.Items().Append(join);
    }
    if (!receivers.Items().Size()) return;
    FontIcon more;
    more.Glyph(L"\xE76C");
    more.FontSize(12);
    TextBlock text;
    text.Text(tr(L"Zone_GroupHeading"));
    auto group = pickerRow(nullptr, text, more);
    group.Padding(ThicknessHelper::FromLengths(15, 8, 11, 9)); // its text under the zone's name
    group.Flyout(receivers);
    content.Children().Append(group);
}

// A row's right-click menu: the zone the window follows sends its queue to another; another takes the queue here; and
// each opens its output's settings.
void MainWindow::fillZoneActions(MenuFlyout const& menu, const std::string& id) {
    menu.Items().Clear();
    const auto snapshot = server_->snapshot();
    const auto zone = std::ranges::find(snapshot.zones, id, &ZoneSnapshot::id);
    const auto row = std::ranges::find(zoneRows_, id, &ZoneRow::zone);
    if (zone == snapshot.zones.end() || row == zoneRows_.end()) return;
    if (id == snapshot.selectedZone) {
        MenuFlyoutSubItem transfer;
        transfer.Text(tr(L"Zone_Transfer"));
        transfer.Icon(glyph(L"\xE8AB"));
        fillTransferMenu(transfer, id);
        transfer.IsEnabled(!zone->queue.entries.empty() && transfer.Items().Size() > 0);
        menu.Items().Append(transfer);
    } else {
        const auto from = std::ranges::find(snapshot.zones, snapshot.selectedZone, &ZoneSnapshot::id);
        const bool held = !zone->inUseBy.empty() && zone->inUseBy != snapshot.selectedZone;
        MenuFlyoutItem here;
        here.Text(tr(L"Zone_TransferHere"));
        here.Icon(glyph(L"\xE8AB"));
        here.IsEnabled(from != snapshot.zones.end() && !from->queue.entries.empty() && !held);
        here.Click([this, from = snapshot.selectedZone, to = id](auto&&, auto&&) {
            if (zonePicker_) zonePicker_.Hide();
            transferZone(from, to);
        });
        menu.Items().Append(here);
    }
    const auto& output = row->output;
    if (output.backend == Backend::AirPlay && output.pair) return; // a stereo pair has nothing to set
    MenuFlyoutItem settings;
    settings.Text(tr(L"Zone_OutputSettings"));
    settings.Icon(glyph(L"\xE713"));
    settings.Click([this, output](auto&&, auto&&) {
        if (zonePicker_) zonePicker_.Hide();
        showOutputSettings(output.backend, output.id,
                           output.backend == Backend::AirPlay ? outputMenuLabel(output) : outputLabel(output));
    });
    menu.Items().Append(settings);
}

// What each row shows as the zones play: the zone the window follows marked, and for each its state or why it cannot
// play, with its track while it plays or is paused; a stopped one shows its name alone.
void MainWindow::showZoneRows(const Snapshot& snapshot) {
    if (!zonePicker_) return;
    bool anyPlaying = false;
    for (auto& row : zoneRows_) {
        const auto found = std::ranges::find(snapshot.zones, row.zone, &ZoneSnapshot::id);
        if (found == snapshot.zones.end()) continue;
        const ZoneSnapshot& zone = *found;
        const bool selected = zone.id == snapshot.selectedZone;
        const auto& p = zone.playback;
        const bool playing = p.state == PlaybackState::Playing;
        anyPlaying = anyPlaying || playing;
        // An AirPlay group is named by its receivers.
        const hstring name =
            zone.playback.receivers.size() > 1 ? ui::utf8(zone.name) + L" (AirPlay)" : outputMenuLabel(row.output);
        const bool unavailable = !row.output.connected || !zone.inUseBy.empty();
        const hstring state = !row.output.connected ? tr(L"Zone_Offline")
                              : !zone.inUseBy.empty()
                                  ? ui::format(L"Zone_InUseBy", {std::wstring(zoneName(snapshot, zone.inUseBy))})
                              : p.error            ? ui::errorTitle(*p.error)
                              : p.bufferingSeconds ? ui::format(L"State_Buffering", {std::to_wstring(*p.bufferingSeconds)})
                              : playing && p.waitingForPlayer    ? tr(L"State_WaitingForPlayer")
                              : playing                          ? tr(L"State_Playing")
                              : p.state == PlaybackState::Paused ? tr(L"State_Paused")
                                                                 : hstring();
        hstring detail = state;
        if (const auto entry = std::ranges::find(zone.queue.entries, zone.queueEntryId, &QueueEntry::id);
            !unavailable && p.state != PlaybackState::Stopped && entry != zone.queue.entries.end()) {
            hstring track = ui::utf8(
                entry->title.empty() ? std::filesystem::path(toWide(entry->source.pathUtf8())).stem().string() : entry->title);
            if (!entry->artist.empty()) track = track + L" — " + ui::utf8(entry->artist);
            detail = state.empty() ? track : state + L" · " + track;
        }
        row.title.Text(name);
        row.detail.Text(detail);
        row.detail.Visibility(detail.empty() ? Visibility::Collapsed : Visibility::Visible);
        const auto* tone = selected      ? L"AccentTextFillColorPrimaryBrush"
                           : unavailable ? L"TextFillColorDisabledBrush"
                                         : L"TextFillColorPrimaryBrush";
        row.title.Foreground(themeBrush(tone));
        row.icon.Foreground(themeBrush(tone));
        row.detail.Foreground(themeBrush(selected      ? L"AccentTextFillColorPrimaryBrush"
                                         : unavailable ? L"TextFillColorDisabledBrush"
                                                       : L"TextFillColorSecondaryBrush"));
        row.select.IsEnabled(row.output.connected);
        Automation::AutomationProperties::SetName(
            row.select, ui::format(selected ? L"Zone_RowSelectedName" : L"Zone_RowName",
                                   {std::wstring(name), std::wstring(detail.empty() ? tr(L"State_Stopped") : detail)}));
        row.pause.IsEnabled(!selected && playing);
        if (!row.pause.IsEnabled()) row.pause.Opacity(0);
        Automation::AutomationProperties::SetName(
            row.pause, ui::format(L"Zone_PlayName", {std::wstring(tr(L"Tip_Pause")), std::wstring(name)}));
    }
    if (pauseAllButton_) pauseAllButton_.IsEnabled(anyPlaying);
}

hstring MainWindow::zoneLabel(const Snapshot& snapshot, const ZoneSnapshot& zone) const {
    if (zone.playback.receivers.size() > 1) return ui::utf8(zone.name) + L" (AirPlay)";
    const auto rows = camel::server::outputRows(nativeSettings(settings_), snapshot.devices);
    const auto row = std::ranges::find_if(
        rows, [&](const camel::server::OutputRow& r) { return r.backend == zone.backend && r.id == zone.output; });
    return row == rows.end() ? ui::utf8(zone.name) : outputMenuLabel(*row);
}

void MainWindow::OnQueueMenuOpening(Windows::Foundation::IInspectable const&, Windows::Foundation::IInspectable const&) {
    const auto snapshot = server_->snapshot();
    fillTransferMenu(QueueTransferItem(), snapshot.selectedZone);
    QueueTransferItem().IsEnabled(!snapshot.queue.entries.empty() && QueueTransferItem().Items().Size() > 0);
}

// Each other zone takes the queue; one whose device another zone holds is greyed and says which.
void MainWindow::fillTransferMenu(MenuFlyoutSubItem const& menu, const std::string& from) {
    menu.Items().Clear();
    const auto snapshot = server_->snapshot();
    for (const auto& zone : snapshot.zones) {
        if (zone.id == from) continue;
        MenuFlyoutItem item;
        const hstring label = zoneLabel(snapshot, zone);
        const bool held = !zone.inUseBy.empty() && zone.inUseBy != from;
        item.Text(held ? label + L" · " + ui::format(L"Zone_InUseBy", {std::wstring(zoneName(snapshot, zone.inUseBy))}) : label);
        item.IsEnabled(!held);
        item.Click([this, from, to = zone.id](auto&&, auto&&) {
            if (zonePicker_) zonePicker_.Hide();
            transferZone(from, to);
        });
        menu.Items().Append(item);
    }
}

// The queue goes to `to`, replacing its own after one question, which offers to save it as a playlist first; moved from
// the zone the window follows, the window follows it to `to`.
winrt::fire_and_forget MainWindow::transferZone(std::string from, std::string to) {
    auto strong = get_strong();
    const auto snapshot = server_->snapshot();
    const auto target = std::ranges::find(snapshot.zones, to, &ZoneSnapshot::id);
    if (target == snapshot.zones.end()) co_return;
    const hstring label = zoneLabel(snapshot, *target);
    if (!target->queue.entries.empty()) {
        ContentDialog dialog;
        ui::placeDialog(dialog, ContentLayout().XamlRoot());
        dialog.Title(box_value(ui::format(L"Zone_TransferReplaceTitle", {std::wstring(label)})));
        StackPanel content;
        content.Spacing(12);
        TextBlock explanation;
        explanation.Text(tr(L"Zone_TransferReplaceText"));
        explanation.TextWrapping(TextWrapping::Wrap);
        content.Children().Append(explanation);
        CheckBox keep;
        keep.Content(box_value(tr(L"Zone_TransferKeepQueue")));
        content.Children().Append(keep);
        dialog.Content(content);
        dialog.PrimaryButtonText(tr(L"Zone_TransferReplace"));
        dialog.CloseButtonText(tr(L"Reset_Cancel"));
        dialog.DefaultButton(ContentDialogButton::Close);
        if (co_await ui::showDialog(dialog) != ContentDialogResult::Primary) co_return;
        if (keep.IsChecked().Value()) {
            const auto name = toUtf8(std::wstring(ui::format(L"Output_DeletedQueueName", {std::wstring(label)})));
            auto server = server_;
            winrt::apartment_context ui;
            co_await resume_background();
            std::optional<PlaylistChange> change;
            try {
                change = server->saveQueueAsPlaylist(name, to);
            } catch (const std::exception& e) {
                camel::log::warn("save queue as playlist: %s", e.what());
            }
            co_await ui;
            if (!change) {
                showPlaylistNotice(tr(L"Queue_SaveFailed"), {}, true);
                co_return; // its queue is not replaced unsaved
            }
            showPlaylistChange(*change, true);
        }
    }
    Command command;
    command.kind = CommandKind::Transfer;
    command.zone = from;
    command.target = to;
    submitCommand(std::move(command));
    if (from == server_->snapshot().selectedZone) selectZoneOf(*target);
}

hstring MainWindow::zoneName(const Snapshot& snapshot, const std::string& id) const {
    const auto found = std::ranges::find(snapshot.zones, id, &ZoneSnapshot::id);
    return found == snapshot.zones.end() ? ui::utf8(id) : ui::utf8(found->name);
}

void MainWindow::selectZoneOf(const ZoneSnapshot& zone) {
    const auto rows = camel::server::outputRows(nativeSettings(settings_), server_->snapshot().devices);
    const auto row = std::ranges::find_if(
        rows, [&](const camel::server::OutputRow& r) { return r.backend == zone.backend && r.id == zone.output; });
    if (row == rows.end() || row->selected) return;
    const auto& output = *row;
    changeOutputs([&](camel::Settings& s, const DeviceCatalog& devices) {
        camel::server::selectOutput(s, output.backend, output.id, devices);
    });
    populateSettings();
}

// Selecting a zone makes its output the one in use; every zone keeps its queue and playback.
void MainWindow::selectZone(const std::string& id) {
    const auto row = std::ranges::find(zoneRows_, id, &ZoneRow::zone);
    if (row == zoneRows_.end()) return;
    const auto output = row->output;
    if (zonePicker_) zonePicker_.Hide();
    if (output.selected) return;
    const auto select = [this, output] {
        changeOutputs([&](camel::Settings& s, const DeviceCatalog& devices) {
            camel::server::selectOutput(s, output.backend, output.id, devices);
        });
        populateSettings();
    };
    if (pairable(output))
        askAirPlayPairing(output, outputLabel(output), select);
    else
        select();
}

} // namespace winrt::CamelPlayer::implementation
