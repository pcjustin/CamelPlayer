// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pch.h"

#include "PlaylistView.xaml.h"
#if __has_include("PlaylistView.g.cpp")
#include "PlaylistView.g.cpp"
#endif

#include <shobjidl.h>

#include <algorithm>
#include <filesystem>
#include <set>
#include <thread>

#include "Common/util/Log.h"
#include "Common/util/Text.h"
#include "TrackMarks.h"
#include "UiText.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace camel::server;
using ui::tr;

namespace winrt::CamelPlayer::implementation {
namespace {

constexpr COMDLG_FILTERSPEC kPlaylistFiles[] = {{L"M3U8", L"*.m3u8;*.m3u"}};

std::optional<std::filesystem::path> pickPlaylistFile(HWND owner) {
    auto dialog = winrt::create_instance<IFileOpenDialog>(CLSID_FileOpenDialog);
    dialog->SetFileTypes(1, kPlaylistFiles);
    winrt::com_ptr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dialog->Show(owner)) || FAILED(dialog->GetResult(item.put())) ||
        FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return std::nullopt;
    std::filesystem::path result(path);
    CoTaskMemFree(path);
    return result;
}

std::optional<std::filesystem::path> pickExportFile(HWND owner, const hstring& name) {
    auto dialog = winrt::create_instance<IFileSaveDialog>(CLSID_FileSaveDialog);
    constexpr COMDLG_FILTERSPEC types[] = {{L"M3U8", L"*.m3u8"}};
    dialog->SetFileTypes(1, types);
    dialog->SetDefaultExtension(L"m3u8");
    std::wstring file(name);
    std::replace_if(file.begin(), file.end(), [](wchar_t c) { return std::wstring_view(L"<>:\"/\\|?*").find(c) != std::wstring_view::npos; }, L'_');
    dialog->SetFileName(file.c_str());
    winrt::com_ptr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dialog->Show(owner)) || FAILED(dialog->GetResult(item.put())) ||
        FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return std::nullopt;
    std::filesystem::path result(path);
    CoTaskMemFree(path);
    return result;
}

ContentDialog dialog(XamlRoot const& root, hstring const& title, hstring const& primary) {
    ContentDialog dialog;
    dialog.XamlRoot(root);
    dialog.Title(box_value(title));
    dialog.PrimaryButtonText(primary);
    dialog.CloseButtonText(tr(L"Playlist_Cancel"));
    dialog.DefaultButton(ContentDialogButton::Primary);
    return dialog;
}

} // namespace

void PlaylistView::attach(std::shared_ptr<IServer> server, Handlers handlers) {
    server_ = std::move(server);
    handlers_ = std::move(handlers);
    EntryList().ItemsSource(entries_.as<Windows::Foundation::Collections::IObservableVector<Windows::Foundation::IInspectable>>());
    ToolTipService::SetToolTip(MoreSortButton(), box_value(tr(L"Playlist_SortMore")));
    Automation::AutomationProperties::SetName(MoreSortButton(), tr(L"Playlist_SortMore"));
}

void PlaylistView::showAll() {
    playlist_ = 0;
    ++generation_;
    HeaderTitle().Text(tr(L"Playlist_PageTitle"));
    HeaderDetail().Text(L"");
    PlaylistActions().Visibility(Visibility::Collapsed);
    ColumnHeaders().Visibility(Visibility::Collapsed);
    EntryList().Visibility(Visibility::Collapsed);
    ImportButton().Visibility(Visibility::Visible);
    PlaylistList().Visibility(Visibility::Visible);
    EmptyText().Visibility(Visibility::Collapsed);
    entries_->reset({});
    load();
}

void PlaylistView::showPlaylist(int64_t playlist) {
    if (playlist <= 0) return showAll();
    if (playlist != playlist_) sortKey_.reset();
    playlist_ = playlist;
    ++generation_;
    HeaderTitle().Text(L"");
    HeaderDetail().Text(L"");
    PlaylistActions().Visibility(Visibility::Visible);
    ColumnHeaders().Visibility(Visibility::Visible);
    EntryList().Visibility(Visibility::Visible);
    ImportButton().Visibility(Visibility::Collapsed);
    PlaylistList().Visibility(Visibility::Collapsed);
    EmptyText().Visibility(Visibility::Collapsed);
    entries_->reset({});
    load();
}

