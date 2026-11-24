// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pch.h"

#include "MainWindow.xaml.h"
#include "MainWindowParts.h"

#include <utility>

#include "DialogPlacement.h"
#include "UiText.h"
#include "Common/settings/Settings.h"
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

void MainWindow::OnGuideClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    showFirstRunGuide();
}

// Only a new installation sees the guide by itself: never finished or skipped, and no library folder yet.
winrt::fire_and_forget MainWindow::showFirstRunGuideIfDue() {
    if (settings_.firstRunDone || !server_) co_return;
    auto weak = get_weak();
    auto server = server_;
    auto queue = DispatcherQueue();
    co_await winrt::resume_background();
    bool empty = true;
    try {
        empty = server->libraryRoots().empty();
    } catch (const std::exception&) {
    }
    if (empty)
        queue.TryEnqueue([weak] {
            if (auto self = weak.get()) self->showFirstRunGuide();
        });
}

winrt::fire_and_forget MainWindow::showFirstRunGuide() {
    if (guideFolders_ || !server_) co_return; // already open
    auto strong = get_strong();
    const auto text = [](const wchar_t* key) {
        TextBlock block;
        block.Text(tr(key));
        block.TextWrapping(TextWrapping::Wrap);
        return block;
    };

    // The role first (docs/Plan.md §17.8): playing here, the default, or controlling another CamelPlayer.
    StackPanel welcome;
    welcome.Spacing(12);
    welcome.Children().Append(text(L"Guide_Welcome"));
    RadioButtons role;
    role.Header(box_value(tr(L"Guide_Role")));
    role.Items().Append(box_value(tr(L"Guide_RolePlay")));
    role.Items().Append(box_value(tr(L"Guide_RoleControl")));
    role.SelectedIndex(0);
    welcome.Children().Append(role);
    welcome.Children().Append(text(L"Guide_RoleNote"));

    // Playing here asks once whether other computers may control this one, off by default.
    StackPanel others;
    others.Spacing(12);
    others.Children().Append(text(L"Guide_Control"));
    RadioButtons allow;
    allow.Items().Append(box_value(tr(L"Guide_ControlNo")));
    allow.Items().Append(box_value(tr(L"Guide_ControlYes")));
    allow.SelectedIndex(settings_.allowControl ? 1 : 0);
    others.Children().Append(allow);

    // Appearance: applied and saved at once, so the guide itself already shows the choice.
    ContentDialog dialog;
    StackPanel appearance;
    appearance.Spacing(12);
    appearance.Children().Append(text(L"Guide_Theme"));
    RadioButtons themes;
    for (const auto* key : {L"Theme_System", L"Theme_Light", L"Theme_Dark", L"Theme_Black"})
        themes.Items().Append(box_value(tr(key)));
    themes.SelectedIndex(int32_t(settings_.theme));
    themes.SelectionChanged([this, themes, dialog](auto&&, auto&&) {
        settings_.theme = Theme(std::clamp(themes.SelectedIndex(), 0, int32_t(Theme::Black)));
        applySettingsNow();
        dialog.RequestedTheme(Content().as<FrameworkElement>().ActualTheme());
    });
    appearance.Children().Append(themes);

    StackPanel folders;
    folders.Spacing(12);
    folders.Children().Append(text(L"Guide_Folders"));
    Button add;
    add.Content(box_value(tr(L"Settings_AddFolder/Content")));
    add.Style(Application::Current().Resources().Lookup(box_value(L"AccentButtonStyle")).as<Style>());
    add.Click([this](auto&&, auto&&) { OnAddFolderClick(nullptr, nullptr); });
    folders.Children().Append(add);
    guideFolders_ = ListView();
    guideFolders_.SelectionMode(ListViewSelectionMode::None);
    guideFolders_.MaxHeight(200);
    folders.Children().Append(guideFolders_);
    refreshRoots();

    // Output: a backend and its device; WASAPI follows the Windows default device unless one is chosen.
    StackPanel output;
    output.Spacing(12);
    output.Children().Append(text(L"Guide_Output"));
    ComboBox backend;
    backend.Header(box_value(tr(L"Settings_Backend/Header")));
    backend.Width(320);
    // In Backend's order, so an index is its Backend.
    for (const auto& name :
         {hstring(L"WASAPI"), hstring(L"ASIO"), hstring(L"Kernel Streaming"), tr(L"Settings_NetworkGroup/Text"),
          hstring(L"AirPlay"), ui::utf8(camel::backendName(Backend::HQPlayer))})
        backend.Items().Append(box_value(name));
    ComboBox device;
    device.Header(box_value(tr(L"Settings_Device/Header")));
    device.Width(420);
    auto devices = std::make_shared<std::vector<std::string>>(); // ids in the order shown
    const auto fillDevices = [this, device, devices](Backend chosen) {
        const auto catalog = server_->snapshot().devices;
        const auto* list = std::array{&catalog.wasapi,  &catalog.asio,    &catalog.ks,
                                      &catalog.network, &catalog.airplay, &catalog.hqplayer}[size_t(chosen)];
        const std::string saved =
            *std::array{&settings_.wasapi.deviceId, &settings_.asio.driverClsid, &settings_.ks.deviceId,
                        &settings_.network.udn,     &settings_.airplay.deviceId, &settings_.hqplayer.address}[size_t(chosen)];
        device.Items().Clear();
        devices->clear();
        if (chosen == Backend::Wasapi) {
            device.Items().Append(box_value(tr(L"Device_Default")));
            devices->emplace_back();
        }
        for (const auto& entry : *list) {
            if (!entry.unsupported.empty()) continue;
            device.Items().Append(box_value(ui::utf8(entry.name)));
            devices->push_back(entry.id);
        }
        // A player or receiver not found now is still the output; nothing chosen keeps it.
        const bool network = camel::isStreamed(chosen);
        const auto selected = std::find(devices->begin(), devices->end(), saved);
        device.SelectedIndex(devices->empty()             ? -1
                             : selected != devices->end() ? int32_t(selected - devices->begin())
                             : network                    ? -1
                                                          : 0);
    };
    backend.SelectionChanged(
        [fillDevices, backend](auto&&, auto&&) { fillDevices(Backend(std::max(0, backend.SelectedIndex()))); });
    backend.SelectedIndex(int32_t(settings_.backend));
    output.Children().Append(backend);
    output.Children().Append(device);

    StackPanel done;
    done.Children().Append(text(L"Guide_Done"));

    const std::array<std::pair<const wchar_t*, UIElement>, 6> steps{{{L"Guide_WelcomeTitle", welcome},
                                                                     {L"Guide_ControlTitle", others},
                                                                     {L"Guide_ThemeTitle", appearance},
                                                                     {L"Guide_FoldersTitle", folders},
                                                                     {L"Guide_OutputTitle", output},
                                                                     {L"Guide_DoneTitle", done}}};
    ui::placeDialog(dialog, ContentLayout().XamlRoot());
    dialog.DefaultButton(ContentDialogButton::Primary);
    size_t step = 0;
    bool control = false; // Control another CamelPlayer was chosen: the guide ends, and pairing follows
    const auto show = [&] {
        dialog.Title(box_value(tr(steps[step].first)));
        dialog.Content(steps[step].second);
        const bool last = step + 1 == steps.size();
        dialog.PrimaryButtonText(tr(last ? L"Guide_Finish" : L"Guide_Next"));
        dialog.SecondaryButtonText(step > 0 && !last ? tr(L"Guide_Back") : hstring{});
        dialog.CloseButtonText(last ? hstring{} : tr(L"Guide_Skip"));
    };
    dialog.PrimaryButtonClick([&](auto&&, ContentDialogButtonClickEventArgs const& e) {
        if (step + 1 == steps.size()) return;
        if (step == 0 && role.SelectedIndex() == 1) {
            control = true;
            return;
        }
        e.Cancel(true);
        ++step;
        show();
    });
    dialog.SecondaryButtonClick([&](auto&&, ContentDialogButtonClickEventArgs const& e) {
        e.Cancel(true);
        if (step > 0) --step;
        show();
    });
    show();
    const auto result = co_await ui::showDialog(dialog);
    guideFolders_ = nullptr;

    // Finishing applies the chosen output; skipping keeps the defaults. Either way the guide is done.
    if (result == ContentDialogResult::Primary && !control) {
        const auto index = device.SelectedIndex();
        if (index >= 0 && size_t(index) < devices->size())
            changeOutputs([&](camel::Settings& s, const DeviceCatalog& catalog) {
                camel::server::selectOutput(s, Backend(std::max(0, backend.SelectedIndex())), (*devices)[size_t(index)], catalog);
            });
    }
    settings_.firstRunDone = true;
    Command command;
    command.kind = CommandKind::ApplySettings;
    command.settings = settings_;
    submitCommand(std::move(command));
    populateSettings();
    if (control)
        chooseServerToControl();
    else if (result == ContentDialogResult::Primary && allow.SelectedIndex() == 1 && !settings_.allowControl)
        allowControl();
}

} // namespace winrt::CamelPlayer::implementation
