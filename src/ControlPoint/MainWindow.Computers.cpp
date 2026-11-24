// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// This computer's role and other computers controlling it (docs/Plan.md §17.8): Settings › This computer and others,
// pairing with another CamelPlayer, and switching roles, which restarts CamelPlayer.
#include "pch.h"

#include "MainWindow.xaml.h"

#include <shellapi.h>

#include <charconv>
#include <chrono>
#include <ctime>
#include <thread>

#include "DialogPlacement.h"
#include "ThemeBrush.h"
#include "UiText.h"
#include "Common/util/Log.h"
#include "Common/util/StartWithWindows.h"
#include "Common/util/Text.h"
#include "Server/Network/Firewall.h"
#include "Server/Remote/Discovery.h"
#include "Server/Remote/RemoteAccess.h"
#include "Server/Remote/RemoteServer.h"
#include "Server/Remote/ServerTokens.h"
#include "Server/Support/Updates.h"
#include "Common/Version.h"
#include "MainWindowParts.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace camel;
using namespace camel::server;
using ui::tr;

namespace winrt::CamelPlayer::implementation {

using namespace mainwindow;

namespace {

template <class F> auto withCom(F&& f) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto result = f();
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}

std::wstring lines(const std::vector<std::wstring>& names) {
    std::wstring list;
    for (const auto& name : names) list += (list.empty() ? L"" : L"\n") + name;
    return list;
}

std::wstring trimmed(hstring const& text) {
    std::wstring s(text);
    s.erase(0, s.find_first_not_of(L" \t"));
    s.erase(s.find_last_not_of(L" \t") + 1);
    return s;
}

// "host" or "host:port", as typed.
std::optional<ControlledServer> typedAddress(const std::wstring& text) {
    if (text.empty()) return std::nullopt;
    ControlledServer target;
    const auto utf8 = toUtf8(text);
    const auto colon = utf8.rfind(':');
    target.host = utf8.substr(0, colon);
    if (colon != std::string::npos) {
        unsigned port = 0;
        const auto digits = std::string_view(utf8).substr(colon + 1);
        const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), port);
        if (error != std::errc{} || end != digits.data() + digits.size() || port == 0 || port > 65535) return std::nullopt;
        target.port = uint16_t(port);
    }
    if (target.host.empty()) return std::nullopt;
    return target;
}

std::wstring clockTime(int64_t ms) {
    const auto seconds = std::time_t(ms / 1000);
    std::tm local{};
    localtime_s(&local, &seconds);
    wchar_t text[16];
    wcsftime(text, std::size(text), L"%H:%M", &local);
    return text;
}

std::wstring dateOf(int64_t ms) {
    const auto seconds = std::time_t(ms / 1000);
    std::tm local{};
    localtime_s(&local, &seconds);
    wchar_t text[32];
    wcsftime(text, std::size(text), L"%Y-%m-%d", &local);
    return text;
}

void openSettingsPage(const wchar_t* uri) {
    ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL);
}

ContentDialog question(XamlRoot const& root, hstring const& title, hstring const& text, hstring const& confirm) {
    ContentDialog dialog;
    ui::placeDialog(dialog, root);
    dialog.Title(box_value(title));
    TextBlock body;
    body.Text(text);
    body.TextWrapping(TextWrapping::Wrap);
    dialog.Content(body);
    dialog.PrimaryButtonText(confirm);
    dialog.CloseButtonText(tr(L"Reset_Cancel"));
    dialog.DefaultButton(ContentDialogButton::Primary);
    return dialog;
}

// The pairing dialog's controls and what its handlers share; copied into each coroutine, as the handlers' own
// captures may go before a coroutine resumes.
struct PairForm {
    ListView servers;
    TextBlock looking;
    TextBox address, name, code;
    TextBlock problem;
    std::shared_ptr<std::vector<FoundServer>> found;
    std::shared_ptr<std::optional<ControlledServer>> chosen;
    std::shared_ptr<bool> working;
    std::string ownId;                    // this computer's own Server, not offered
    std::shared_ptr<RemoteAccess> access; // to undo a pairing with it through a typed address
};

void say(const PairForm& form, hstring const& text, bool problem = true) {
    form.problem.Text(text);
    form.problem.Foreground(
        ui::themeBrush(form.problem, problem ? L"SystemFillColorCriticalBrush" : L"TextFillColorSecondaryBrush"));
    form.problem.Visibility(Visibility::Visible);
}

winrt::fire_and_forget lookForServers(PairForm form) {
    winrt::apartment_context foreground;
    form.looking.Text(tr(L"Pair_Looking"));
    form.looking.Visibility(Visibility::Visible);
    co_await winrt::resume_background();
    auto answered = findServers(std::chrono::milliseconds(1500));
    std::erase_if(answered, [&](const FoundServer& s) { return s.announced.id == form.ownId; });
    co_await foreground;
    *form.found = std::move(answered);
    form.servers.Items().Clear();
    for (const auto& s : *form.found)
        form.servers.Items().Append(
            box_value(toWide(s.announced.name + " · " + s.address + ":" + std::to_string(s.announced.port))));
    if (!form.found->empty()) form.servers.SelectedIndex(0);
    form.looking.Text(tr(L"Pair_NoneFound"));
    form.looking.Visibility(form.found->empty() ? Visibility::Visible : Visibility::Collapsed);
}

