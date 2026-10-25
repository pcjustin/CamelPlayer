// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pch.h"

#include "LibraryView.xaml.h"
#if __has_include("LibraryView.g.cpp")
#include "LibraryView.g.cpp"
#endif

#include "Common/util/Log.h"
#include "Common/util/Text.h"
#include "UiText.h"
#include "ArtworkLoader.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace camel::server;
using ui::tr;

namespace winrt::CamelPlayer::implementation {
namespace {

// Localized texts are read on the UI thread and handed to background fetches.
struct Texts {
    hstring unknown = tr(L"Library_Unknown"), albums = tr(L"Library_Albums"), tracks = tr(L"Library_Tracks"),
            folder = tr(L"Library_Folder");
};

hstring counted(const hstring& format, int64_t n) {
    std::wstring text(format);
    if (const auto at = text.find(L"{0}"); at != std::wstring::npos) text.replace(at, 3, std::to_wstring(n));
    return hstring(text);
}

hstring orUnknown(const std::string& text, const Texts& texts) {
    return text.empty() ? texts.unknown : ui::utf8(text);
}

hstring duration(int64_t ms) {
    return hstring(ui::formatTime(double(ms) / 1000.0));
}

ui::BrowseRow trackRow(const LibraryTrackRow& t, const Texts& texts) {
    return {t.id, t.path, orUnknown(t.title, texts), ui::utf8(t.artist), ui::utf8(t.album), duration(t.durationMs)};
}

ui::BrowseRow albumTrackRow(const LibraryTrackRow& t, bool discs, const Texts& texts) {
    std::wstring number;
    if (t.number) number = (discs ? std::to_wstring(t.disc) + L"-" : std::wstring{}) + std::to_wstring(t.number) + L". ";
    return {t.id, t.path, hstring(number + std::wstring(orUnknown(t.title, texts))), ui::utf8(t.artist), {}, duration(t.durationMs)};
}

Windows::Foundation::IInspectable item(const ui::BrowseRow& row) {
    auto item = make<BrowseItem>();
    get_self<BrowseItem>(item)->fill(row);
    return item;
}

} // namespace

void LibraryView::InitializeComponent() {
    LibraryViewT::InitializeComponent();
    ToolTipService::SetToolTip(DescendingToggle(), box_value(tr(L"Library_Descending")));
    Automation::AutomationProperties::SetName(DescendingToggle(), tr(L"Library_Descending"));
    Automation::AutomationProperties::SetName(BackButton(), tr(L"Library_Back"));
}

void LibraryView::attach(std::shared_ptr<IServer> server, std::function<void(Command)> submit) {
    server_ = std::move(server);
    submit_ = std::move(submit);
    using namespace std::chrono_literals;
    refreshTimer_ = DispatcherQueue().CreateTimer();
    refreshTimer_.Interval(3s); // scans change the library continuously; refresh at a pace that keeps scrolling smooth
    refreshTimer_.Tick([this](auto&&, auto&&) {
        if (collection_) collection_->refresh();
    });
}

void LibraryView::show(hstring const& tag, hstring const& title) {
    if (!server_) return;
    View view;
    view.title = title;
    if (tag == L"albums") view.mode = Mode::Albums;
    else if (tag == L"tracks") view.mode = Mode::Tracks;
    else if (tag == L"folders") view.mode = Mode::Folders;
    else {
        view.mode = Mode::Groups;
        view.field = tag == L"composers" ? LibraryGroupField::Composer
                     : tag == L"genres"  ? LibraryGroupField::Genre
                     : tag == L"years"   ? LibraryGroupField::Year
                                         : LibraryGroupField::Artist;
    }
    back_.clear();
    open(std::move(view), false);
    refreshTimer_.Start();
    EmptyText().Visibility(Visibility::Collapsed);
    background([this, server = server_] {
        const bool roots = !server->libraryRoots().empty();
        return std::function<void()>([this, roots] {
            hasRoots_ = roots;
            if (!roots) showEmpty(0);
        });
    });
}

void LibraryView::showAlbum(int64_t album, hstring const& title) {
    if (!server_ || album <= 0) return;
    View view;
    view.mode = Mode::AlbumDetail;
    view.album = album;
    view.title = title;
    back_.clear();
    open(std::move(view), false);
    refreshTimer_.Start();
}

void LibraryView::showArtist(hstring const& artist) {
    if (!server_ || artist.empty()) return;
    View view;
    view.mode = Mode::Albums;
    view.filter = LibraryGroupFilter{LibraryGroupField::Artist, camel::toUtf8(std::wstring_view(artist.c_str(), artist.size()))};
    view.title = artist;
    back_.clear();
    open(std::move(view), false);
    refreshTimer_.Start();
}

void LibraryView::showEmpty(int64_t total) {
    const bool noRoots = hasRoots_ && !*hasRoots_;
    const bool pending = total == 0 && current_.mode == Mode::Albums && !current_.filter;
    if (total > 0 || (!noRoots && !pending)) {
        EmptyText().Visibility(Visibility::Collapsed);
        return;
    }
    EmptyText().Text(tr(noRoots ? L"Library_Empty" : L"Library_AlbumsPending"));
    EmptyText().Visibility(Visibility::Visible);
}

void LibraryView::setActive(bool active) {
    if (!refreshTimer_ || !collection_) return;
    if (active) {
        refreshTimer_.Start();
        collection_->refresh();
    } else {
        refreshTimer_.Stop();
    }
}

void LibraryView::hide() {
    cancelArtwork();
    if (refreshTimer_) refreshTimer_.Stop();
    ++generation_;
    collection_ = nullptr;
    AlbumGrid().ItemsSource(nullptr);
    RowList().ItemsSource(nullptr);
}

void LibraryView::open(View view, bool push) {
    cancelArtwork();
    DetailCover().Source(nullptr);
    DetailCover().Visibility(view.mode == Mode::AlbumDetail ? Visibility::Visible : Visibility::Collapsed);
    AlbumActions().Visibility(view.mode == Mode::AlbumDetail ? Visibility::Visible : Visibility::Collapsed);
    if (push) back_.push_back(current_);
    current_ = std::move(view);
    ++generation_;
    HeaderTitle().Text(current_.title);
    HeaderDetail().Text(current_.detail);
    BackButton().Visibility(back_.empty() ? Visibility::Collapsed : Visibility::Visible);
    fillSortChoices();
    collection_ = nullptr;
    AlbumGrid().ItemsSource(nullptr);
    RowList().ItemsSource(nullptr);
    showList(current_.mode);
    switch (current_.mode) {
    case Mode::AlbumDetail:
        loadAlbumDetail();
        break;
    case Mode::Folders:
        loadFolder();
        break;
    default:
        startCollection();
        break;
    }
}

void LibraryView::showList(Mode mode) {
    AlbumGrid().Visibility(mode == Mode::Albums ? Visibility::Visible : Visibility::Collapsed);
    RowList().Visibility(mode == Mode::Groups || mode == Mode::Tracks || mode == Mode::AlbumDetail ? Visibility::Visible
                                                                                                  : Visibility::Collapsed);
    FolderList().Visibility(mode == Mode::Folders ? Visibility::Visible : Visibility::Collapsed);
    FolderTrail().Visibility(mode == Mode::Folders && current_.folder ? Visibility::Visible : Visibility::Collapsed);
}

void LibraryView::fillSortChoices() {
    populating_ = true;
    SortCombo().Items().Clear();
    std::vector<const wchar_t*> keys;
    int selected = 0;
    if (current_.mode == Mode::Albums) {
        keys = {L"Library_SortTitle", L"Library_SortArtist", L"Library_SortYear", L"Library_SortAdded"};
        selected = int(albumSort_);
    } else if (current_.mode == Mode::Tracks) {
        keys = {L"Library_SortTitle", L"Library_SortArtist", L"Library_SortAlbum",
                L"Library_SortYear",  L"Library_SortDuration", L"Library_SortAdded"};
        selected = int(trackSort_);
    }
    for (const auto* key : keys) SortCombo().Items().Append(box_value(tr(key)));
    SortCombo().Visibility(keys.empty() ? Visibility::Collapsed : Visibility::Visible);
    if (!keys.empty()) SortCombo().SelectedIndex(selected);
    DescendingToggle().Visibility(current_.mode == Mode::Tracks ? Visibility::Visible : Visibility::Collapsed);
    DescendingToggle().IsChecked(descending_);
    populating_ = false;
}

void LibraryView::startCollection() {
    const Texts texts;
    auto server = server_;
    ui::BrowseFetch fetch;
    if (current_.mode == Mode::Albums) {
        fetch = [server, texts, sort = albumSort_, filter = current_.filter](int64_t offset, int64_t limit,
                                                                              std::optional<uint64_t> revision) {
            const auto page = server->browseAlbums(sort, filter, offset, limit, revision);
            ui::BrowsePage out{page.revision, page.total, {}};
            for (const auto& a : page.albums)
                out.rows.push_back({a.id, {}, orUnknown(a.title, texts), ui::utf8(a.artist), {}, counted(texts.tracks, a.tracks)});
            return out;
        };
    } else if (current_.mode == Mode::Groups) {
        fetch = [server, texts, field = current_.field](int64_t offset, int64_t limit, std::optional<uint64_t> revision) {
            const auto page = server->browseGroups(field, offset, limit, revision);
            ui::BrowsePage out{page.revision, page.total, {}};
            // Tracks not yet grouped into albums would make a zero count misleading.
            for (const auto& g : page.groups)
                out.rows.push_back({0, g.name, ui::utf8(g.name), g.albums ? counted(texts.albums, g.albums) : hstring{}, {},
                                    counted(texts.tracks, g.tracks)});
            return out;
        };
    } else {
        fetch = [server, texts, sort = trackSort_, descending = descending_](int64_t offset, int64_t limit,
                                                                              std::optional<uint64_t> revision) {
            const auto page = server->browseTrackRows(sort, descending, offset, limit, revision);
            ui::BrowsePage out{page.revision, page.total, {}};
            for (const auto& t : page.tracks) out.rows.push_back(trackRow(t, texts));
            return out;
        };
    }
    collection_ = make_self<ui::LibraryCollection>(DispatcherQueue(), std::move(fetch));
    const auto source = collection_.as<Windows::Foundation::Collections::IObservableVector<Windows::Foundation::IInspectable>>();
    (current_.mode == Mode::Albums ? AlbumGrid().as<ItemsControl>() : RowList().as<ItemsControl>()).ItemsSource(source);
    collection_->start([this](int64_t total) { showEmpty(total); });
}

void LibraryView::loadAlbumDetail() {
    ui::loadArtwork(DetailCover(), server_, {ArtworkKind::Album, current_.album, {}, 600}, detailArtwork_);
    detailTracks_.clear();
    background([this, server = server_, id = current_.album, texts = Texts{}] {
        auto detail = std::make_shared<LibraryAlbumDetail>(server->albumDetail(id));
        return std::function<void()>([this, detail, texts] {
            const auto& a = detail->album;
            std::wstring info(ui::utf8(a.artist));
            if (a.year) info += (info.empty() ? L"" : L" · ") + std::to_wstring(a.year);
            info += (info.empty() ? L"" : L" · ") + std::wstring(counted(texts.tracks, a.tracks)) + L" · " +
                    std::wstring(duration(a.durationMs));
            HeaderTitle().Text(orUnknown(a.title, texts));
            HeaderDetail().Text(hstring(info));
            auto items = single_threaded_observable_vector<Windows::Foundation::IInspectable>();
            for (const auto& t : detail->tracks) items.Append(item(albumTrackRow(t, a.discs > 1, texts)));
            detailTracks_ = std::move(detail->tracks);
            RowList().ItemsSource(items);
        });
    });
}

void LibraryView::loadFolder() {
    FolderList().ItemsSource(nullptr);
    FolderTrail().ItemsSource(nullptr);
    folderTrail_.clear();
    folderTracks_.clear();
    background([this, server = server_, folder = current_.folder, texts = Texts{}] {
        auto listing = std::make_shared<LibraryFolderListing>(server->browseFolder(folder));
        return std::function<void()>([this, listing, texts] {
            auto items = single_threaded_observable_vector<Windows::Foundation::IInspectable>();
            for (const auto& child : listing->folders) {
                ui::BrowseRow row{child.id, child.path, ui::utf8(child.name), ui::utf8(child.location)};
                row.folder = true;
                items.Append(item(row));
            }
            for (const auto& t : listing->tracks) items.Append(item(trackRow(t, texts)));
            folderTracks_ = listing->tracks;
            FolderList().ItemsSource(items);
            if (listing->trail.empty()) return;
            auto names = single_threaded_vector<Windows::Foundation::IInspectable>();
            for (const auto& step : listing->trail) names.Append(box_value(ui::utf8(step.name)));
            folderTrail_ = listing->trail;
            FolderTrail().ItemsSource(names);
            HeaderTitle().Text(ui::utf8(folderTrail_.back().name));
        });
    });
}

void LibraryView::playTracks(const std::vector<LibraryTrackRow>& tracks, size_t index) {
    if (!submit_ || index >= tracks.size()) return;
    Command command;
    command.kind = CommandKind::QueueReplace;
    for (const auto& t : tracks) command.sources.emplace_back(t.path);
    command.index = index;
    submit_(std::move(command));
}

void LibraryView::queueTracks(const std::vector<LibraryTrackRow>& tracks, bool playNext) {
    if (!submit_ || tracks.empty()) return;
    Command command;
    command.kind = playNext ? CommandKind::QueuePlayNext : CommandKind::QueueAdd;
    for (const auto& track : tracks) command.sources.emplace_back(track.path);
    submit_(std::move(command));
}

void LibraryView::queueAlbum(int64_t album, bool playNext) {
    if (!server_ || album <= 0) return;
    background([this, server = server_, album, playNext] {
        auto tracks = server->albumDetail(album).tracks;
        return std::function<void()>([this, tracks = std::move(tracks), playNext] { queueTracks(tracks, playNext); });
    });
}

void LibraryView::playAlbum(int64_t album) {
    if (!server_ || album <= 0) return;
    background([this, server = server_, album] {
        auto tracks = server->albumDetail(album).tracks;
        return std::function<void()>([this, tracks = std::move(tracks)] { playTracks(tracks, 0); });
    });
}

void LibraryView::queueItem(const ui::BrowseRow& row, bool playNext) {
    if (current_.mode == Mode::Folders && row.folder) {
        queueFolder(row.id, playNext);
    } else if (current_.mode == Mode::AlbumDetail) {
        const auto track = std::find_if(detailTracks_.begin(), detailTracks_.end(), [&](const auto& t) { return t.id == row.id; });
        if (track != detailTracks_.end()) queueTracks({*track}, playNext);
    } else if ((current_.mode == Mode::Tracks || current_.mode == Mode::Folders) && !row.value.empty()) {
        Command command;
        command.kind = playNext ? CommandKind::QueuePlayNext : CommandKind::QueueAdd;
        command.sources.emplace_back(row.value);
        if (submit_) submit_(std::move(command));
    }
}

void LibraryView::queueFolder(int64_t folder, bool playNext) {
    if (!server_ || folder <= 0) return;
    background([this, server = server_, folder, playNext] {
        std::vector<int64_t> pending{folder};
        std::vector<LibraryTrackRow> tracks;
        for (size_t i = 0; i < pending.size(); ++i) {
            const auto listing = server->browseFolder(pending[i]);
            tracks.insert(tracks.end(), listing.tracks.begin(), listing.tracks.end());
            for (const auto& child : listing.folders) pending.push_back(child.id);
        }
        return std::function<void()>([this, tracks = std::move(tracks), playNext] { queueTracks(tracks, playNext); });
    });
}

void LibraryView::OnRowContextRequested(UIElement const& sender, Input::ContextRequestedEventArgs const& e) {
    const auto element = sender.as<FrameworkElement>();
    const auto item = element.DataContext().try_as<CamelPlayer::BrowseItem>();
    if (!item || !(item.IsTrack() || (current_.mode == Mode::Folders && item.IsFolder()))) return;
    menuRow_ = ui::LibraryCollection::rowOf(item);
    if (!rowMenu_) {
        rowMenu_ = MenuFlyout();
        for (const auto& [key, glyph, playNext] : {std::tuple{L"Library_MenuPlayNext", L"\xE893", true},
                                                   std::tuple{L"Library_MenuAddQueue", L"\xE710", false}}) {
            MenuFlyoutItem entry;
            entry.Text(tr(key));
            FontIcon icon;
            icon.Glyph(glyph);
            entry.Icon(icon);
            entry.Click([this, playNext](auto&&, auto&&) { queueItem(menuRow_, playNext); });
            rowMenu_.Items().Append(entry);
        }
    }
    Primitives::FlyoutShowOptions options;
    if (Windows::Foundation::Point point; e.TryGetPosition(element, point)) options.Position(point);
    rowMenu_.ShowAt(element, options);
    e.Handled(true);
}

void LibraryView::OnAddAlbumClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    queueAlbum(current_.album, false);
}

