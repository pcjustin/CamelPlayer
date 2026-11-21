// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pch.h"

#include "MainWindow.xaml.h"
#include "MainWindowParts.h"

#include <utility>
#include <winrt/Microsoft.UI.Interop.h>

#include "FileDetailsWindow.h"
#include "MenuSelection.h"
#include "UiText.h"
#include "Common/util/Log.h"
#include "Common/util/Measure.h"
#include "Common/util/Text.h"
#include "TrackMarks.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace camel;
using namespace camel::audio;
using ui::tr;
using namespace camel::server;

namespace winrt::CamelPlayer::implementation {

using namespace mainwindow;

void MainWindow::OnQueueToggleClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    queueOpen_ = !queueOpen_;
    updateQueuePanel();
}

void MainWindow::removeSelectedQueueEntries() {
    if (QueueList().SelectedItems().Size() == 0) return;
    Command command;
    command.kind = CommandKind::QueueRemove;
    for (const auto& item : QueueList().SelectedItems()) command.entryIds.push_back(ui::QueueCollection::idOf(item));
    submitCommand(std::move(command));
}

void MainWindow::OnQueueClearClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    Command command;
    command.kind = CommandKind::QueueClear;
    submitCommand(std::move(command));
}

winrt::fire_and_forget MainWindow::OnQueueSaveClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    auto strong = get_strong();
    if (!server_) co_return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t date[32] = {};
    swprintf_s(date, L"%04u-%02u-%02u %02u:%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute);
    const auto name = toUtf8(std::wstring(tr(L"Queue_DefaultName")) + L" " + date);
    auto server = server_;
    auto weak = get_weak();
    auto dispatcher = DispatcherQueue();
    co_await resume_background();
    std::optional<PlaylistChange> change;
    try {
        change = server->saveQueueAsPlaylist(name);
    } catch (const std::exception& e) {
        camel::log::warn("save queue as playlist: %s", e.what());
    }
    dispatcher.TryEnqueue([weak, change] {
        auto self = weak.get();
        if (!self) return;
        if (change)
            self->showPlaylistChange(*change, true);
        else
            self->showPlaylistNotice(tr(L"Queue_SaveFailed"), {}, true);
    });
}

void MainWindow::OnQueueDoubleTapped(Windows::Foundation::IInspectable const&, Input::DoubleTappedRoutedEventArgs const&) {
    if (!QueueList().SelectedItem()) return;
    Command command;
    command.kind = CommandKind::QueueSelect;
    command.entryId = ui::QueueCollection::idOf(QueueList().SelectedItem());
    camel::measure::markPlayRequested();
    submitCommand(std::move(command));
}

void MainWindow::OnQueueRemoveEntryClick(Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&) {
    Command command;
    command.kind = CommandKind::QueueRemove;
    command.entryIds.push_back(ui::QueueCollection::idOf(sender.as<FrameworkElement>().DataContext()));
    submitCommand(std::move(command));
}

void MainWindow::OnQueueKeyDown(Windows::Foundation::IInspectable const&, Input::KeyRoutedEventArgs const& e) {
    if (e.Key() != Windows::System::VirtualKey::Delete || QueueList().SelectedItems().Size() == 0) return;
    e.Handled(true);
    removeSelectedQueueEntries();
}

void MainWindow::showPlaybackFavorite(bool favorite) {
    if (playbackFavorite_ == favorite) return;
    playbackFavorite_ = favorite;
    PlaybackFavoriteIcon().Glyph(favorite ? L"\xE00B" : L"\xE006");
    if (favorite)
        PlaybackFavoriteIcon().Foreground(themeBrush(L"AccentTextFillColorPrimaryBrush"));
    else
        PlaybackFavoriteIcon().ClearValue(IconElement::ForegroundProperty());
}

void MainWindow::OnPlaybackFavoriteClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    if (!playbackFile_) return;
    showPlaybackFavorite(!playbackFavorite_);
    submitCommand(ui::favoriteCommand({{playbackFile_}}, playbackFavorite_));
}

std::vector<Windows::Foundation::IInspectable> MainWindow::queueMenuItems() {
    if (!ui::actsOnSelection(QueueList(), *queueItems_, queueMenuItem_)) return {queueMenuItem_};
    std::vector<Windows::Foundation::IInspectable> items;
    ui::forEachSelected(QueueList(), [&](uint32_t i) { items.push_back(queueItems_->GetAt(i)); });
    return items;
}