// Whether the token kept for `server` still opens it; one it no longer knows is forgotten.
bool stillPaired(RemoteServer::Options options, ServerTokens& tokens, const std::string& server) {
    const auto kept = tokens.token(server);
    if (!kept) return false;
    options.token = *kept;
    try {
        RemoteServer(options).shutdown();
        return true;
    } catch (const NotPaired&) {
        tokens.forget(server);
        return false;
    }
}

winrt::fire_and_forget tryPairing(PairForm form, ContentDialog shown) {
    if (*form.working) co_return;
    std::optional<ControlledServer> target;
    if (const auto typed = trimmed(form.address.Text()); !typed.empty()) {
        target = typedAddress(typed);
        if (!target) {
            say(form, ui::format(L"Pair_Unreached", {typed}));
            co_return;
        }
    } else if (const auto index = form.servers.SelectedIndex(); index >= 0 && size_t(index) < form.found->size()) {
        const auto& s = (*form.found)[size_t(index)];
        target = ControlledServer{s.announced.id, s.announced.name, s.address, s.announced.port};
    } else {
        say(form, tr(L"Pair_ChooseOne"));
        co_return;
    }
    auto deviceName = toUtf8(trimmed(form.name.Text()));
    if (deviceName.empty()) deviceName = computerName();
    const auto codeText = toUtf8(trimmed(form.code.Text()));
    *form.working = true;
    shown.IsPrimaryButtonEnabled(false);
    say(form, tr(L"Pair_Working"), false);
    auto server = *target;
    winrt::apartment_context foreground;
    co_await winrt::resume_background();
    std::optional<PairingRefused> refused;
    std::string failure;
    bool self = false;
    try {
        // A Server this computer paired with before takes it back without a code.
        ServerTokens tokens(dataDirectory() / L"server-tokens");
        RemoteServer::Options options;
        options.host = server.host;
        options.port = server.port;
        options.cache = dataDirectory() / L"remote-cache";
        if (server.id.empty() || !stillPaired(options, tokens, server.id)) {
            const auto paired = RemoteServer::pair(server.host, server.port, deviceName, codeText);
            options.token = paired.token;
            RemoteServer probe(options);
            const auto hello = probe.hello();
            server.id = hello["server"].value("id", std::string());
            server.name = hello["server"].value("name", server.host);
            probe.shutdown();
            self = !form.ownId.empty() && server.id == form.ownId;
            if (self && form.access)
                form.access->revokeDevice(paired.device);
            else if (!self)
                tokens.set(server.id, paired.token);
        }
    } catch (const PairingRefused& r) {
        refused = r;
    } catch (const std::exception& e) {
        log::warn("pairing with %s: %s", server.host.c_str(), e.what());
        failure = e.what();
    }
    co_await foreground;
    *form.working = false;
    shown.IsPrimaryButtonEnabled(true);
    if (self) {
        say(form, tr(L"Pair_Self"));
    } else if (!failure.empty()) {
        say(form, ui::format(L"Pair_Unreached", {toWide(server.host + ":" + std::to_string(server.port))}));
    } else if (refused) {
        form.code.Visibility(Visibility::Visible);
        if (refused->code == "codeNeeded") {
            say(form, tr(L"Pair_CodeNeeded"));
        } else if (refused->code == "wrongCode") {
            say(form, tr(L"Pair_WrongCode"));
        } else if (refused->code == "stopped") {
            say(form, ui::format(L"Pair_Stopped", {clockTime(refused->until)}));
        } else if (refused->code == "nameTaken") {
            say(form, ui::format(L"Pair_NameTaken", {toWide(deviceName)}));
            form.name.Text(toWide(refused->suggestion));
            if (codeText.empty()) form.code.Visibility(Visibility::Collapsed);
        } else {
            say(form, ui::format(L"Pair_Unreached", {toWide(server.host)}));
        }
    } else {
        *form.chosen = server;
        shown.Hide();
    }
}

// The folder browser of a Control point: the Server's folders, read through it.
struct FolderForm {
    std::shared_ptr<IServer> server;
    TextBox path;
    ListView folders;
    TextBlock problem;
    std::shared_ptr<std::vector<ServerFolder>> listed;
};

// One level up: a drive's root goes to the drives, a share to its computer's shares.
std::wstring folderAbove(std::wstring path) {
    while (path.size() > 3 && (path.ends_with(L'\\') || path.ends_with(L'/'))) path.pop_back();
    if (path.size() <= 3 && path.size() >= 2 && path[1] == L':') return {};
    const auto cut = path.find_last_of(L"\\/");
    if (cut == std::wstring::npos || cut < 2) return {};
    const auto above = path.substr(0, cut);
    return above.size() == 2 && above[1] == L':' ? above + L"\\" : above;
}

winrt::fire_and_forget showFolders(FolderForm form, std::wstring path) {
    winrt::apartment_context foreground;
    form.path.Text(path);
    form.problem.Visibility(Visibility::Collapsed);
    co_await winrt::resume_background();
    std::vector<ServerFolder> listed;
    std::string failure;
    try {
        listed = form.server->serverFolders(toUtf8(path));
    } catch (const std::exception& e) {
        failure = e.what();
    }
    co_await foreground;
    if (std::wstring(form.path.Text()) != path) co_return; // another folder was asked for meanwhile
    *form.listed = std::move(listed);
    form.folders.Items().Clear();
    for (const auto& folder : *form.listed) form.folders.Items().Append(box_value(toWide(folder.name)));
    if (!failure.empty()) {
        form.problem.Text(ui::format(L"ServerFolder_Unreadable", {path}));
        form.problem.Visibility(Visibility::Visible);
    }
}

} // namespace