void PlaylistView::hide() {
    ++generation_;
    entries_->reset({});
    PlaylistList().ItemsSource(nullptr);
}

void PlaylistView::reload(int64_t playlist) {
    if (Visibility() == Visibility::Visible && (playlist_ == 0 || playlist_ == playlist)) load();
}

void PlaylistView::load() {
    if (!server_) return;
    if (playlist_ == 0) {
        background([this, server = server_, tracks = tr(L"Library_Tracks")] {
            auto list = std::make_shared<std::vector<PlaylistSummary>>(server->playlists());
            return std::function<void()>([this, list, tracks] {
                playlists_ = *list;
                auto items = single_threaded_observable_vector<Windows::Foundation::IInspectable>();
                for (const auto& p : playlists_) {
                    ui::BrowseRow row{p.id, {}, ui::utf8(p.name)};
                    std::wstring count(tracks);
                    if (const auto at = count.find(L"{0}"); at != std::wstring::npos) count.replace(at, 3, std::to_wstring(p.tracks));
                    row.trailing = hstring(count);
                    auto item = make<BrowseItem>();
                    get_self<BrowseItem>(item)->fill(row);
                    items.Append(item);
                }
                PlaylistList().ItemsSource(items);
                HeaderDetail().Text(ui::format(L"Playlist_Count", {std::to_wstring(playlists_.size())}));
                EmptyText().Text(tr(L"Playlist_None"));
                EmptyText().Visibility(playlists_.empty() ? Visibility::Visible : Visibility::Collapsed);
            });
        });
        return;
    }
    struct Loaded {
        hstring name;
        int64_t revision = 0, durationMs = 0, unavailable = 0;
        std::vector<ui::BrowseRow> rows;
    };
    background([this, server = server_, playlist = playlist_] {
        auto loaded = std::make_shared<Loaded>();
        for (const auto& p : server->playlists())
            if (p.id == playlist) loaded->name = ui::utf8(p.name);
        for (int64_t offset = 0;;) {
            const auto page = server->playlistItems(playlist, offset, 500);
            loaded->revision = page.revision;
            for (const auto& item : page.items) {
                ui::BrowseRow row;
                row.id = item.id;
                row.value = item.path;
                const auto file = camel::toWide(item.name.empty() ? item.path : item.name); // paths are case-folded
                row.title = item.title.empty() ? hstring(std::filesystem::path(file).stem().wstring()) : ui::utf8(item.title);
                row.subtitle = ui::utf8(item.artist);
                row.detail = ui::utf8(item.album);
                if (item.durationMs > 0) row.trailing = hstring(ui::formatTime(double(item.durationMs) / 1000.0));
                row.file = item.track ? item.track->file : 0;
                row.unavailable = !item.available;
                row.marks = row.file != 0;
                row.favorite = item.favorite;
                loaded->durationMs += item.durationMs;
                loaded->unavailable += !item.available;
                loaded->rows.push_back(std::move(row));
            }
            offset += int64_t(page.items.size());
            if (page.items.empty() || offset >= page.total) break;
        }
        return std::function<void()>([this, loaded] {
            name_ = loaded->name;
            revision_ = loaded->revision;
            HeaderTitle().Text(name_);
            const auto total = loaded->rows.size();
            std::wstring detail(ui::format(L"Library_Tracks", {std::to_wstring(total)}));
            if (loaded->durationMs) detail += L" · " + ui::formatTime(double(loaded->durationMs) / 1000.0);
            if (loaded->unavailable) detail += L" · " + std::wstring(ui::format(L"Playlist_UnavailableCount", {std::to_wstring(loaded->unavailable)}));
            if (total > ui::PlaylistCollection::kShown)
                detail += L" · " + std::wstring(ui::format(L"Playlist_Listed", {std::to_wstring(ui::PlaylistCollection::kShown)}));
            HeaderDetail().Text(hstring(detail));
            entries_->reset(std::move(loaded->rows));
            EmptyText().Text(tr(L"Playlist_EmptyPlaylist"));
            EmptyText().Visibility(total ? Visibility::Collapsed : Visibility::Visible);
        });
    });
}

void PlaylistView::play(size_t index) {
    const auto& rows = entries_->rows();
    if (!handlers_.submit || index >= rows.size()) return;
    Command command;
    command.kind = CommandKind::QueueReplace;
    for (const auto& row : rows) command.sources.emplace_back(row.value);
    command.index = index;
    handlers_.submit(std::move(command));
}