void LibraryView::OnPlayNextAlbumClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    queueAlbum(current_.album, true);
}

void LibraryView::OnPlayAlbumClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    playAlbum(current_.album);
}

winrt::fire_and_forget LibraryView::background(std::function<std::function<void()>()> work) {
    auto weak = get_weak();
    auto queue = DispatcherQueue();
    const auto generation = generation_;
    co_await winrt::resume_background();
    std::function<void()> apply;
    try {
        apply = work();
    } catch (const std::exception& e) {
        camel::log::warn("library view: %s", e.what());
        co_return;
    }
    queue.TryEnqueue([weak, generation, apply] {
        auto self = weak.get();
        if (self && self->generation_ == generation && apply) apply();
    });
}

void LibraryView::cancelArtwork() {
    detailArtwork_.cancel();
    detailArtwork_ = {};
    for (auto& [key, slot] : artworkSlots_) slot.token.cancel();
    artworkSlots_.clear();
}

void LibraryView::loadRowArtwork(Image const& image) {
    const auto it = artworkSlots_.find(get_abi(image));
    if (it == artworkSlots_.end()) return;
    it->second.token.cancel();
    it->second.token = {};
    image.Source(nullptr);
    const auto value = image.DataContext().try_as<CamelPlayer::BrowseItem>();
    if (!value) return;
    const auto& row = get_self<BrowseItem>(value)->row();
    const bool trackCover = image.Name() == L"TrackCover";
    if (!row.id || (image.Name() == L"FolderCover" && !row.folder) || (trackCover && row.folder)) return;
    const auto kind = row.folder ? ArtworkKind::Folder : trackCover ? ArtworkKind::Track : ArtworkKind::Album;
    ui::loadArtwork(image, server_, {kind, row.id, trackCover ? row.value : std::string{}}, it->second.token);
}