std::shared_ptr<DevicePairing> MainWindow::devicePairing() const {
    if (access_) return access_;
    return std::dynamic_pointer_cast<RemoteServer>(server_);
}

std::string MainWindow::ownDevice() const {
    const auto remote = std::dynamic_pointer_cast<RemoteServer>(server_);
    return remote ? remote->device() : std::string();
}

// Another computer's Server that does not answer is shown as such until it is back, which RemoteServer waits for.
void MainWindow::showLink() {
    const auto link = server_->link();
    if (shownLink_ == link) return;
    shownLink_ = link;
    const auto name = toWide(settings_.controlled.name);
    const auto remote = std::dynamic_pointer_cast<RemoteServer>(server_);
    const auto version = toWide(remote ? remote->serverVersion() : std::string());
    switch (link) {
    case IServer::Link::Refused:
        ConnectionBar().Title(ui::format(L"Link_RefusedTitle", {name}));
        ConnectionBar().Message(tr(L"Link_RefusedText"));
        break;
    case IServer::Link::ServerTooOld:
        ConnectionBar().Title(ui::format(L"Control_ServerTooOldTitle", {name}));
        ConnectionBar().Message(ui::format(L"Control_ServerTooOldText", {version, L"" CAMEL_VERSION_STRING}));
        break;
    case IServer::Link::ThisTooOld:
        ConnectionBar().Title(ui::format(L"Control_ThisTooOldTitle", {name}));
        ConnectionBar().Message(ui::format(L"Control_ThisTooOldText", {version, L"" CAMEL_VERSION_STRING}));
        break;
    default:
        ConnectionBar().Title(ui::format(L"Link_LostTitle", {name}));
        ConnectionBar().Message(tr(L"Link_LostText"));
        break;
    }
    ConnectionBar().IsOpen(link != IServer::Link::Connected);
}

// What another device did to the music this window follows, or to all of it, as "Paused by Study PC" (docs/Plan.md
// §17.8). The changes made before the window opened, or before it followed its zone, are passed over.
void MainWindow::showChanges(const Snapshot& snapshot) {
    const auto zoneNamed = [&](const std::string& id) {
        const auto zone = std::ranges::find(snapshot.zones, id, &ZoneSnapshot::id);
        return zone == snapshot.zones.end() ? nullptr : &*zone;
    };
    const ZoneSnapshot* followed = zoneNamed(snapshot.selectedZone);
    const uint64_t latest = followed ? followed->change.serial : 0;
    const auto nameOf = [&](const ZoneChange& change) {
        return toWide(change.name.empty() ? settings_.controlled.name : change.name);
    };
    std::optional<hstring> said;
    if (!changesKnown_) {
        changesKnown_ = true;
        shownNotice_ = snapshot.notice.serial;
    } else if (snapshot.selectedZone != changesZone_ && snapshot.transfer.serial != shownTransfer_ &&
               snapshot.transfer.from == changesZone_ && snapshot.transfer.to == snapshot.selectedZone) {
        // The queue this window followed went to another zone, and the window with it.
        const ZoneSnapshot* from = zoneNamed(snapshot.transfer.from);
        if (from && from->change.device != ownDevice())
            said = ui::format(L"Change_moved", {nameOf(from->change), followed ? toWide(followed->name) : std::wstring()});
    } else if (snapshot.selectedZone == changesZone_ && latest > shownChange_ && followed->change.device != ownDevice()) {
        said = ui::format((L"Change_" + toWide(followed->change.kind)).c_str(), {nameOf(followed->change)});
    }
    if (snapshot.notice.serial > shownNotice_ && snapshot.notice.device != ownDevice())
        said = ui::format((L"Change_" + toWide(snapshot.notice.kind)).c_str(),
                          {nameOf(snapshot.notice), toWide(snapshot.notice.zone)});
    changesZone_ = snapshot.selectedZone;
    shownChange_ = latest;
    shownTransfer_ = snapshot.transfer.serial;
    shownNotice_ = snapshot.notice.serial;
    if (!said) return;
    ChangeBar().Message(*said);
    ChangeBar().IsOpen(true);
}

void MainWindow::useRemoteAccess(std::shared_ptr<RemoteAccess> access) {
    access_ = std::move(access);
    // A Control point's Update Server confirmed there what stops; this window only steps aside for the updater.
    if (access_)
        access_->setInstall(
            [weak = get_weak(), queue = DispatcherQueue()](const update::Release& release, const update::Prepared& prepared) {
                queue.TryEnqueue([weak, release, prepared] {
                    const auto self = weak.get();
                    if (!self) return;
                    std::wstring executable(32768, L'\0');
                    executable.resize(GetModuleFileNameW(nullptr, executable.data(), DWORD(executable.size())));
                    try {
                        update::startUpdater(std::filesystem::path(executable).parent_path(), prepared, dataDirectory(),
                                             release.version, CAMEL_VERSION_STRING);
                    } catch (const std::exception& e) {
                        log::warn("update: %s", e.what());
                        return;
                    }
                    log::info("update: CamelUpdater takes over to install %s", release.version.c_str());
                    self->exiting_ = true;
                    self->Close();
                });
            });
    populateComputers();
}

