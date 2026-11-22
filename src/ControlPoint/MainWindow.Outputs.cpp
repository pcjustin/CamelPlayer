// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pch.h"

#include "MainWindow.xaml.h"
#include "MainWindowParts.h"

#include <filesystem>
#include <utility>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>

#include "ArtworkLoader.h"
#include "DialogPlacement.h"
#include "UiText.h"
#include "Common/settings/Settings.h"
#include "Common/util/Log.h"
#include "Common/util/Text.h"
#include "Server/Network/Firewall.h"
#include "Server/Network/HQPlayer.h"
#include "Server/Playback/Outputs.h"

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

bool firewallRuleFound() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool found = camel::server::firewallRuleExists();
    if (SUCCEEDED(com)) CoUninitialize();
    return found;
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
    co_await winrt::resume_background();
    const bool found = firewallRuleFound();
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
    auto weak = get_weak();
    auto queue = DispatcherQueue();
    co_await winrt::resume_background();
    camel::server::addFirewallRule();
    const bool found = firewallRuleFound();
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
    if (zonePicker_) zonePicker_.Hide();
    Flyout picker;
    picker.Placement(Primitives::FlyoutPlacementMode::Top);
    zonePicker_ = picker;
    zoneRows_.clear();
    StackPanel content;
    content.Spacing(4);
    content.MinWidth(380);
    content.MaxWidth(460);
    Automation::AutomationProperties::SetName(content, tr(L"Zone_PickerName"));
    const auto snapshot = server_->snapshot();
    const auto outputs = camel::server::outputRows(nativeSettings(settings_), snapshot.devices);
    StackPanel rows;
    rows.Spacing(2);
    for (const auto& zone : snapshot.zones) {
        const auto output = std::ranges::find_if(
            outputs, [&](const camel::server::OutputRow& o) { return o.backend == zone.backend && o.id == zone.output; });
        if (output == outputs.end()) continue;
        ZoneRow row;
        row.zone = zone.id;
        row.output = *output;
        if (const auto saved = std::ranges::find_if(
                settings_.outputs, [&](const camel::SavedOutput& o) { return o.backend == zone.backend && o.id == zone.output; });
            saved != settings_.outputs.end()) {
            row.volumeMode = saved->volumeMode;
            row.volumeDb = saved->volumeDb;
        }
        Grid grid;
        grid.ColumnSpacing(8);
        for (const auto width : {GridLengthHelper::FromValueAndType(1, GridUnitType::Star), GridLengthHelper::Auto(),
                                 GridLengthHelper::Auto(), GridLengthHelper::Auto()}) {
            ColumnDefinition column;
            column.Width(width);
            grid.ColumnDefinitions().Append(column);
        }
        Grid face;
        face.ColumnSpacing(10);
        for (const auto width :
             {GridLengthHelper::Auto(), GridLengthHelper::Auto(), GridLengthHelper::FromValueAndType(1, GridUnitType::Star)}) {
            ColumnDefinition column;
            column.Width(width);
            face.ColumnDefinitions().Append(column);
        }
        row.mark = Shapes::Rectangle();
        row.mark.Width(3);
        row.mark.RadiusX(1.5);
        row.mark.RadiusY(1.5);
        row.mark.Fill(themeBrush(L"AccentFillColorDefaultBrush"));
        face.Children().Append(row.mark);
        Border frame;
        frame.Width(40);
        frame.Height(40);
        frame.CornerRadius(CornerRadiusHelper::FromUniformRadius(4));
        row.cover = Image();
        row.cover.Stretch(Media::Stretch::UniformToFill);
        frame.Child(row.cover);
        Grid::SetColumn(frame, 1);
        face.Children().Append(frame);
        StackPanel texts;
        texts.VerticalAlignment(VerticalAlignment::Center);
        row.title = TextBlock();
        row.title.TextTrimming(TextTrimming::CharacterEllipsis);
        row.detail = TextBlock();
        row.detail.TextTrimming(TextTrimming::CharacterEllipsis);
        row.detail.Style(Application::Current().Resources().Lookup(box_value(L"CaptionTextBlockStyle")).as<Style>());
        row.detail.Foreground(themeBrush(L"TextFillColorSecondaryBrush"));
        texts.Children().Append(row.title);
        texts.Children().Append(row.detail);
        Grid::SetColumn(texts, 2);
        face.Children().Append(texts);
        row.select = Button();
        row.select.Content(face);
        row.select.HorizontalAlignment(HorizontalAlignment::Stretch);
        row.select.HorizontalContentAlignment(HorizontalAlignment::Stretch);
        row.select.Background(Media::SolidColorBrush(Microsoft::UI::Colors::Transparent()));
        row.select.BorderThickness(ThicknessHelper::FromUniformLength(0));
        row.select.Padding(ThicknessHelper::FromLengths(4, 4, 4, 4));
        row.select.Click([this, id = zone.id](auto&&, auto&&) { selectZone(id); });
        grid.Children().Append(row.select);
        row.playIcon = SymbolIcon(Symbol::Play);
        row.playPause = Button();
        row.playPause.Content(row.playIcon);
        row.playPause.VerticalAlignment(VerticalAlignment::Center);
        row.playPause.Style(PlayPauseButton().Style());
        row.playPause.Click([this, id = zone.id](auto&&, auto&&) {
            Command command;
            command.kind = CommandKind::TogglePause;
            command.zone = id;
            submitCommand(std::move(command));
        });
        Grid::SetColumn(row.playPause, 1);
        grid.Children().Append(row.playPause);
        row.volume = Slider();
        row.volume.Width(96);
        row.volume.VerticalAlignment(VerticalAlignment::Center);
        row.volume.ValueChanged([this, id = zone.id](auto&&, Primitives::RangeBaseValueChangedEventArgs const& e) {
            if (!showingZones_) changeZoneVolume(id, e.NewValue());
        });
        Grid::SetColumn(row.volume, 2);
        grid.Children().Append(row.volume);
        Button more;
        more.Content(FontIcon());
        more.Content().as<FontIcon>().Glyph(L"\xE712");
        more.Style(PlayPauseButton().Style());
        more.VerticalAlignment(VerticalAlignment::Center);
        Automation::AutomationProperties::SetName(more, ui::format(L"Zone_MoreName", {std::wstring(outputMenuLabel(*output))}));
        MenuFlyout actions;
        MenuFlyoutSubItem transfer;
        transfer.Text(tr(L"Zone_Transfer"));
        fillTransferMenu(transfer, zone.id);
        transfer.IsEnabled(!zone.queue.entries.empty() && transfer.Items().Size() > 0);
        actions.Items().Append(transfer);
        more.Flyout(actions);
        Grid::SetColumn(more, 3);
        grid.Children().Append(more);
        rows.Children().Append(grid);
        zoneRows_.push_back(std::move(row));
    }
    content.Children().Append(rows);
    // The selected AirPlay group's receivers, which join and leave it; the last one stays. One another group plays
    // through is greyed and says which; one of a group that does not play moves from it into this one (MZ-R08).
    if (settings_.backend == Backend::AirPlay) {
        const auto grouped = std::ranges::count_if(outputs, &camel::server::OutputRow::grouped);
        StackPanel receivers;
        for (const auto& output : outputs) {
            if (!output.enabled || output.backend != Backend::AirPlay) continue;
            const auto holder = std::ranges::find_if(snapshot.zones, [&](const ZoneSnapshot& z) {
                return z.id != snapshot.selectedZone && z.backend == Backend::AirPlay &&
                       (z.output == output.id ||
                        std::ranges::find(z.playback.receivers, output.id, &ReceiverState::id) != z.playback.receivers.end());
            });
            const std::string from = holder == snapshot.zones.end() ? std::string() : holder->output;
            const bool held = holder != snapshot.zones.end() && holder->playback.state == PlaybackState::Playing;
            CheckBox join;
            join.Content(box_value(
                held ? ui::format(L"Zone_ReceiverInUse", {std::wstring(outputMenuText(output)), ui::utf8(holder->name).c_str()})
                     : outputMenuText(output)));
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
            receivers.Children().Append(join);
        }
        if (receivers.Children().Size()) {
            TextBlock heading;
            heading.Text(tr(L"Zone_GroupHeading"));
            heading.Style(Application::Current().Resources().Lookup(box_value(L"BodyStrongTextBlockStyle")).as<Style>());
            heading.Margin(ThicknessHelper::FromLengths(4, 8, 0, 0));
            content.Children().Append(heading);
            receivers.Margin(ThicknessHelper::FromLengths(4, 0, 0, 0));
            content.Children().Append(receivers);
        }
    }
    Grid foot;
    foot.Margin(ThicknessHelper::FromLengths(0, 8, 0, 0));
    pauseAllButton_ = Button();
    pauseAllButton_.Content(box_value(tr(L"Zone_PauseAll")));
    pauseAllButton_.Click([this](auto&&, auto&&) {
        Command command;
        command.kind = CommandKind::PauseAll;
        submitCommand(std::move(command));
    });
    foot.Children().Append(pauseAllButton_);
    HyperlinkButton open;
    open.Content(box_value(tr(L"Output_MenuSettings")));
    open.HorizontalAlignment(HorizontalAlignment::Right);
    open.Click([this](auto&&, auto&&) {
        if (zonePicker_) zonePicker_.Hide();
        showSettingsCard(OutputsCard());
    });
    foot.Children().Append(open);
    content.Children().Append(foot);
    picker.Content(content);
    // One closing as the next opens leaves the next one's rows.
    picker.Closed([this, weak = winrt::make_weak(picker)](auto&&, auto&&) {
        if (zonePicker_ != weak.get()) return;
        zonePicker_ = nullptr;
        zoneRows_.clear();
        pauseAllButton_ = nullptr;
    });
    showZoneRows(snapshot);
    picker.ShowAt(OutputButton());
}