void MainWindow::OnQueueContextRequested(UIElement const& sender, Input::ContextRequestedEventArgs const& e) {
    const auto element = sender.as<FrameworkElement>();
    queueMenuItem_ = element.DataContext();
    if (!queueMenuItem_) return;
    if (!queueMenu_) {
        queueMenu_ = MenuFlyout();
        extendPlaylistMenu(queueMenu_, [this]() -> ui::SourceFetch {
            std::vector<PlaylistSource> sources;
            for (const auto& item : queueMenuItems()) {
                const auto path = item.as<CamelPlayer::QueueItem>().Path();
                sources.push_back({PlaylistSource::Kind::Path, 0, {}, toUtf8(std::wstring_view(path.c_str(), path.size()))});
            }
            return [sources] { return sources; };
        });
        queueMenu_.Items().RemoveAt(0); // the separator before the playlist entries
        ui::appendFavoriteMenu(queueMenu_, [this](bool favorite) {
            std::vector<LibraryTrackKey> tracks;
            for (const auto& item : queueMenuItems()) {
                const auto file = get_self<QueueItem>(item.as<CamelPlayer::QueueItem>())->row().file;
                if (file && std::ranges::find(tracks, LibraryTrackKey{file}) == tracks.end()) tracks.push_back({file});
            }
            if (!tracks.empty()) submitCommand(ui::favoriteCommand(tracks, favorite));
        });
        ui::appendFileDetailsMenu(queueMenu_, [this] {
            const auto item = queueMenuItem_.as<CamelPlayer::QueueItem>();
            const auto path = item.Path();
            showFileDetails(toUtf8(std::wstring_view(path.c_str(), path.size())), item.Title());
        });
    }
    Primitives::FlyoutShowOptions options;
    if (Windows::Foundation::Point point; e.TryGetPosition(element, point)) options.Position(point);
    queueMenu_.ShowAt(element, options);
    e.Handled(true);
}

void MainWindow::showFileDetails(const std::string& pathUtf8, hstring const& title) {
    if (!server_ || pathUtf8.empty()) return;
    fileDetails_.show(Microsoft::UI::GetWindowFromWindowId(AppWindow().Id()), title,
                      Content().as<FrameworkElement>().ActualTheme(),
                      [server = server_, pathUtf8] { return server->fileDetails(pathUtf8); });
}

// Alt+Enter opens the details of the track row the focus is on, or is in.
bool MainWindow::openFocusedFileDetails() {
    if (!Content().XamlRoot()) return false;
    auto element = Input::FocusManager::GetFocusedElement(Content().XamlRoot()).try_as<DependencyObject>();
    for (; element; element = Media::VisualTreeHelper::GetParent(element)) {
        const auto container = element.try_as<Primitives::SelectorItem>();
        if (!container) continue;
        const auto item = container.Content() ? container.Content() : container.DataContext();
        if (const auto queued = item.try_as<CamelPlayer::QueueItem>()) {
            const auto path = queued.Path();
            showFileDetails(toUtf8(std::wstring_view(path.c_str(), path.size())), queued.Title());
            return true;
        }
        if (const auto browse = item.try_as<CamelPlayer::BrowseItem>(); browse && browse.IsTrack()) {
            const auto& row = get_self<BrowseItem>(browse)->row();
            showFileDetails(row.value, row.title);
            return true;
        }
        return false;
    }
    return false;
}

void MainWindow::OnPlayerTrackContextRequested(UIElement const& sender, Input::ContextRequestedEventArgs const& e) {
    if (!server_ || server_->snapshot().playback.pathUtf8.empty()) return;
    if (!playerTrackMenu_) {
        playerTrackMenu_ = MenuFlyout();
        ui::appendFileDetailsMenu(playerTrackMenu_, [this] {
            if (server_) showFileDetails(server_->snapshot().playback.pathUtf8, TitleText().Text());
        });
        playerTrackMenu_.Items().RemoveAt(0);                                                       // the separator
        playerTrackMenu_.Items().GetAt(0).as<MenuFlyoutItem>().KeyboardAcceleratorTextOverride({}); // Alt+Enter is for rows
    }
    const auto element = sender.as<FrameworkElement>();
    Primitives::FlyoutShowOptions options;
    if (Windows::Foundation::Point point; e.TryGetPosition(element, point)) options.Position(point);
    playerTrackMenu_.ShowAt(element, options);
    e.Handled(true);
}

void MainWindow::OnQueueDragItemsCompleted(ListViewBase const&, DragItemsCompletedEventArgs const& e) {
    if (e.DropResult() != Windows::ApplicationModel::DataTransfer::DataPackageOperation::Move || queueItems_->Size() < 2) return;
    Command command;
    command.kind = CommandKind::QueueReorder;
    command.entryIds = queueItems_->ids();
    submitCommand(std::move(command));
}

} // namespace winrt::CamelPlayer::implementation