void MainWindow::populateComputers() {
    const auto remote = std::dynamic_pointer_cast<RemoteServer>(server_);
    const bool wasPopulating = std::exchange(populating_, true);
    const auto& controlled = settings_.controlled;
    if (remote) {
        RoleText().Text(ui::format(L"Role_Control",
                                   {toWide(controlled.name), toWide(controlled.host + ":" + std::to_string(controlled.port))}));
        const auto allow = box_value(ui::format(L"Control_AllowOn", {toWide(controlled.name)}));
        AllowControlSwitch().OnContent(allow);
        AllowControlSwitch().OffContent(allow);
    } else {
        RoleText().Text(tr(L"Role_Play"));
        const auto allow = box_value(tr(L"Control_Allow"));
        AllowControlSwitch().OnContent(allow);
        AllowControlSwitch().OffContent(allow);
    }
    PlayHereButton().Visibility(remote ? Visibility::Visible : Visibility::Collapsed);
    UpdateServerButton().Visibility(remote ? Visibility::Visible : Visibility::Collapsed);
    // A Control point older than its Server offers to update itself (docs/Plan.md §17.8).
    if (const auto theirs = remote ? update::parseVersion(remote->serverVersion()) : std::nullopt;
        theirs && !offeredOwnUpdate_ && *theirs > update::parseVersion(CAMEL_VERSION_STRING).value_or(update::Version{})) {
        offeredOwnUpdate_ = true;
        checkForUpdates(true);
    }
    AllowControlSwitch().IsOn(settings_.allowControl);
    AllowControlSwitch().IsEnabled(remote || access_);
    populating_ = wasPopulating;
    if (!settings_.allowControl) {
        AllowControlText().Visibility(Visibility::Collapsed);
        PairedPanel().Visibility(Visibility::Collapsed);
        ControlFirewallNotice().IsOpen(false);
        PublicNetworkNotice().IsOpen(false);
        return;
    }
    refreshComputers();
}