std::vector<ui::BrowseRow> PlaylistView::selectedRows(Windows::Foundation::IInspectable const& clicked) {
    const auto& rows = entries_->rows();
    std::vector<ui::BrowseRow> out;
    bool clickedSelected = !clicked;
    uint32_t clickedIndex = 0;
    if (clicked && entries_->IndexOf(clicked, clickedIndex)) {
        for (const auto& range : EntryList().SelectedRanges())
            if (clickedIndex >= uint32_t(range.FirstIndex()) && clickedIndex <= uint32_t(range.LastIndex())) clickedSelected = true;
    }
    if (!clickedSelected) return {ui::PlaylistCollection::rowOf(clicked)};
    for (const auto& range : EntryList().SelectedRanges())
        for (auto i = range.FirstIndex(); i <= range.LastIndex() && size_t(i) < rows.size(); ++i) out.push_back(rows[size_t(i)]);
    return out;
}

void PlaylistView::queue(const std::vector<ui::BrowseRow>& rows, bool playNext) {
    if (!handlers_.submit || rows.empty()) return;
    Command command;
    command.kind = playNext ? CommandKind::QueuePlayNext : CommandKind::QueueAdd;
    for (const auto& row : rows) command.sources.emplace_back(row.value);
    handlers_.submit(std::move(command));
}

void PlaylistView::remove(const std::vector<ui::BrowseRow>& rows) {
    if (rows.empty()) return;
    PlaylistEdit change;
    change.kind = PlaylistEdit::Kind::Remove;
    change.playlist = playlist_;
    std::set<int64_t> ids;
    for (const auto& row : rows) {
        change.items.push_back(row.id);
        ids.insert(row.id);
    }
    entries_->remove(ids);
    edit(std::move(change), true);
}

void PlaylistView::edit(PlaylistEdit change, bool reloadAfter) {
    change.revision = revision_;
    const bool renamedOrDeleted = change.kind == PlaylistEdit::Kind::Rename || change.kind == PlaylistEdit::Kind::Delete;
    auto weak = get_weak();
    auto queue = DispatcherQueue();
    std::thread([weak, queue, server = server_, change, reloadAfter, renamedOrDeleted] {
        std::optional<PlaylistChange> result;
        try {
            result = server->editPlaylist(change);
        } catch (const std::exception& e) {
            camel::log::warn("playlist edit: %s", e.what());
        }
        queue.TryEnqueue([weak, result, change, reloadAfter, renamedOrDeleted] {
            auto self = weak.get();
            if (!self) return;
            if (!result) {
                if (self->handlers_.notify) self->handlers_.notify(tr(L"Playlist_EditFailed"), {}, true);
                if (self->playlist_ == change.playlist) self->load();
                return;
            }
            if (self->playlist_ == change.playlist) {
                self->revision_ = result->revision;
                if (reloadAfter) self->load();
            }
            if (renamedOrDeleted && self->handlers_.changed) self->handlers_.changed();
            if (change.kind == PlaylistEdit::Kind::Delete && self->handlers_.open) self->handlers_.open(0);
        });
    }).detach();
}

void PlaylistView::OnPlayClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    play(0);
}

winrt::fire_and_forget PlaylistView::OnRenameClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    auto strong = get_strong();
    auto box = dialog(XamlRoot(), tr(L"Playlist_RenameTitle"), tr(L"Playlist_RenamePrimary"));
    TextBox name;
    name.Text(name_);
    name.SelectAll();
    box.Content(name);
    if (co_await box.ShowAsync() != ContentDialogResult::Primary) co_return;
    const auto text = name.Text();
    if (text.empty() || text == name_) co_return;
    PlaylistEdit change;
    change.kind = PlaylistEdit::Kind::Rename;
    change.playlist = playlist_;
    change.name = camel::toUtf8(std::wstring_view(text.c_str(), text.size()));
    edit(std::move(change), true);
}

winrt::fire_and_forget PlaylistView::OnDeleteClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    auto strong = get_strong();
    auto box = dialog(XamlRoot(), ui::format(L"Playlist_DeleteTitle", {std::wstring(name_)}), tr(L"Playlist_DeletePrimary"));
    box.Content(box_value(tr(L"Playlist_DeleteText")));
    box.DefaultButton(ContentDialogButton::Close);
    if (co_await box.ShowAsync() != ContentDialogResult::Primary) co_return;
    PlaylistEdit change;
    change.kind = PlaylistEdit::Kind::Delete;
    change.playlist = playlist_;
    edit(std::move(change), false);
}

