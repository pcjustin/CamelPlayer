// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pch.h"

#include "MainWindow.xaml.h"
#include "MainWindowParts.h"

#include <thread>
#include <utility>

#include "DialogPlacement.h"
#include "UiText.h"
#include "Common/util/Log.h"
#include "Common/util/Text.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace camel;
using namespace camel::audio;
using ui::tr;
using namespace camel::server;

namespace winrt::CamelPlayer::implementation {

using namespace mainwindow;

void MainWindow::extendPlaylistMenu(MenuFlyout const& menu, const std::function<ui::SourceFetch()>& selection) {
    menu.Items().Append(MenuFlyoutSeparator());
    MenuFlyoutItem create;
    create.Text(tr(L"Library_MenuCreatePlaylist"));
    FontIcon createIcon;
    createIcon.Glyph(L"\xE8F4");
    create.Icon(createIcon);
    create.Click([this, selection](auto&&, auto&&) { runPlaylistAction(selection(), std::nullopt, {}); });
    menu.Items().Append(create);
    MenuFlyoutSubItem add;
    add.Text(tr(L"Library_MenuAddToPlaylist"));
    FontIcon addIcon;
    addIcon.Glyph(L"\xE90B");
    add.Icon(addIcon);
    menu.Items().Append(add);
    menu.Opening([this, weakAdd = make_weak(add), selection](auto&&, auto&&) {
        const auto add = weakAdd.get();
        if (!add) return;
        add.Items().Clear();
        for (const auto& playlist : playlists_) {
            MenuFlyoutItem entry;
            entry.Text(ui::utf8(playlist.name));
            entry.Click([this, selection, id = playlist.id](auto&&, auto&&) { runPlaylistAction(selection(), id, {}); });
            add.Items().Append(entry);
        }
        if (!playlists_.empty()) add.Items().Append(MenuFlyoutSeparator());
        MenuFlyoutItem create;
        create.Text(tr(L"Library_MenuNewPlaylist"));
        create.Click([this, selection](auto&&, auto&&) { askNewPlaylist(selection()); });
        add.Items().Append(create);
    });
}

void MainWindow::runPlaylistAction(ui::SourceFetch fetch, std::optional<int64_t> playlist, std::string name) {
    if (!server_ || !fetch) return;
    auto weak = get_weak();
    auto dispatcher = DispatcherQueue();
    std::thread([weak, dispatcher, server = server_, fetch = std::move(fetch), playlist, name = std::move(name)] {
        std::vector<PlaylistSource> sources;
        std::optional<SelectionPreview> preview;
        bool changed = false;
        try {
            sources = fetch();
            preview = sources.empty() ? SelectionPreview{} : server->previewSelection(sources);
        } catch (const camel::server::LibraryRevisionChanged&) {
            changed = true;
        } catch (const std::exception& e) {
            camel::log::warn("playlist selection: %s", e.what());
        }
        dispatcher.TryEnqueue([weak, sources = std::move(sources), preview, playlist, name, changed] {
            auto self = weak.get();
            if (!self) return;
            if (changed)
                self->showSelectionChanged();
            else if (!preview)
                self->showPlaylistNotice(tr(L"Playlist_Failed"), {}, true);
            else if (preview->tracks == 0)
                self->showPlaylistNotice(tr(L"Playlist_NothingAdded"), tr(L"Playlist_NothingAddedText"), false);
            else
                self->applyPlaylistAction(sources, *preview, playlist, name);
        });
    }).detach();
}

winrt::fire_and_forget MainWindow::applyPlaylistAction(std::vector<PlaylistSource> sources, SelectionPreview preview,
                                                       std::optional<int64_t> playlist, std::string name) {
    auto strong = get_strong();
    if (preview.tracks > 1000) {
        ContentDialog dialog;
        ui::placeDialog(dialog, ContentLayout().XamlRoot());
        dialog.Title(box_value(tr(L"Playlist_ConfirmTitle")));
        const std::wstring count = ui::count(preview.tracks);
        const int64_t minutes = preview.durationMs / 60'000;
        const std::wstring length =
            minutes < int64_t{24} * 60
                ? ui::formatTime(double(preview.durationMs) / 1000.0)
                : std::wstring(ui::format(L"Home_DaysHours",
                                          {std::to_wstring(minutes / (int64_t{24} * 60)), std::to_wstring(minutes / 60 % 24)}));
        dialog.Content(box_value(ui::format(L"Playlist_ConfirmText", {count, length})));
        dialog.PrimaryButtonText(tr(playlist ? L"Playlist_ConfirmAdd" : L"Playlist_ConfirmCreate"));
        dialog.CloseButtonText(tr(L"Playlist_Cancel"));
        dialog.DefaultButton(ContentDialogButton::Primary);
        if (co_await ui::showDialog(dialog) != ContentDialogResult::Primary) co_return;
    }
    auto server = server_;
    auto weak = get_weak();
    auto dispatcher = DispatcherQueue();
    co_await resume_background();
    std::optional<PlaylistChange> change;
    try {
        change = playlist ? server->addToPlaylist(*playlist, std::nullopt, sources) : server->createPlaylist(name, sources);
    } catch (const std::exception& e) {
        camel::log::warn("playlist change: %s", e.what());
    }
    dispatcher.TryEnqueue([weak, change, created = !playlist] {
        auto self = weak.get();
        if (!self) return;
        if (change)
            self->showPlaylistChange(*change, created);
        else
            self->showPlaylistNotice(tr(L"Playlist_Failed"), {}, true);
    });
}

winrt::fire_and_forget MainWindow::askNewPlaylist(ui::SourceFetch fetch) {
    auto strong = get_strong();
    ContentDialog dialog;
    ui::placeDialog(dialog, ContentLayout().XamlRoot());
    dialog.Title(box_value(tr(L"Playlist_NewTitle")));
    TextBox name;
    name.Header(box_value(tr(L"Playlist_NameLabel")));
    name.PlaceholderText(tr(L"Playlist_NamePlaceholder"));
    dialog.Content(name);
    dialog.PrimaryButtonText(tr(L"Playlist_ConfirmCreate"));
    dialog.CloseButtonText(tr(L"Playlist_Cancel"));
    dialog.DefaultButton(ContentDialogButton::Primary);
    if (co_await ui::showDialog(dialog) != ContentDialogResult::Primary) co_return;
    const auto text = name.Text();
    runPlaylistAction(std::move(fetch), std::nullopt, toUtf8(std::wstring_view(text.c_str(), text.size())));
}

void MainWindow::showPlaylistChange(const PlaylistChange& change, bool created) {
    std::wstring message(ui::counted(L"Library_Tracks", change.added));
    if (const auto skipped = change.skippedUnavailable + change.skippedDuplicates + change.skippedExisting)
        message += L" · " + std::wstring(ui::format(L"Playlist_Skipped", {ui::count(skipped)}));
    const std::wstring name(ui::utf8(change.name));
    if (!created && change.added == 0)
        showPlaylistNotice(ui::format(L"Playlist_NothingAddedTo", {name}), hstring(message), false, std::nullopt,
                           change.playlist);
    else
        showPlaylistNotice(ui::format(created ? L"Playlist_Created" : L"Playlist_Added", {name}), hstring(message), false,
                           change.undo, change.playlist);
    get_self<PlaylistView>(Playlists())->reload(change.playlist);
    if (created)
        refreshPlaylists([this, id = change.playlist] { renameInNavigation(id); });
    else
        refreshPlaylists();
}

void MainWindow::showSelectionChanged() {
    showPlaylistNotice(tr(L"Library_SelectionChangedTitle"), tr(L"Library_SelectionChangedText"), false);
    PlaylistNotice().Severity(InfoBarSeverity::Warning);
}

void MainWindow::showPlaylistNotice(hstring const& title, hstring const& message, bool error, std::optional<PlaylistUndo> undo,
                                    int64_t playlist) {
    auto bar = PlaylistNotice();
    bar.IsOpen(false);
    bar.Title(title);
    bar.Message(message);
    bar.Severity(error ? InfoBarSeverity::Error : InfoBarSeverity::Success);
    StackPanel actions;
    actions.Orientation(Orientation::Horizontal);
    actions.Spacing(8);
    actions.Margin({0, 0, 0, 12});
    if (playlist) {
        HyperlinkButton open;
        open.Content(box_value(tr(L"Playlist_Open")));
        open.Click([this, playlist](auto&&, auto&&) {
            PlaylistNotice().IsOpen(false);
            selectPage(L"playlist:" + to_hstring(playlist));
        });
        actions.Children().Append(open);
    }
    if (undo && (undo->created || !undo->items.empty())) {
        Button button;
        button.Content(box_value(tr(L"Playlist_Undo")));
        button.Click([this](auto&&, auto&&) { undoPlaylistNotice(); });
        actions.Children().Append(button);
    }
    if (actions.Children().Size())
        bar.Content(actions);
    else
        bar.Content(nullptr);
    bar.IsOpen(true);
    noticeUndo_ = undo; // after IsOpen: reopening a closed notice raises Closed for the previous one
    noticeTimer_.Stop();
    noticeTimer_.Start();
}

void MainWindow::undoPlaylistNotice() {
    if (!noticeUndo_ || !server_) return;
    const auto undo = *noticeUndo_;
    noticeUndo_.reset();
    PlaylistNotice().IsOpen(false);
    auto weak = get_weak();
    auto dispatcher = DispatcherQueue();
    std::thread([weak, dispatcher, server = server_, undo] {
        bool undone = false;
        try {
            undone = server->undoPlaylistChange(undo);
        } catch (const std::exception& e) {
            camel::log::warn("playlist undo: %s", e.what());
        }
        dispatcher.TryEnqueue([weak, undo, undone] {
            auto self = weak.get();
            if (!self) return;
            self->showPlaylistNotice(tr(undone ? L"Playlist_Undone" : L"Playlist_UndoFailed"), {}, !undone);
            self->refreshPlaylists();
            auto view = get_self<PlaylistView>(self->Playlists());
            if (undone && undo.created && view->shown() == undo.playlist)
                self->selectPage(L"playlists");
            else
                view->reload(undo.playlist);
        });
    }).detach();
}

winrt::fire_and_forget MainWindow::refreshPlaylists(std::function<void()> then) {
    auto strong = get_strong();
    if (!server_) co_return;
    auto server = server_;
    auto weak = get_weak();
    auto dispatcher = DispatcherQueue();
    co_await resume_background();
    std::vector<PlaylistSummary> list;
    try {
        list = server->playlists();
    } catch (const std::exception& e) {
        camel::log::warn("playlists: %s", e.what());
        co_return;
    }
    dispatcher.TryEnqueue([weak, list = std::move(list), then] {
        auto self = weak.get();
        if (!self) return;
        self->playlists_ = list;
        self->showPlaylistsInNavigation();
        if (then) then();
    });
}

void MainWindow::showPlaylistsInNavigation() {
    const auto children = PlaylistsNav().MenuItems();
    const auto tagOf = [](const PlaylistSummary& p) { return L"playlist:" + to_hstring(p.id); };
    bool same = children.Size() == playlists_.size();
    for (uint32_t i = 0; same && i < children.Size(); ++i)
        same = unbox_value_or<hstring>(children.GetAt(i).as<NavigationViewItem>().Tag(), L"") == tagOf(playlists_[i]);
    if (same) {
        for (uint32_t i = 0; i < children.Size(); ++i)
            children.GetAt(i).as<NavigationViewItem>().Content(box_value(ui::utf8(playlists_[i].name)));
        return;
    }
    navUpdating_ = true;
    children.Clear();
    for (const auto& playlist : playlists_) {
        NavigationViewItem item;
        item.Content(box_value(ui::utf8(playlist.name)));
        item.Tag(box_value(tagOf(playlist)));
        FontIcon icon;
        icon.Glyph(L"\xE90B");
        item.Icon(icon);
        children.Append(item);
        if (shownTag_ == tagOf(playlist)) Nav().SelectedItem(item);
    }
    navUpdating_ = false;
}

void MainWindow::renameInNavigation(int64_t playlist) {
    if (!Nav().IsPaneOpen() || Nav().DisplayMode() != NavigationViewDisplayMode::Expanded) return;
    NavigationViewItem item{nullptr};
    for (const auto& child : PlaylistsNav().MenuItems())
        if (unbox_value_or<hstring>(child.as<NavigationViewItem>().Tag(), L"") == L"playlist:" + to_hstring(playlist))
            item = child.as<NavigationViewItem>();
    if (!item) return;
    PlaylistsNav().IsExpanded(true);
    // The box sits in a flyout beside the item: inside the item, Enter would also open the playlist.
    const auto name = unbox_value_or<hstring>(item.Content(), L"");
    TextBox box;
    box.Header(box_value(tr(L"Playlist_NameLabel")));
    box.Text(name);
    box.MinWidth(240);
    Flyout flyout;
    flyout.Content(box);
    flyout.Placement(Primitives::FlyoutPlacementMode::RightEdgeAlignedTop);
    auto cancelled = std::make_shared<bool>(false);
    box.KeyDown([cancelled, weakFlyout = make_weak(flyout)](auto&&, Input::KeyRoutedEventArgs const& e) {
        if (e.Key() != Windows::System::VirtualKey::Enter && e.Key() != Windows::System::VirtualKey::Escape) return;
        e.Handled(true);
        *cancelled = e.Key() == Windows::System::VirtualKey::Escape;
        if (const auto flyout = weakFlyout.get()) flyout.Hide();
    });
    flyout.Opened([weakBox = make_weak(box)](auto&&, auto&&) {
        if (const auto box = weakBox.get()) {
            box.Focus(FocusState::Programmatic);
            box.SelectAll();
        }
    });
    flyout.Closed([this, playlist, name, cancelled, weakBox = make_weak(box)](auto&&, auto&&) {
        const auto box = weakBox.get();
        const hstring text = box ? box.Text() : name;
        if (*cancelled || text.empty() || text == name || !server_) return;
        PlaylistEdit edit;
        edit.kind = PlaylistEdit::Kind::Rename;
        edit.playlist = playlist;
        edit.name = toUtf8(std::wstring_view(text.c_str(), text.size()));
        auto weak = get_weak();
        auto dispatcher = DispatcherQueue();
        std::thread([weak, dispatcher, server = server_, edit] {
            try {
                server->editPlaylist(edit);
            } catch (const std::exception& e) {
                camel::log::warn("playlist rename: %s", e.what());
            }
            dispatcher.TryEnqueue([weak, playlist = edit.playlist] {
                if (auto self = weak.get()) {
                    self->refreshPlaylists();
                    get_self<PlaylistView>(self->Playlists())->reload(playlist);
                }
            });
        }).detach();
    });
    // An item under a collapsed Playlists entry is created by expanding it; a flyout needs it loaded.
    const auto show = [flyout, weakItem = make_weak(item)] {
        const auto item = weakItem.get();
        if (!item || !item.IsLoaded()) return;
        try {
            flyout.ShowAt(item);
        } catch (const hresult_error& e) {
            camel::log::warn("playlist rename box: 0x%08x", uint32_t(e.code()));
        }
    };
    if (item.IsLoaded())
        show();
    else {
        auto token = std::make_shared<event_token>();
        *token = item.Loaded([show, token, weakItem = make_weak(item)](auto&&, auto&&) {
            if (const auto item = weakItem.get()) item.Loaded(*token);
            show();
        });
    }
}

} // namespace winrt::CamelPlayer::implementation