// The address, the firewall, the network and the paired devices, read in the background.
winrt::fire_and_forget MainWindow::refreshComputers() {
    auto strong = get_strong();
    winrt::apartment_context foreground;
    const auto serial = ++computersRefresh_;
    const auto access = access_;
    const auto pairing = devicePairing();
    const auto remote = std::dynamic_pointer_cast<RemoteServer>(server_);
    if (!pairing) co_return;
    co_await winrt::resume_background();
    std::optional<uint16_t> port;
    bool rule = true, onPublic = false;
    if (access) {
        // Opened by RemoteAccess's own thread once the setting has reached the Server.
        for (int i = 0; i < 30 && !(port = access->port()); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        rule = withCom([] { return firewallRuleExists(); });
        onPublic = withCom([] { return publicNetworkConnected(); });
    }
    nlohmann::json devices = nlohmann::json::array();
    try {
        devices = pairing->pairedDevices();
    } catch (const std::exception& e) {
        log::warn("paired devices: %s", e.what());
    }
    const auto self = remote ? remote->device() : std::string();
    std::string ownName;
    for (const auto& device : devices)
        if (!self.empty() && device.value("id", std::string()) == self) ownName = device.value("name", std::string());
    const auto addresses = lanAddresses();
    co_await foreground;
    if (serial != computersRefresh_ || !settings_.allowControl) co_return;
    if (access) {
        std::wstring reach;
        for (const auto& address : addresses)
            reach += (reach.empty() ? L"" : L", ") + toWide(address) + L":" + std::to_wstring(port.value_or(0));
        AllowControlText().Text(port ? ui::format(L"Control_Address", {reach}) : tr(L"Control_NotServed"));
        AllowControlText().Visibility(Visibility::Visible);
    }
    if (access) firewallRule_ = rule;
    ControlFirewallNotice().IsOpen(access && !rule);
    RenamePanel().Visibility(ownName.empty() ? Visibility::Collapsed : Visibility::Visible);
    if (const auto root = Content().XamlRoot();
        !ownName.empty() && (!root || Input::FocusManager::GetFocusedElement(root) != DeviceNameBox()))
        DeviceNameBox().Text(toWide(ownName));
    PublicNetworkNotice().IsOpen(onPublic);
    auto rows = PairedDeviceRows().Children();
    rows.Clear();
    if (devices.empty()) {
        TextBlock none;
        none.Text(tr(L"Paired_None"));
        none.TextWrapping(TextWrapping::Wrap);
        none.Foreground(themeBrush(L"TextFillColorSecondaryBrush"));
        rows.Append(none);
    }
    const auto snapshot = server_->snapshot();
    for (const auto& device : devices) {
        const auto id = device.value("id", std::string());
        auto name = toWide(device.value("name", std::string()));
        if (!self.empty() && id == self) name = std::wstring(ui::format(L"Paired_This", {name}));
        const bool connected = device.value("connected", false);
        const auto followed = std::ranges::find(snapshot.zones, device.value("zone", std::string()), &ZoneSnapshot::id);
        Grid row;
        row.ColumnSpacing(12);
        row.ColumnDefinitions().Append(ColumnDefinition());
        ColumnDefinition button;
        button.Width(GridLengthHelper::Auto());
        row.ColumnDefinitions().Append(button);
        TextBlock text;
        text.VerticalAlignment(VerticalAlignment::Center);
        text.TextWrapping(TextWrapping::Wrap);
        text.Text(!connected ? ui::format(L"Paired_Since", {name, dateOf(device.value("paired", int64_t{0}))})
                  : followed != snapshot.zones.end() ? ui::format(L"Paired_ConnectedZone", {name, toWide(followed->name)})
                                                     : ui::format(L"Paired_Connected", {name}));
        row.Children().Append(text);
        Button revoke;
        revoke.Content(box_value(tr(L"Paired_Revoke")));
        Automation::AutomationProperties::SetName(revoke, ui::format(L"Paired_RevokeTitle", {name}));
        Grid::SetColumn(revoke, 1);
        revoke.Click([this, id, name, mine = !self.empty() && id == self](auto&&, auto&&) { revokeDevice(id, name, mine); });
        row.Children().Append(revoke);
        rows.Append(row);
    }
    PairedPanel().Visibility(Visibility::Visible);
}

void MainWindow::OnAllowControlToggled(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    if (populating_) return;
    if (AllowControlSwitch().IsOn())
        allowControl();
    else
        stopControl();
}

// The Server's own window: the setting first, then what lets other computers in, each asked once.
winrt::fire_and_forget MainWindow::allowControl() {
    auto strong = get_strong();
    winrt::apartment_context foreground;
    settings_.allowControl = true;
    applySettingsNow();
    co_await winrt::resume_background();
    const bool rule = withCom([] { return firewallRuleExists(); });
    const bool onPublic = withCom([] { return publicNetworkConnected(); });
    co_await foreground;
    const auto root = ContentLayout().XamlRoot();
    if (!rule && co_await ui::showDialog(question(root, tr(L"Control_FirewallTitle"), tr(L"Control_FirewallText"),
                                                  tr(L"Firewall_AddButton/Content"))) == ContentDialogResult::Primary)
        co_await addControlFirewallRule();
    if (!camel::startup::state().on() && !settings_.startWithWindowsAsked) {
        auto dialog = question(root, tr(L"Control_StartTitle"), tr(L"Control_StartText"), tr(L"Control_StartButton"));
        dialog.SecondaryButtonText(tr(L"Control_SignInOptions"));
        dialog.CloseButtonText(tr(L"Control_NotNow"));
        const auto answer = co_await ui::showDialog(dialog);
        if (answer == ContentDialogResult::Primary) {
            wchar_t program[MAX_PATH];
            if (GetModuleFileNameW(nullptr, program, MAX_PATH)) camel::startup::enable(program);
            showStartWithWindows();
        } else if (answer == ContentDialogResult::Secondary) {
            openSettingsPage(L"ms-settings:signinoptions");
        }
        settings_.startWithWindowsAsked = true;
        applySettingsNow();
    }
    if (onPublic) {
        auto dialog = question(root, tr(L"Control_PublicNotice/Title"), tr(L"Control_PublicNotice/Message"),
                               tr(L"Control_NetworkSettings/Content"));
        dialog.CloseButtonText(tr(L"Control_NotNow"));
        if (co_await ui::showDialog(dialog) == ContentDialogResult::Primary) openSettingsPage(L"ms-settings:network-status");
    }
    refreshComputers();
}

// Turning it off, here or from a Control point, names the devices it cuts off, this one among them (the owner,
// 2026-11-22).
winrt::fire_and_forget MainWindow::stopControl() {
    auto strong = get_strong();
    winrt::apartment_context foreground;
    const auto pairing = devicePairing();
    const auto remote = std::dynamic_pointer_cast<RemoteServer>(server_);
    co_await winrt::resume_background();
    std::vector<std::wstring> cut;
    try {
        const auto self = remote ? remote->device() : std::string();
        if (pairing)
            for (const auto& device : pairing->pairedDevices())
                if (device.value("connected", false)) {
                    const auto name = toWide(device.value("name", std::string()));
                    cut.push_back(device.value("id", std::string()) == self ? std::wstring(ui::format(L"Paired_This", {name}))
                                                                            : name);
                }
    } catch (const std::exception& e) {
        log::warn("paired devices: %s", e.what());
    }
    co_await foreground;
    const auto text = cut.empty() ? tr(L"Control_StopNone") : ui::format(L"Control_StopCut", {lines(cut)});
    auto dialog = question(ContentLayout().XamlRoot(), tr(L"Control_StopTitle"), text, tr(L"Control_StopButton"));
    dialog.DefaultButton(ContentDialogButton::Close);
    if (co_await ui::showDialog(dialog) != ContentDialogResult::Primary) {
        populateComputers();
        co_return;
    }
    settings_.allowControl = false;
    applySettingsNow();
    populateComputers();
}

void MainWindow::OnAddControlFirewallRuleClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    addControlFirewallRule();
}

Windows::Foundation::IAsyncAction MainWindow::addControlFirewallRule() {
    auto strong = get_strong();
    winrt::apartment_context foreground;
    co_await winrt::resume_background();
    camel::server::addFirewallRule();
    const bool found = withCom([] { return firewallRuleExists(); });
    co_await foreground;
    firewallRule_ = found;
    populateOutputs();
    refreshComputers();
}

void MainWindow::OnNetworkSettingsClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    openSettingsPage(L"ms-settings:network-status");
}

winrt::fire_and_forget MainWindow::OnShowPairingCodeClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    auto strong = get_strong();
    winrt::apartment_context foreground;
    const auto pairing = devicePairing();
    if (!pairing) co_return;
    co_await winrt::resume_background();
    std::string code, failure;
    try {
        code = pairing->pairingCode();
    } catch (const std::exception& e) {
        failure = e.what();
    }
    co_await foreground;
    ContentDialog dialog;
    ui::placeDialog(dialog, ContentLayout().XamlRoot());
    dialog.Title(box_value(tr(L"Pairing_CodeTitle")));
    StackPanel content;
    content.Spacing(12);
    if (!code.empty()) {
        TextBlock digits;
        digits.Text(toWide(code.substr(0, 3) + " " + code.substr(3)));
        digits.FontSize(40);
        digits.IsTextSelectionEnabled(true);
        Automation::AutomationProperties::SetName(digits, toWide(code).c_str());
        content.Children().Append(digits);
    }
    TextBlock text;
    text.Text(code.empty() ? ui::format(L"Pairing_Failed", {toWide(failure)}) : tr(L"Pairing_CodeText"));
    text.TextWrapping(TextWrapping::Wrap);
    content.Children().Append(text);
    dialog.Content(content);
    dialog.CloseButtonText(tr(L"Pairing_Done"));
    co_await ui::showDialog(dialog);
}

