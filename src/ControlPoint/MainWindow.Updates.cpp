// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pch.h"

#include "MainWindow.xaml.h"
#include "MainWindowParts.h"

#include <filesystem>
#include <fstream>

#include "DialogPlacement.h"
#include "UiText.h"
#include "Common/settings/Settings.h"
#include "Common/util/Log.h"
#include "Common/util/Text.h"
#include "Common/Version.h"

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

std::filesystem::path programDirectory() {
    std::wstring executable(32768, L'\0');
    executable.resize(GetModuleFileNameW(nullptr, executable.data(), DWORD(executable.size())));
    return std::filesystem::path(executable).parent_path();
}

} // namespace

void MainWindow::OnCheckUpdatesClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    checkForUpdates(true);
}

// The startup and daily checks skip a skipped version and stay silent on failure; the About button does neither.
winrt::fire_and_forget MainWindow::checkForUpdates(bool manual) {
    auto weak = get_weak();
    auto queue = DispatcherQueue();
    if (manual) {
        CheckUpdatesButton().IsEnabled(false);
        UpdateStatusText().Text(tr(L"Update_Checking"));
    }
    co_await resume_background();
    std::optional<update::Release> release;
    std::string failure;
    try {
        update::cleanTemp(camel::dataDirectory());
        release = update::check(CAMEL_VERSION_STRING);
    } catch (const std::exception& e) {
        failure = e.what();
        camel::log::warn("update check: %s", e.what());
    }
    queue.TryEnqueue([weak, manual, release, failure] {
        auto self = weak.get();
        if (!self) return;
        if (manual) {
            self->CheckUpdatesButton().IsEnabled(true);
            self->UpdateStatusText().Text(!failure.empty() ? ui::format(L"Update_CheckFailed", {std::wstring(ui::utf8(failure))})
                                          : release        ? ui::format(L"Update_Found", {toWide(release->version)})
                                                           : tr(L"Update_UpToDate"));
        }
        if (!release || (!manual && release->version == self->settings_.skippedVersion)) return;
        if (self->release_ && self->release_->version == release->version) { // keeps a running or finished download
            self->UpdateBar().IsOpen(true);
            return;
        }
        camel::log::info("update: CamelPlayer %s is available", release->version.c_str());
        self->release_ = release;
        self->updateFiles_ = {};
        self->showUpdateOffer();
    });
}

void MainWindow::showUpdateOffer() {
    if (!release_ || (RepairBar().IsOpen() && RepairBar().Severity() != InfoBarSeverity::Success)) return;
    const auto& release = *release_;
    const std::wstring version = toWide(release.version);
    auto bar = UpdateBar();
    bar.Severity(InfoBarSeverity::Informational);
    bar.Title(ui::format(updateFiles_.files.empty() ? L"Update_AvailableTitle" : L"Update_ReadyTitle", {version}));
    bar.Message(ui::format(L"Update_AvailableText", {L"" CAMEL_VERSION_STRING}));
    UpdateInstallButton().Content(box_value(tr(updateFiles_.files.empty() ? L"Update_Install" : L"Update_Restart")));
    UpdateInstallButton().Visibility(Visibility::Visible);
    UpdateNotesButton().Visibility(release.notes.empty() ? Visibility::Collapsed : Visibility::Visible);
    UpdateSkipButton().Visibility(Visibility::Visible);
    UpdateProgress().Visibility(Visibility::Collapsed);
    UpdateCancelButton().Visibility(Visibility::Collapsed);
    bar.IsClosable(true);
    bar.IsOpen(true);
}

