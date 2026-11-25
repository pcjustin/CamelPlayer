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

void MainWindow::moveQueueEntries(int step) {
    if (!ui::moveSelected(QueueList(), *queueItems_, step)) return;
    Command command;
    command.kind = CommandKind::QueueReorder;
    command.entryIds = queueItems_->ids();
    submitCommand(std::move(command));
}

// Clearing can be undone, as a playlist's edits can: the entries come back, and the one playing plays on where it was.
void MainWindow::OnQueueClearClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    if (!server_) return;
    const auto snapshot = server_->snapshot();
    std::vector<TrackSource> sources;
    uint64_t index = 0;
    for (const auto& entry : snapshot.queue.entries) {
        if (entry.id == snapshot.queueEntryId) index = sources.size();
        sources.push_back(entry.source);
    }
    const auto& playback = snapshot.playback;
    const bool playing = playback.state == PlaybackState::Playing;
    const uint64_t frame = playing ? playback.position : 0;
    Command command;
    command.kind = CommandKind::QueueClear;
    submitCommand(std::move(command));
    if (!sources.empty()) showQueueCleared(snapshot.selectedZone, sources, index, playing, frame);
}

void MainWindow::showQueueCleared(const std::string& zone, const std::vector<TrackSource>& sources, uint64_t index, bool playing,
                                  uint64_t frame) {
    auto bar = PlaylistNotice();
    bar.IsOpen(false);
    bar.Title(tr(L"Queue_Cleared"));
    bar.Message({});
    bar.Severity(InfoBarSeverity::Informational);
    Button undo;
    undo.Content(box_value(tr(L"Playlist_Undo")));
    undo.Click([this, zone, sources, index, playing, frame](auto&&, auto&&) {
        PlaylistNotice().IsOpen(false);
        // A queue that was not playing comes back without playing.
        Command restore;
        restore.kind = playing ? CommandKind::QueueReplace : CommandKind::QueueAdd;
        restore.zone = zone;
        restore.sources = sources;
        restore.index = index;
        submitCommand(std::move(restore));
        if (!playing || !frame) return;
        Command seek;
        seek.kind = CommandKind::Seek;
        seek.zone = zone;
        seek.frame = frame;
        submitCommand(std::move(seek));
    });
    StackPanel actions;
    actions.Margin({0, 0, 0, 12});
    actions.Children().Append(undo);
    bar.Content(actions);
    bar.IsOpen(true);
    noticeUndo_.reset();
    noticeTimer_.Stop();
    noticeTimer_.Start();
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

// Taken before the list, which moves the focus on Up and Down.
void MainWindow::OnQueuePreviewKeyDown(Windows::Foundation::IInspectable const&, Input::KeyRoutedEventArgs const& e) {
    const int step = ui::moveStep(e);
    if (!step) return;
    e.Handled(true);
    moveQueueEntries(step);
}

void MainWindow::showPlaybackFavorite(bool favorite) {
    if (playbackFavorite_ == favorite) return;
    playbackFavorite_ = favorite;
    PlaybackFavoriteIcon().Glyph(favorite ? L"\xE00B" : L"\xE006");
    const auto name = tr(favorite ? L"Marks_FavoriteOn" : L"Marks_FavoriteOff");
    Automation::AutomationProperties::SetName(PlaybackFavoriteButton(), name);
    ToolTipService::SetToolTip(PlaybackFavoriteButton(), box_value(name));
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
        ui::insertMoveMenu(queueMenu_, 0, [this](int step) {
            if (!ui::actsOnSelection(QueueList(), *queueItems_, queueMenuItem_)) QueueList().SelectedItem(queueMenuItem_);
            moveQueueEntries(step);
        });
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