winrt::fire_and_forget MainWindow::OnResetOwnerClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    auto strong = get_strong();
    winrt::apartment_context foreground;
    const auto pairing = devicePairing();
    const auto remote = std::dynamic_pointer_cast<RemoteServer>(server_);
    if (!pairing) co_return;
    co_await winrt::resume_background();
    std::vector<std::wstring> names;
    try {
        const auto self = remote ? remote->device() : std::string();
        for (const auto& device : pairing->pairedDevices()) {
            const auto name = toWide(device.value("name", std::string()));
            names.push_back(device.value("id", std::string()) == self ? std::wstring(ui::format(L"Paired_This", {name})) : name);
        }
    } catch (const std::exception& e) {
        log::warn("paired devices: %s", e.what());
    }
    co_await foreground;
    const auto text = names.empty() ? tr(L"Paired_ResetEmpty") : ui::format(L"Paired_ResetText", {lines(names)});
    auto dialog = question(ContentLayout().XamlRoot(), tr(L"Paired_ResetTitle"), text, tr(L"Paired_Reset"));
    dialog.DefaultButton(ContentDialogButton::Close);
    if (co_await ui::showDialog(dialog) != ContentDialogResult::Primary) co_return;
    co_await winrt::resume_background();
    try {
        pairing->resetOwner();
    } catch (const std::exception& e) {
        log::warn("reset owner: %s", e.what());
    }
    co_await foreground;
    refreshComputers();
}

winrt::fire_and_forget MainWindow::revokeDevice(std::string id, std::wstring name, bool mine) {
    auto strong = get_strong();
    winrt::apartment_context foreground;
    const auto pairing = devicePairing();
    if (!pairing) co_return;
    auto dialog = question(ContentLayout().XamlRoot(), ui::format(L"Paired_RevokeTitle", {name}),
                           mine ? tr(L"Paired_RevokeSelfText") : ui::format(L"Paired_RevokeText", {name}), tr(L"Paired_Revoke"));
    dialog.DefaultButton(ContentDialogButton::Close);
    if (co_await ui::showDialog(dialog) != ContentDialogResult::Primary) co_return;
    co_await winrt::resume_background();
    try {
        pairing->revokeDevice(id);
    } catch (const std::exception& e) {
        log::warn("revoke: %s", e.what());
    }
    co_await foreground;
    refreshComputers();
}

// A Control point's name, which no other paired device may have (docs/Plan.md §17.8).
winrt::fire_and_forget MainWindow::OnRenameDeviceClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    auto strong = get_strong();
    const auto remote = std::dynamic_pointer_cast<RemoteServer>(server_);
    const auto name = toUtf8(trimmed(DeviceNameBox().Text()));
    if (!remote || name.empty()) co_return;
    winrt::apartment_context foreground;
    co_await winrt::resume_background();
    nlohmann::json answer;
    try {
        answer = remote->renameDevice(name);
    } catch (const std::exception& e) {
        log::warn("rename: %s", e.what());
    }
    co_await foreground;
    if (answer.contains("error")) {
        DeviceNameBox().Text(toWide(answer.value("suggestion", std::string())));
        RenameText().Text(ui::format(L"Pair_NameTaken", {toWide(name)}));
        RenameText().Visibility(Visibility::Visible);
        co_return;
    }
    RenameText().Visibility(Visibility::Collapsed);
    refreshComputers();
}

// Update Server (docs/Plan.md §17.8): the Server finds the release itself; what stops is said here first, and a Server
// only an administrator may update is left to be updated at its own computer.
winrt::fire_and_forget MainWindow::OnUpdateServerClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    auto strong = get_strong();
    const auto remote = std::dynamic_pointer_cast<RemoteServer>(server_);
    if (!remote) co_return;
    const auto name = toWide(settings_.controlled.name);
    winrt::apartment_context foreground;
    UpdateServerButton().IsEnabled(false);
    co_await winrt::resume_background();
    nlohmann::json state;
    std::string failure;
    try {
        state = remote->serverUpdate();
    } catch (const std::exception& e) {
        failure = e.what();
    }
    co_await foreground;
    UpdateServerButton().IsEnabled(true);
    const auto root = ContentLayout().XamlRoot();
    if (!failure.empty()) {
        co_await ui::showDialog(textDialog(root, ui::format(L"UpdateServer_FailedTitle", {name}),
                                           ui::format(L"UpdateServer_Failed", {name, toWide(failure)})));
        co_return;
    }
    const auto current = toWide(state.value("version", std::string()));
    if (state["release"].is_null()) {
        co_await ui::showDialog(textDialog(root, ui::format(L"UpdateServer_UpToDateTitle", {name}),
                                           ui::format(L"UpdateServer_UpToDateText", {current})));
        co_return;
    }
    const auto version = toWide(state["release"].value("version", std::string()));
    if (state.value("administrator", false)) {
        co_await ui::showDialog(textDialog(root, ui::format(L"UpdateServer_AdministratorTitle", {name}),
                                           ui::format(L"UpdateServer_AdministratorText", {version})));
        co_return;
    }
    const auto listed = [&](const char* key) {
        std::vector<std::wstring> names;
        for (const auto& item : state.value(key, nlohmann::json::array())) names.push_back(toWide(item.get<std::string>()));
        return lines(names);
    };
    std::wstring text(tr(L"UpdateServer_Text"));
    if (const auto playing = listed("playing"); !playing.empty())
        text += L"\n\n" + std::wstring(ui::format(L"UpdateServer_Playing", {playing}));
    if (const auto connected = listed("connected"); !connected.empty())
        text += L"\n\n" + std::wstring(ui::format(L"UpdateServer_Connected", {connected}));
    auto dialog = question(root, ui::format(L"UpdateServer_Title", {name, version}), hstring(text), tr(L"UpdateServer_Button"));
    if (co_await ui::showDialog(dialog) != ContentDialogResult::Primary) co_return;
    UpdateServerButton().IsEnabled(false);
    co_await winrt::resume_background();
    try {
        remote->updateServer();
    } catch (const std::exception& e) {
        failure = e.what();
    }
    co_await foreground;
    UpdateServerButton().IsEnabled(true);
    co_await ui::showDialog(textDialog(root, ui::format(L"UpdateServer_Title", {name, version}),
                                       failure.empty() ? ui::format(L"UpdateServer_Started", {name, version})
                                                       : ui::format(L"UpdateServer_Failed", {name, toWide(failure)})));
}