winrt::fire_and_forget MainWindow::OnUpdateInstallClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    auto strong = get_strong();
    if (!release_) co_return;
    if (!updateFiles_.files.empty()) {
        restartToUpdate(*release_, updateFiles_);
        co_return;
    }
    const auto release = *release_;
    const std::wstring version = toWide(release.version);
    updateToken_ = {};
    const auto token = updateToken_;
    UpdateBar().Title(ui::format(L"Update_Downloading", {version, L""}));
    UpdateBar().IsClosable(false);
    UpdateInstallButton().Visibility(Visibility::Collapsed);
    UpdateNotesButton().Visibility(Visibility::Collapsed);
    UpdateSkipButton().Visibility(Visibility::Collapsed);
    UpdateProgress().IsIndeterminate(true);
    UpdateProgress().Visibility(Visibility::Visible);
    UpdateCancelButton().Visibility(Visibility::Visible);
    auto weak = get_weak();
    auto queue = DispatcherQueue();
    co_await resume_background();
    update::Prepared files;
    std::string failure;
    auto shownPercent = std::make_shared<int>(-1);
    try {
        files = update::prepare(
            release,
            [weak, queue, version, shownPercent](uint64_t done, uint64_t total) {
                if (total == 0) return;
                const int percent = int(done * 100 / total);
                if (percent == *shownPercent) return;
                *shownPercent = percent;
                queue.TryEnqueue([weak, version, percent] {
                    auto self = weak.get();
                    if (!self) return;
                    self->UpdateProgress().IsIndeterminate(false);
                    self->UpdateProgress().Value(percent);
                    self->UpdateBar().Title(ui::format(L"Update_Downloading", {version, std::to_wstring(percent) + L"%"}));
                });
            },
            token);
    } catch (const std::exception& e) {
        failure = e.what();
    }
    const bool cancelled = token.cancelled();
    queue.TryEnqueue([weak, release, files, failure, cancelled] {
        auto self = weak.get();
        if (!self) return;
        if (!failure.empty()) {
            camel::log::warn("update download: %s", failure.c_str());
            self->showUpdateOffer();
            if (cancelled) return;
            self->UpdateBar().Severity(InfoBarSeverity::Error);
            self->UpdateBar().Title(tr(L"Update_DownloadFailedTitle"));
            self->UpdateBar().Message(ui::utf8(failure));
            self->UpdateInstallButton().Content(box_value(tr(L"Update_Retry")));
            return;
        }
        self->updateFiles_ = files;
        self->showUpdateOffer();
        self->restartToUpdate(release, files);
    });
}

void MainWindow::OnUpdateCancelClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    updateToken_.cancel();
}

winrt::fire_and_forget MainWindow::OnUpdateNotesClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    auto strong = get_strong();
    if (!release_) co_return;
    const auto release = *release_;
    co_await ui::showDialog(textDialog(ContentLayout().XamlRoot(), ui::format(L"Update_NotesTitle", {toWide(release.version)}),
                                       ui::utf8(release.notes)));
}

void MainWindow::OnUpdateSkipClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    if (!release_) return;
    const auto& release = *release_;
    settings_.skippedVersion = release.version;
    applySettingsNow();
    UpdateBar().IsOpen(false);
}

// Hands the verified files to CamelUpdater.exe and exits the normal way, so the queue and position are saved. The
// running version's own files repair the installation.
winrt::fire_and_forget MainWindow::restartToUpdate(update::Release release, update::Prepared prepared) {
    auto strong = get_strong();
    const bool repair = release.version == CAMEL_VERSION_STRING;
    auto bar = repair ? RepairBar() : UpdateBar();
    if (server_ && server_->snapshot().playback.state == PlaybackState::Playing && !inTray_) {
        ContentDialog dialog;
        ui::placeDialog(dialog, ContentLayout().XamlRoot());
        dialog.Title(box_value(tr(repair ? L"Repair_ConfirmTitle" : L"Update_ConfirmTitle")));
        dialog.Content(
            box_value(repair ? tr(L"Repair_ConfirmText") : ui::format(L"Update_ConfirmText", {toWide(release.version)})));
        dialog.PrimaryButtonText(tr(repair ? L"Repair_ConfirmPrimary" : L"Update_ConfirmPrimary"));
        dialog.CloseButtonText(tr(L"Update_ConfirmCancel"));
        dialog.DefaultButton(ContentDialogButton::Primary);
        if (co_await ui::showDialog(dialog) != ContentDialogResult::Primary) {
            if (repair) showRepairOffer();
            co_return;
        }
    }
    try {
        update::startUpdater(programDirectory(), prepared, camel::dataDirectory(), release.version, CAMEL_VERSION_STRING);
    } catch (const std::exception& e) {
        camel::log::warn("update: %s", e.what());
        if (repair) showRepairOffer();
        bar.Severity(InfoBarSeverity::Error);
        bar.Title(ui::format(L"Update_StartFailed", {std::wstring(ui::utf8(e.what()))}));
        co_return;
    }
    camel::log::info("update: CamelUpdater takes over to %s %s", repair ? "repair" : "install", release.version.c_str());
    exiting_ = true;
    Close();
}