winrt::fire_and_forget PlaylistView::OnColumnClick(Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&) {
    auto strong = get_strong();
    const auto element = sender.as<FrameworkElement>();
    const auto key = PlaylistSortKey(std::stoi(std::wstring(unbox_value_or<hstring>(element.Tag(), L"0"))));
    const bool descending = sortKey_ == key && !sortDescending_;
    hstring column;
    if (const auto item = sender.try_as<MenuFlyoutItem>()) column = item.Text();
    else if (const auto text = sender.as<Button>().Content().try_as<TextBlock>()) column = text.Text();
    auto box = dialog(XamlRoot(), tr(L"Playlist_SortTitle"), tr(L"Playlist_SortPrimary"));
    box.Content(box_value(ui::format(descending ? L"Playlist_SortTextDescending" : L"Playlist_SortText", {std::wstring(column)})));
    if (co_await box.ShowAsync() != ContentDialogResult::Primary) co_return;
    sortKey_ = key;
    sortDescending_ = descending;
    PlaylistEdit change;
    change.kind = PlaylistEdit::Kind::Sort;
    change.playlist = playlist_;
    change.key = key;
    change.descending = descending;
    edit(std::move(change), true);
}

winrt::fire_and_forget PlaylistView::OnExportClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    auto strong = get_strong();
    const auto file = pickExportFile(handlers_.window ? handlers_.window() : nullptr, name_);
    if (!file || !server_) co_return;
    auto server = server_;
    const auto playlist = playlist_;
    auto weak = get_weak();
    auto queue = DispatcherQueue();
    co_await resume_background();
    bool done = true;
    try {
        server->exportPlaylist(playlist, *file);
    } catch (const std::exception& e) {
        camel::log::warn("playlist export: %s", e.what());
        done = false;
    }
    queue.TryEnqueue([weak, done, file = *file] {
        auto self = weak.get();
        if (!self || !self->handlers_.notify) return;
        if (done) self->handlers_.notify(tr(L"Playlist_Exported"), hstring(file.wstring()), false);
        else self->handlers_.notify(tr(L"Playlist_ExportFailed"), hstring(file.wstring()), true);
    });
}

winrt::fire_and_forget PlaylistView::OnImportClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    auto strong = get_strong();
    const auto file = pickPlaylistFile(handlers_.window ? handlers_.window() : nullptr);
    if (!file || !server_) co_return;
    auto server = server_;
    auto weak = get_weak();
    auto queue = DispatcherQueue();
    co_await resume_background();
    std::optional<PlaylistChange> change;
    try {
        change = server->importPlaylist(*file);
    } catch (const std::exception& e) {
        camel::log::warn("playlist import: %s", e.what());
    }
    queue.TryEnqueue([weak, change, file = *file] {
        auto self = weak.get();
        if (!self) return;
        if (!change) {
            if (self->handlers_.notify) self->handlers_.notify(tr(L"Playlist_ImportFailed"), hstring(file.wstring()), true);
            return;
        }
        std::wstring message(ui::format(L"Library_Tracks", {std::to_wstring(change->added)}));
        if (change->skippedUnavailable)
            message += L" · " + std::wstring(ui::format(L"Playlist_SkippedUrls", {std::to_wstring(change->skippedUnavailable)}));
        if (self->handlers_.changed) self->handlers_.changed();
        if (self->handlers_.notify)
            self->handlers_.notify(ui::format(L"Playlist_Imported", {std::wstring(ui::utf8(change->name))}), hstring(message), false);
        if (self->handlers_.open) self->handlers_.open(change->playlist);
    });
}

void PlaylistView::OnPlaylistClick(Windows::Foundation::IInspectable const&, ItemClickEventArgs const& e) {
    const auto& row = get_self<BrowseItem>(e.ClickedItem().as<CamelPlayer::BrowseItem>())->row();
    if (handlers_.open && row.id) handlers_.open(row.id);
}

void PlaylistView::OnEntryDoubleTapped(Windows::Foundation::IInspectable const&, Input::DoubleTappedRoutedEventArgs const& e) {
    const auto element = e.OriginalSource().try_as<FrameworkElement>();
    if (!element || !element.DataContext()) return;
    uint32_t index = 0;
    if (entries_->IndexOf(element.DataContext(), index)) play(index);
}