void MainWindow::OnControlAnotherClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    chooseServerToControl();
}

void MainWindow::OnPlayHereClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    playHere();
}

// The CamelPlayers mDNS finds, or a typed address; pairing asks for a code only once the Server has an owner, and for
// another name when a paired device has this one.
winrt::fire_and_forget MainWindow::chooseServerToControl() {
    auto strong = get_strong();
    const auto ownId = access_ ? access_->serverId() : std::string();
    const auto controlledId = std::dynamic_pointer_cast<RemoteServer>(server_) ? settings_.controlled.id : std::string();
    ContentDialog dialog;
    ui::placeDialog(dialog, ContentLayout().XamlRoot());
    dialog.Title(box_value(tr(L"Pair_Title")));
    StackPanel content;
    content.Spacing(12);
    content.MinWidth(420);
    TextBlock foundHeader;
    foundHeader.Text(tr(L"Pair_Found"));
    content.Children().Append(foundHeader);
    ListView servers;
    servers.SelectionMode(ListViewSelectionMode::Single);
    servers.MaxHeight(180);
    Automation::AutomationProperties::SetName(servers, tr(L"Pair_Found"));
    content.Children().Append(servers);
    TextBlock looking;
    looking.TextWrapping(TextWrapping::Wrap);
    looking.Foreground(themeBrush(L"TextFillColorSecondaryBrush"));
    content.Children().Append(looking);
    Button again;
    again.Content(box_value(tr(L"Pair_LookAgain")));
    content.Children().Append(again);
    TextBox address;
    address.Header(box_value(tr(L"Pair_Address")));
    address.PlaceholderText(tr(L"Pair_AddressPlaceholder"));
    content.Children().Append(address);
    TextBox name;
    name.Header(box_value(tr(L"Pair_Name")));
    name.Text(toWide(computerName()));
    content.Children().Append(name);
    TextBox code;
    code.Header(box_value(tr(L"Pair_Code")));
    code.MaxLength(6);
    code.Visibility(Visibility::Collapsed);
    content.Children().Append(code);
    TextBlock problem;
    problem.TextWrapping(TextWrapping::Wrap);
    problem.Visibility(Visibility::Collapsed);
    content.Children().Append(problem);
    dialog.Content(content);
    dialog.PrimaryButtonText(tr(L"Pair_Connect"));
    dialog.CloseButtonText(tr(L"Reset_Cancel"));
    dialog.DefaultButton(ContentDialogButton::Primary);

    PairForm form{servers,
                  looking,
                  address,
                  name,
                  code,
                  problem,
                  std::make_shared<std::vector<FoundServer>>(),
                  std::make_shared<std::optional<ControlledServer>>(),
                  std::make_shared<bool>(false),
                  ownId,
                  access_};
    again.Click([form](auto&&, auto&&) { lookForServers(form); });
    lookForServers(form);
    dialog.PrimaryButtonClick([form](ContentDialog const& shown, ContentDialogButtonClickEventArgs const& e) {
        e.Cancel(true);
        tryPairing(form, shown);
    });
    const auto chosen = form.chosen;
    co_await ui::showDialog(dialog);
    if (*chosen && chosen->value().id != controlledId) switchToControl(chosen->value());
}