void MainWindow::showDataRecovery() {
    if (!server_) return;
    const auto recovery = server_->dataRecovery();
    std::wstring text;
    // Sentences after a full-width full stop need no space.
    const auto add = [&text](hstring const& sentence) {
        if (!text.empty() && text.back() != L'。') text += L' ';
        text += sentence;
    };
    if (recovery.settings == camel::SettingsRecovery::RestoredLastGood) add(tr(L"Recovery_SettingsRestored"));
    if (recovery.settings == camel::SettingsRecovery::Reset) add(tr(L"Recovery_SettingsReset"));
    if (recovery.library == DataRecovery::Library::RestoredBackup) add(tr(L"Recovery_LibraryRestored"));
    if (recovery.library == DataRecovery::Library::Rebuilt) add(tr(L"Recovery_LibraryRebuilt"));
    if (text.empty()) return;
    add(ui::format(L"Recovery_Kept", {camel::dataDirectory().wstring()}));
    RecoveryBar().Title(tr(L"Recovery_Title"));
    RecoveryBar().Message(text);
    RecoveryBar().IsOpen(true);
}

void MainWindow::OnCheckInstallClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    checkInstallation(true);
}

// A version whose files have not passed a full check yet gets one, later starts compare sizes; the About button always
// checks fully. A development build has no install-manifest.txt and nothing to check.
winrt::fire_and_forget MainWindow::checkInstallation(bool manual) {
    auto weak = get_weak();
    auto queue = DispatcherQueue();
    if (manual) {
        CheckInstallButton().IsEnabled(false);
        InstallStatusText().Text(tr(L"Install_Checking"));
    }
    co_await resume_background();
    const auto data = camel::dataDirectory();
    const bool full = manual || !update::verified(data, CAMEL_VERSION_STRING);
    std::optional<update::InstallCheck> check;
    std::string failure;
    try {
        check = update::checkInstallation(programDirectory(), full);
        if (check && check->intact() && full) update::markVerified(data, CAMEL_VERSION_STRING);
    } catch (const std::exception& e) {
        failure = e.what();
        camel::log::warn("installation check: %s", e.what());
    }
    if (check && !check->intact())
        camel::log::warn("installation check: %zu of %zu files missing, %zu damaged", check->missing.size(), check->files,
                         check->damaged.size());
    queue.TryEnqueue([weak, manual, check, failure] {
        auto self = weak.get();
        if (!self) return;
        if (manual) {
            self->CheckInstallButton().IsEnabled(true);
            self->InstallStatusText().Text(
                !failure.empty()  ? ui::format(L"Install_CheckFailed", {std::wstring(ui::utf8(failure))})
                : !check          ? tr(L"Install_NoManifest")
                : check->intact() ? ui::format(L"Install_Intact", {std::to_wstring(check->files)})
                                  : ui::counted(L"Install_Problems", int64_t(check->missing.size() + check->damaged.size())));
        }
        if (!check || check->intact()) return;
        self->installCheck_ = check;
        // A repair result still shown keeps its message; its button tries again.
        if (!manual && self->RepairBar().IsOpen()) return;
        self->showRepairOffer();
    });
}