void LibraryView::bindArtwork(Image const& image) {
    if (const auto old = artworkSlots_.find(get_abi(image)); old != artworkSlots_.end()) {
        old->second.token.cancel();
        artworkSlots_.erase(old);
    }
    image.Source(nullptr);
    if (!image.IsLoaded()) return;
    const auto value = image.DataContext().try_as<CamelPlayer::BrowseItem>();
    if (!value) return;
    auto& slot = artworkSlots_[get_abi(image)];
    slot.changed = value.PropertyChanged(auto_revoke, [weak = get_weak(), target = make_weak(image)](auto&&, auto&&) {
        if (auto self = weak.get()) if (auto image = target.get()) self->loadRowArtwork(image);
    });
    loadRowArtwork(image);
}

void LibraryView::OnArtworkLoaded(Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&) {
    bindArtwork(sender.as<Image>());
}

void LibraryView::OnArtworkUnloaded(Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&) {
    const auto it = artworkSlots_.find(get_abi(sender.as<Image>()));
    if (it != artworkSlots_.end()) {
        it->second.token.cancel();
        artworkSlots_.erase(it);
    }
    sender.as<Image>().Source(nullptr);
}

void LibraryView::OnArtworkContextChanged(FrameworkElement const& sender, DataContextChangedEventArgs const&) {
    bindArtwork(sender.as<Image>());
}