// The drives, shares and folders of the Server's computer, as its Windows user reaches them; no password is asked for
// or sent (docs/Plan.md §17.8).
winrt::fire_and_forget MainWindow::chooseServerFolder() {
    auto strong = get_strong();
    ContentDialog dialog;
    ui::placeDialog(dialog, ContentLayout().XamlRoot());
    dialog.Title(box_value(ui::format(L"ServerFolder_Title", {toWide(settings_.controlled.name)})));
    StackPanel content;
    content.Spacing(12);
    content.MinWidth(460);
    TextBlock note;
    note.Text(tr(L"ServerFolder_Note"));
    note.TextWrapping(TextWrapping::Wrap);
    content.Children().Append(note);
    Grid where;
    where.ColumnSpacing(8);
    where.ColumnDefinitions().Append(ColumnDefinition());
    ColumnDefinition upColumn;
    upColumn.Width(GridLengthHelper::Auto());
    where.ColumnDefinitions().Append(upColumn);
    TextBox path;
    path.Header(box_value(tr(L"ServerFolder_Path")));
    path.PlaceholderText(tr(L"ServerFolder_PathPlaceholder"));
    where.Children().Append(path);
    Button up;
    up.Content(box_value(tr(L"ServerFolder_Up")));
    up.VerticalAlignment(VerticalAlignment::Bottom);
    Grid::SetColumn(up, 1);
    where.Children().Append(up);
    content.Children().Append(where);
    ListView folders;
    folders.SelectionMode(ListViewSelectionMode::None);
    folders.IsItemClickEnabled(true);
    folders.Height(260);
    Automation::AutomationProperties::SetName(folders, tr(L"ServerFolder_List"));
    content.Children().Append(folders);
    TextBlock problem;
    problem.TextWrapping(TextWrapping::Wrap);
    problem.Foreground(themeBrush(L"SystemFillColorCriticalBrush"));
    problem.Visibility(Visibility::Collapsed);
    content.Children().Append(problem);
    dialog.Content(content);
    dialog.PrimaryButtonText(tr(L"ServerFolder_Add"));
    dialog.CloseButtonText(tr(L"Reset_Cancel"));
    dialog.DefaultButton(ContentDialogButton::Primary);

    const FolderForm form{server_, path, folders, problem, std::make_shared<std::vector<ServerFolder>>()};
    folders.ItemClick([form](auto&&, ItemClickEventArgs const& e) {
        const auto name = unbox_value_or<hstring>(e.ClickedItem(), L"");
        for (const auto& folder : *form.listed)
            if (toWide(folder.name) == std::wstring(name)) showFolders(form, toWide(folder.pathUtf8));
    });
    up.Click([form](auto&&, auto&&) { showFolders(form, folderAbove(std::wstring(form.path.Text()))); });
    path.KeyDown([form](auto&&, Input::KeyRoutedEventArgs const& e) {
        if (e.Key() != Windows::System::VirtualKey::Enter) return;
        e.Handled(true);
        showFolders(form, trimmed(form.path.Text()));
    });
    showFolders(form, {});
    if (co_await ui::showDialog(dialog) != ContentDialogResult::Primary) co_return;
    const auto chosen = trimmed(path.Text());
    if (chosen.empty()) co_return;
    Command command;
    command.kind = CommandKind::AddLibraryRoot;
    command.path = toUtf8(chosen);
    submitCommand(std::move(command));
    refreshRoots();
}

// Turning into a Control point restarts CamelPlayer without its library, outputs and background work; the music
// playing here stops, its queues kept, and Control points connected here are cut off (docs/Plan.md §17.8).
winrt::fire_and_forget MainWindow::switchToControl(ControlledServer target) {
    auto strong = get_strong();
    winrt::apartment_context foreground;
    const auto playing = zonesPlaying();
    const auto access = access_;
    co_await winrt::resume_background();
    std::vector<std::wstring> connected;
    try {
        if (access && access->port())
            for (const auto& device : access->pairedDevices())
                if (device.value("connected", false)) connected.push_back(toWide(device.value("name", std::string())));
    } catch (const std::exception& e) {
        log::warn("paired devices: %s", e.what());
    }
    co_await foreground;
    std::wstring text(tr(L"Pair_SwitchText"));
    if (!playing.empty()) {
        std::vector<std::wstring> names;
        names.reserve(playing.size());
        for (const auto& zone : playing) names.emplace_back(zone);
        text += L"\n\n" + std::wstring(ui::format(L"Pair_SwitchPlaying", {lines(names)}));
    }
    if (!connected.empty()) text += L"\n\n" + std::wstring(ui::format(L"Pair_SwitchConnected", {lines(connected)}));
    const auto dialog = question(ContentLayout().XamlRoot(), ui::format(L"Pair_SwitchTitle", {toWide(target.name)}),
                                 hstring(text), tr(L"Pair_SwitchButton"));
    if (co_await ui::showDialog(dialog) != ContentDialogResult::Primary) co_return;
    settings_.role = Role::Control;
    settings_.controlled = target;
    applySettingsNow();
    restartInRole();
}

winrt::fire_and_forget MainWindow::playHere() {
    auto strong = get_strong();
    const auto dialog = question(ContentLayout().XamlRoot(), tr(L"PlayHere_Title"),
                                 ui::format(L"PlayHere_Text", {toWide(settings_.controlled.name)}), tr(L"PlayHere_Button"));
    if (co_await ui::showDialog(dialog) != ContentDialogResult::Primary) co_return;
    settings_.role = Role::Play;
    applySettingsNow();
    restartInRole();
}

// The next instance waits for this one to save its settings and queues and exit.
winrt::fire_and_forget MainWindow::restartInRole() {
    auto strong = get_strong();
    winrt::apartment_context foreground;
    const auto server = server_;
    co_await winrt::resume_background();
    try {
        server->waitIdle();
    } catch (const std::exception& e) {
        log::warn("restart: %s", e.what());
    }
    co_await foreground;
    std::wstring executable(32768, L'\0');
    executable.resize(GetModuleFileNameW(nullptr, executable.data(), DWORD(executable.size())));
    log::info("restarting as %s", settings_.role == Role::Control ? "a Control point" : "a player");
    ShellExecuteW(nullptr, L"open", executable.c_str(), L"--restart", nullptr, SW_SHOWNORMAL);
    exiting_ = true;
    Close();
}

} // namespace winrt::CamelPlayer::implementation