void MainWindow::showRepairOffer() {
    if (!installCheck_) return;
    const auto& check = installCheck_.value();
    auto bar = RepairBar();
    bar.Severity(InfoBarSeverity::Warning);
    bar.Title(tr(L"Repair_Title"));
    const auto files = check.missing.size() + check.damaged.size();
    bar.Message(ui::format(files == 1 ? L"Repair_Text_One" : L"Repair_Text", {std::to_wstring(files), L"" CAMEL_VERSION_STRING}));
    RepairButton().Content(box_value(tr(L"Repair_Install")));
    RepairButton().Visibility(Visibility::Visible);
    RepairDetailsButton().Visibility(Visibility::Visible);
    RepairSkipButton().Visibility(Visibility::Visible);
    RepairProgress().Visibility(Visibility::Collapsed);
    RepairCancelButton().Visibility(Visibility::Collapsed);
    bar.IsClosable(false);
    bar.IsOpen(true);
    UpdateBar().IsOpen(false);
}

// What the updater reported after a repair; Repair stays for one that did not complete.
void MainWindow::showRepairResult(wchar_t const* title, wchar_t const* text, InfoBarSeverity severity) {
    auto bar = RepairBar();
    bar.Severity(severity);
    bar.Title(tr(title));
    bar.Message(tr(text));
    const bool done = severity == InfoBarSeverity::Success;
    RepairButton().Content(box_value(tr(L"Update_Retry")));
    RepairButton().Visibility(done ? Visibility::Collapsed : Visibility::Visible);
    RepairDetailsButton().Visibility(Visibility::Collapsed);
    RepairSkipButton().Visibility(done ? Visibility::Collapsed : Visibility::Visible);
    RepairProgress().Visibility(Visibility::Collapsed);
    RepairCancelButton().Visibility(Visibility::Collapsed);
    bar.IsClosable(done);
    bar.IsOpen(true);
}

// Downloads the running version again and installs it over the program folder like an update, which replaces only the
// program's own files and leaves anything else in the folder where it is. When GitHub no longer has it, a newer release
// is offered instead, which replaces the files as well.
winrt::fire_and_forget MainWindow::OnRepairClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    auto strong = get_strong();
    repairToken_ = {};
    const auto token = repairToken_;
    RepairBar().Severity(InfoBarSeverity::Warning);
    RepairBar().Title(tr(L"Repair_Finding"));
    RepairBar().Message(L"");
    RepairButton().Visibility(Visibility::Collapsed);
    RepairDetailsButton().Visibility(Visibility::Collapsed);
    RepairSkipButton().Visibility(Visibility::Collapsed);
    RepairProgress().IsIndeterminate(true);
    RepairProgress().Visibility(Visibility::Visible);
    RepairCancelButton().Visibility(Visibility::Visible);
    auto weak = get_weak();
    auto queue = DispatcherQueue();
    co_await resume_background();
    std::optional<update::Release> release, newer;
    update::Prepared files;
    std::string failure;
    auto shownPercent = std::make_shared<int>(-1);
    try {
        release = update::releaseOf(CAMEL_VERSION_STRING);
        if (!release) {
            newer = update::check(CAMEL_VERSION_STRING);
        } else {
            files = update::prepare(
                *release,
                [weak, queue, shownPercent](uint64_t done, uint64_t total) {
                    if (total == 0) return;
                    const int percent = int(done * 100 / total);
                    if (percent == *shownPercent) return;
                    *shownPercent = percent;
                    queue.TryEnqueue([weak, percent] {
                        auto self = weak.get();
                        if (!self) return;
                        self->RepairProgress().IsIndeterminate(false);
                        self->RepairProgress().Value(percent);
                        self->RepairBar().Title(ui::format(L"Repair_Downloading", {std::to_wstring(percent) + L"%"}));
                    });
                },
                token);
        }
    } catch (const std::exception& e) {
        failure = e.what();
    }
    const bool cancelled = token.cancelled();
    queue.TryEnqueue([weak, release, newer, files, failure, cancelled] {
        auto self = weak.get();
        if (!self) return;
        if (failure.empty() && release) {
            self->restartToUpdate(*release, files);
            return;
        }
        if (failure.empty() && newer) {
            camel::log::info("repair: %s is no longer on GitHub; offering %s", CAMEL_VERSION_STRING, newer->version.c_str());
            self->RepairBar().IsOpen(false);
            self->release_ = newer;
            self->updateFiles_ = {};
            self->showUpdateOffer();
            self->UpdateBar().Message(tr(L"Repair_UseUpdate"));
            return;
        }
        if (!failure.empty()) camel::log::warn("repair: %s", failure.c_str());
        self->showRepairOffer();
        if (cancelled) return;
        self->RepairBar().Severity(InfoBarSeverity::Error);
        self->RepairBar().Title(tr(failure.empty() ? L"Repair_GoneTitle" : L"Repair_DownloadFailedTitle"));
        self->RepairBar().Message(failure.empty() ? tr(L"Repair_GoneText") : hstring(ui::utf8(failure)));
        self->RepairButton().Content(box_value(tr(L"Update_Retry")));
    });
}