// What each row shows as the zones play: the selected one marked, its track, its state or why it cannot play, and its
// volume where the zone has one of its own to set.
void MainWindow::showZoneRows(const Snapshot& snapshot) {
    if (!zonePicker_) return;
    showingZones_ = true;
    bool anyPlaying = false;
    for (auto& row : zoneRows_) {
        const auto found = std::ranges::find(snapshot.zones, row.zone, &ZoneSnapshot::id);
        if (found == snapshot.zones.end()) continue;
        const ZoneSnapshot& zone = *found;
        const bool selected = zone.id == snapshot.selectedZone;
        const auto& p = zone.playback;
        anyPlaying = anyPlaying || p.state == PlaybackState::Playing;
        const auto entry = std::ranges::find(zone.queue.entries, zone.queueEntryId, &QueueEntry::id);
        const bool hasEntry = entry != zone.queue.entries.end();
        // An AirPlay group is named by its receivers.
        const hstring name =
            zone.playback.receivers.size() > 1 ? ui::utf8(zone.name) + L" (AirPlay)" : outputMenuLabel(row.output);
        hstring track;
        if (hasEntry) {
            track = ui::utf8(entry->title.empty() ? std::filesystem::path(toWide(entry->source.pathUtf8())).stem().string()
                                                  : entry->title);
            if (!entry->artist.empty()) track = track + L" · " + ui::utf8(entry->artist);
        }
        const hstring state = !row.output.connected ? tr(L"Zone_Offline")
                              : !zone.inUseBy.empty()
                                  ? ui::format(L"Zone_InUseBy", {std::wstring(zoneName(snapshot, zone.inUseBy))})
                              : p.error            ? ui::errorTitle(*p.error)
                              : p.bufferingSeconds ? ui::format(L"State_Buffering", {std::to_wstring(*p.bufferingSeconds)})
                              : p.state == PlaybackState::Playing ? tr(L"State_Playing")
                              : p.state == PlaybackState::Paused  ? tr(L"State_Paused")
                                                                  : tr(L"State_Stopped");
        row.title.Text(name);
        row.title.FontWeight(selected ? Microsoft::UI::Text::FontWeights::SemiBold()
                                      : Microsoft::UI::Text::FontWeights::Normal());
        row.detail.Text(track.empty() ? state : track + L" · " + state);
        row.mark.Opacity(selected ? 1.0 : 0.0);
        row.select.IsEnabled(row.output.connected);
        Automation::AutomationProperties::SetName(
            row.select,
            ui::format(selected ? L"Zone_RowSelectedName" : L"Zone_RowName",
                       {std::wstring(name), std::wstring(track.empty() ? tr(L"Zone_NoTrack") : track), std::wstring(state)}));
        if (const std::string cover = hasEntry ? entry->source.pathUtf8() : std::string(); cover != row.coverPath) {
            row.coverPath = cover;
            row.cover.Source(nullptr);
            if (!cover.empty()) ui::loadArtwork(row.cover, server_, {ArtworkKind::Track, 0, cover}, {});
        }
        const bool playing = p.state == PlaybackState::Playing || p.bufferingSeconds;
        row.playIcon.Symbol(playing ? Symbol::Pause : Symbol::Play);
        const hstring action = tr(playing ? L"Tip_Pause" : L"Tip_Play");
        ToolTipService::SetToolTip(row.playPause, box_value(action));
        Automation::AutomationProperties::SetName(row.playPause,
                                                  ui::format(L"Zone_PlayName", {std::wstring(action), std::wstring(name)}));
        row.playPause.IsEnabled(hasEntry && row.output.connected);
        // Its volume: the player's own, else the software volume this zone keeps; none while fixed.
        const auto mode = selected ? settings_.volume.mode : row.volumeMode;
        if (p.playerVolume) {
            row.volume.Minimum(0);
            row.volume.Maximum(100);
            row.volume.StepFrequency(1);
            row.volume.Value(double(*p.playerVolume));
            row.volume.Visibility(Visibility::Visible);
        } else if (mode == camel::VolumeMode::Software) {
            row.volume.Minimum(camel::VolumeSettings::kMinDb);
            row.volume.Maximum(0);
            row.volume.StepFrequency(0.5);
            row.volume.Value(selected ? settings_.volume.db : row.volumeDb);
            row.volume.Visibility(Visibility::Visible);
        } else {
            row.volume.Visibility(Visibility::Collapsed);
        }
        Automation::AutomationProperties::SetName(row.volume, ui::format(L"Zone_VolumeName", {std::wstring(name)}));
    }
    if (pauseAllButton_) pauseAllButton_.IsEnabled(anyPlaying);
    showingZones_ = false;
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

// A row's slider: the player's own volume, or the software volume of its zone, the selected one's as the bar's slider sets
// it, another's in its saved output.
void MainWindow::changeZoneVolume(const std::string& id, double value) {
    const auto row = std::ranges::find(zoneRows_, id, &ZoneRow::zone);
    if (row == zoneRows_.end()) return;
    const auto snapshot = server_->snapshot();
    const auto zone = std::ranges::find(snapshot.zones, id, &ZoneSnapshot::id);
    if (zone == snapshot.zones.end()) return;
    if (zone->playback.playerVolume) {
        Command command;
        command.kind = CommandKind::SetPlayerVolume;
        command.zone = id;
        command.playerVolume = uint32_t(std::lround(value));
        submitCommand(std::move(command));
        return;
    }
    const double db = std::round(value * 2) / 2;
    if (id == snapshot.selectedZone) {
        VolumeSlider().Value(db);
        return;
    }
    for (auto& saved : settings_.outputs)
        if (saved.backend == row->output.backend && saved.id == row->output.id) saved.volumeDb = db;
    row->volumeDb = db;
    if (!eqTimer_.IsRunning()) eqTimer_.Start();
}

} // namespace winrt::CamelPlayer::implementation