void LibraryView::OnBackClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    if (back_.empty()) return;
    auto previous = std::move(back_.back());
    back_.pop_back();
    open(std::move(previous), false);
}

void LibraryView::OnSortChanged(Windows::Foundation::IInspectable const&, SelectionChangedEventArgs const&) {
    const int index = SortCombo().SelectedIndex();
    if (populating_ || index < 0) return;
    if (current_.mode == Mode::Albums) albumSort_ = AlbumSort(index);
    else if (current_.mode == Mode::Tracks) trackSort_ = TrackSort(index);
    open(current_, false);
}

void LibraryView::OnDescendingClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    descending_ = DescendingToggle().IsChecked().Value();
    open(current_, false);
}

void LibraryView::OnAlbumClick(Windows::Foundation::IInspectable const&, ItemClickEventArgs const& e) {
    const auto& row = ui::LibraryCollection::rowOf(e.ClickedItem());
    if (!row.id) return; // still a placeholder
    View view;
    view.mode = Mode::AlbumDetail;
    view.album = row.id;
    view.title = row.title;
    open(std::move(view), true);
}

void LibraryView::OnRowClick(Windows::Foundation::IInspectable const&, ItemClickEventArgs const& e) {
    const auto& row = ui::LibraryCollection::rowOf(e.ClickedItem());
    if (current_.mode == Mode::Groups) {
        if (row.value.empty()) return;
        View view;
        view.mode = Mode::Albums;
        view.filter = LibraryGroupFilter{current_.field, row.value};
        view.title = row.title;
        view.detail = current_.title;
        open(std::move(view), true);
    } else if (current_.mode == Mode::AlbumDetail) {
        const auto found = std::find_if(detailTracks_.begin(), detailTracks_.end(), [&](const auto& t) { return t.id == row.id; });
        if (found != detailTracks_.end()) playTracks(detailTracks_, size_t(found - detailTracks_.begin()));
    } else if (current_.mode == Mode::Tracks && !row.value.empty()) {
        LibraryTrackRow track;
        track.path = row.value;
        playTracks({track}, 0);
    }
}

void LibraryView::OnTrailClicked(BreadcrumbBar const&, BreadcrumbBarItemClickedEventArgs const& e) {
    const auto index = size_t(e.Index());
    if (index + 1 >= folderTrail_.size()) return; // the folder already shown
    View view;
    view.mode = Mode::Folders;
    view.folder = folderTrail_[index].id;
    view.title = ui::utf8(folderTrail_[index].name);
    open(std::move(view), true);
}

void LibraryView::OnFolderItemClick(Windows::Foundation::IInspectable const&, ItemClickEventArgs const& e) {
    const auto& row = ui::LibraryCollection::rowOf(e.ClickedItem());
    if (row.folder) {
        View view;
        view.mode = Mode::Folders;
        view.folder = row.id;
        view.title = row.title;
        open(std::move(view), true);
        return;
    }
    const auto found = std::find_if(folderTracks_.begin(), folderTracks_.end(), [&](const auto& t) { return t.id == row.id; });
    if (found != folderTracks_.end()) playTracks(folderTracks_, size_t(found - folderTracks_.begin()));
}

} // namespace winrt::CamelPlayer::implementation