winrt::fire_and_forget MainWindow::OnRepairDetailsClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    auto strong = get_strong();
    if (!installCheck_) co_return;
    std::wstring text;
    for (const auto& path : installCheck_->missing)
        text += std::wstring(tr(L"Repair_Missing")) + L"  " + path.generic_wstring() + L"\n";
    for (const auto& path : installCheck_->damaged)
        text += std::wstring(tr(L"Repair_Damaged")) + L"  " + path.generic_wstring() + L"\n";
    co_await ui::showDialog(textDialog(ContentLayout().XamlRoot(), tr(L"Repair_DetailsTitle"), hstring(text)));
}

void MainWindow::OnRepairSkipClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    RepairBar().IsOpen(false);
    showUpdateOffer();
}

void MainWindow::OnRepairCancelClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    repairToken_.cancel();
}

winrt::fire_and_forget MainWindow::showUpdateResult() {
    auto strong = get_strong();
    const auto data = camel::dataDirectory();
    const auto result = update::readResult(data);
    if (!result) co_return;
    const std::wstring version = toWide(result->version);
    const bool repair = result->version == CAMEL_VERSION_STRING;
    if (result->kind == update::Result::Kind::Installed) {
        if (!repair || !update::signalStarted(data)) {
            update::removeResult(data);
            co_return;
        }
        if (result->previous == result->version) {
            showRepairResult(L"Repair_DoneTitle", L"Repair_DoneText", InfoBarSeverity::Success);
            co_return;
        }
        // Closed before the dialog, which would otherwise hold the installed file open while it is shown.
        const std::string changelog = [] {
            std::ifstream in(programDirectory() / L"CHANGELOG.md", std::ios::binary);
            return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        }();
        co_await ui::showDialog(textDialog(ContentLayout().XamlRoot(), ui::format(L"Update_InstalledTitle", {version}),
                                           ui::utf8(update::changelogSince(changelog, result->previous, result->version))));
        co_return;
    }
    update::removeResult(data);
    const bool undone = result->kind == update::Result::Kind::Undone;
    const bool failed = undone || result->kind == update::Result::Kind::Failed;
    if (repair) {
        showRepairResult(failed ? L"Repair_FailedTitle" : L"Repair_CancelledTitle",
                         failed ? L"Repair_FailedText" : L"Update_CancelledText", InfoBarSeverity::Warning);
        co_return;
    }
    if (failed && !undone) { // only a version that did not start is skipped
        settings_.skippedVersion = result->version;
        applySettingsNow();
    }
    auto bar = UpdateBar();
    bar.Severity(InfoBarSeverity::Warning);
    bar.Title(ui::format(failed ? L"Update_FailedTitle" : L"Update_CancelledTitle", {version}));
    bar.Message(failed ? ui::format(undone ? L"Update_UndoneText" : L"Update_FailedText", {version})
                       : tr(L"Update_CancelledText"));
    UpdateInstallButton().Visibility(Visibility::Collapsed);
    UpdateNotesButton().Visibility(Visibility::Collapsed);
    UpdateSkipButton().Visibility(Visibility::Collapsed);
    bar.IsOpen(true);
}

} // namespace winrt::CamelPlayer::implementation
