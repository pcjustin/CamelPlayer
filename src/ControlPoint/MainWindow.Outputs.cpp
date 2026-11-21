// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pch.h"

#include "MainWindow.xaml.h"
#include "MainWindowParts.h"

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

void MainWindow::populateOutputs() {
    const auto rows = camel::server::outputRows(nativeSettings(settings_), server_->snapshot().devices);
    const auto inUse = std::ranges::find_if(rows, &camel::server::OutputRow::selected);
    if (inUse != rows.end()) {
        ToolTipService::SetToolTip(OutputButton(), box_value(outputMenuLabel(*inUse)));
        Automation::AutomationProperties::SetName(OutputButton(),
                                                  ui::format(L"Output_ButtonName", {std::wstring(outputMenuLabel(*inUse))}));
        dspOutputLabel_ = outputMenuLabel(*inUse);
        const hstring forOutput = ui::format(L"Dsp_ForOutput", {std::wstring(dspOutputLabel_)});
        DspOutputText().Text(forOutput);
        VolumeOutputText().Text(forOutput);
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

winrt::fire_and_forget MainWindow::askDeleteOutput(Backend backend, std::string id, hstring label) {
    auto strong = get_strong();
    ContentDialog dialog;
    ui::placeDialog(dialog, ContentLayout().XamlRoot());
    dialog.Title(box_value(ui::format(L"Output_DeleteTitle", {std::wstring(label)})));
    TextBlock explanation;
    explanation.Text(tr(L"Output_DeleteText"));
    explanation.TextWrapping(TextWrapping::Wrap);
    dialog.Content(explanation);
    dialog.PrimaryButtonText(tr(L"Output_Delete"));
    dialog.CloseButtonText(tr(L"Reset_Cancel"));
    dialog.DefaultButton(ContentDialogButton::Close);
    if (co_await ui::showDialog(dialog) != ContentDialogResult::Primary) co_return;
    changeOutputs(
        [&](camel::Settings& s, const DeviceCatalog& devices) { camel::server::deleteOutput(s, backend, id, devices); });
    populateSettings();
}

// A new menu at every opening, and the mouse chooses on its press: WinUI at times leaves a menu where a click only
// draws the item pressed, for the rest of the session, while the keyboard still chooses.
void MainWindow::OnOutputButtonClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    MenuFlyout menu;
    menu.Placement(Primitives::FlyoutPlacementMode::Top);
    const winrt::weak_ref<MenuFlyout> weakMenu = menu;
    const auto chosen = std::make_shared<bool>(false);
    const auto add = [&](MenuFlyoutItem const& item, std::function<void()> choose) {
        const auto once = [weakMenu, chosen, choose = std::move(choose)] {
            if (*chosen) return;
            *chosen = true;
            if (const auto shown = weakMenu.get()) shown.Hide();
            choose();
        };
        item.Click([once](auto&&, auto&&) { once(); });
        item.AddHandler(UIElement::PointerPressedEvent(),
                        box_value(Input::PointerEventHandler([once](auto&&, Input::PointerRoutedEventArgs const& e) {
                            if (e.Pointer().PointerDeviceType() == Microsoft::UI::Input::PointerDeviceType::Mouse &&
                                e.GetCurrentPoint(nullptr).Properties().IsLeftButtonPressed())
                                once();
                        })),
                        true);
        menu.Items().Append(item);
    };
    const auto rows = camel::server::outputRows(nativeSettings(settings_), server_->snapshot().devices);
    const auto grouped = std::ranges::count_if(rows, &camel::server::OutputRow::grouped);
    for (const auto& row : rows) {
        if (!row.enabled) continue;
        if (row.backend == Backend::AirPlay && settings_.backend == Backend::AirPlay) {
            // Receivers join and leave the group in use; the last one stays.
            ToggleMenuFlyoutItem item;
            item.Text(outputMenuText(row));
            item.IsChecked(row.grouped);
            item.IsEnabled(row.connected && (!row.grouped || grouped >= 2));
            add(item, [this, row] {
                const auto join = [this, row] {
                    changeOutputs([&](camel::Settings& s, const DeviceCatalog& devices) {
                        camel::server::setInAirPlayGroup(s, row.id, !row.grouped, devices);
                    });
                    populateSettings();
                };
                if (pairable(row) && !row.grouped)
                    askAirPlayPairing(row, outputLabel(row), join);
                else
                    join();
            });
            continue;
        }
        RadioMenuFlyoutItem item;
        item.GroupName(L"Outputs");
        item.Text(outputMenuText(row));
        item.IsChecked(row.selected);
        item.IsEnabled(row.connected);
        add(item, [this, row] {
            const auto select = [this, row] {
                if (row.selected) return;
                changeOutputs([&](camel::Settings& s, const DeviceCatalog& devices) {
                    camel::server::selectOutput(s, row.backend, row.id, devices);
                });
                populateSettings();
            };
            if (pairable(row))
                askAirPlayPairing(row, outputLabel(row), select);
            else
                select();
        });
    }
    menu.Items().Append(MenuFlyoutSeparator());
    MenuFlyoutItem open;
    open.Text(tr(L"Output_MenuSettings"));
    add(open, [this] { showSettingsCard(OutputsCard()); });
    menu.ShowAt(OutputButton());
}

} // namespace winrt::CamelPlayer::implementation