void PlaylistView::OnEntryKeyDown(Windows::Foundation::IInspectable const&, Input::KeyRoutedEventArgs const& e) {
    if (e.Key() != Windows::System::VirtualKey::Delete) return;
    e.Handled(true);
    remove(selectedRows());
}

void PlaylistView::OnEntryDragCompleted(ListViewBase const&, DragItemsCompletedEventArgs const& e) {
    if (e.DropResult() != Windows::ApplicationModel::DataTransfer::DataPackageOperation::Move) return;
    std::vector<std::pair<uint32_t, int64_t>> moved;
    for (const auto& item : e.Items())
        if (uint32_t index = 0; entries_->IndexOf(item, index)) moved.emplace_back(index, ui::PlaylistCollection::rowOf(item).id);
    if (moved.empty()) return;
    std::sort(moved.begin(), moved.end());
    PlaylistEdit change;
    change.kind = PlaylistEdit::Kind::Move;
    change.playlist = playlist_;
    change.position = moved.front().first; // the moved entries sit together, so no other entry is before them
    for (const auto& [index, id] : moved) change.items.push_back(id);
    edit(std::move(change), false);
}

// Every entry of the marked files shows the change, also when the list makes its item again.
void PlaylistView::submitMarks(Command command) {
    std::vector<int64_t> files;
    for (const auto& track : command.tracks) files.push_back(track.file);
    entries_->mark(files, command.favorite);
    if (handlers_.submit) handlers_.submit(std::move(command));
}

void PlaylistView::OnFavoriteClick(Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&) {
    ui::rowFavoriteClicked(sender, [this](Command command) { submitMarks(std::move(command)); });
}

void PlaylistView::OnEntryContextRequested(UIElement const& sender, Input::ContextRequestedEventArgs const& e) {
    const auto element = sender.as<FrameworkElement>();
    menuItem_ = element.DataContext();
    if (!menuItem_) return;
    if (!entryMenu_) {
        entryMenu_ = MenuFlyout();
        const auto add = [this](const wchar_t* key, const wchar_t* glyph, std::function<void()> action) {
            MenuFlyoutItem entry;
            entry.Text(tr(key));
            FontIcon icon;
            icon.Glyph(glyph);
            entry.Icon(icon);
            entry.Click([action](auto&&, auto&&) { action(); });
            entryMenu_.Items().Append(entry);
        };
        add(L"Library_MenuPlayNext", L"\xE893", [this] { queue(selectedRows(menuItem_), true); });
        add(L"Library_MenuAddQueue", L"\xE710", [this] { queue(selectedRows(menuItem_), false); });
        add(L"Playlist_Remove", L"\xE738", [this] { remove(selectedRows(menuItem_)); });
        if (handlers_.menu)
            handlers_.menu(entryMenu_, [this]() -> ui::SourceFetch {
                std::vector<PlaylistSource> sources;
                for (const auto& row : selectedRows(menuItem_))
                    if (row.file) sources.push_back({PlaylistSource::Kind::Track, 0, {row.file}, {}});
                return [sources] { return sources; };
            });
        ui::appendFavoriteMenu(entryMenu_, [this](bool favorite) {
            std::vector<LibraryTrackKey> tracks;
            for (const auto& row : selectedRows(menuItem_))
                if (row.file && std::ranges::find(tracks, LibraryTrackKey{row.file}) == tracks.end()) tracks.push_back({row.file});
            if (!tracks.empty()) submitMarks(ui::favoriteCommand(tracks, favorite));
        });
    }
    Primitives::FlyoutShowOptions options;
    if (Windows::Foundation::Point point; e.TryGetPosition(element, point)) options.Position(point);
    entryMenu_.ShowAt(element, options);
    e.Handled(true);
}

winrt::fire_and_forget PlaylistView::background(std::function<std::function<void()>()> work) {
    auto weak = get_weak();
    auto queue = DispatcherQueue();
    const auto generation = generation_;
    co_await winrt::resume_background();
    std::function<void()> apply;
    try {
        apply = work();
    } catch (const std::exception& e) {
        camel::log::warn("playlist view: %s", e.what());
        co_return;
    }
    queue.TryEnqueue([weak, generation, apply] {
        auto self = weak.get();
        if (self && self->generation_ == generation && apply) apply();
    });
}

} // namespace winrt::CamelPlayer::implementation
