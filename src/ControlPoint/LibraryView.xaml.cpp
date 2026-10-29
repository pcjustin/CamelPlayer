// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pch.h"

#include "LibraryView.xaml.h"
#if __has_include("LibraryView.g.cpp")
#include "LibraryView.g.cpp"
#endif

#include <algorithm>
#include <random>
#include <utility>

#include "Common/util/Log.h"
#include "Common/util/Measure.h"
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
            folder = tr(L"Library_Folder"), artists = tr(L"Search_ArtistCount");
};

hstring counted(const hstring& format, int64_t n) {
    std::wstring text(format);
    if (const auto at = text.find(L"{0}"); at != std::wstring::npos) text.replace(at, 3, std::to_wstring(n));
    return hstring(text);
}

// Tags written with surrounding spaces, tabs or ideographic spaces show without them; a blank one is unknown.
std::string trimmed(std::string text) {
    static constexpr std::string_view kIdeographicSpace = "\xE3\x80\x80", kBlank = " \t\r\n";
    for (bool changed = true; changed;) {
        changed = false;
        while (!text.empty() && kBlank.find(text.back()) != std::string_view::npos) text.pop_back(), changed = true;
        while (!text.empty() && kBlank.find(text.front()) != std::string_view::npos) text.erase(0, 1), changed = true;
        if (text.ends_with(kIdeographicSpace)) text.resize(text.size() - kIdeographicSpace.size()), changed = true;
        if (text.starts_with(kIdeographicSpace)) text.erase(0, kIdeographicSpace.size()), changed = true;
    }
    return text;
}

hstring orUnknown(const std::string& text, const Texts& texts) {
    const auto name = trimmed(text);
    return name.empty() ? texts.unknown : ui::utf8(name);
}

// A track without a title tag shows its file name.
hstring titleOf(const std::string& title, const std::string& file, const Texts& texts) {
    if (!trimmed(title).empty() || file.empty()) return orUnknown(title, texts);
    return hstring(std::filesystem::path(camel::toWide(file)).stem().wstring());
}

// "FLAC · 24-bit / 96 kHz", "MP3 · 44.1 kHz" (a lossy file's bit depth is only its decoder's) or "DSD64".
hstring formatLabel(const LibraryAlbumFormat& f) {
    std::wstring name(camel::toWide(f.format));
    std::ranges::transform(name, name.begin(), towupper);
    if (f.format == "dsd" && f.sampleRate > 0) return hstring(L"DSD" + std::to_wstring(f.sampleRate / 44100));
    wchar_t rate[32]{};
    if (f.sampleRate % 1000 == 0)
        swprintf_s(rate, L"%lld kHz", static_cast<long long>(f.sampleRate / 1000));
    else
        swprintf_s(rate, L"%.1f kHz", double(f.sampleRate) / 1000.0);
    static constexpr std::string_view kLossy[] = {"mp3", "aac", "m4a", "ogg", "vorbis", "opus", "wma"};
    const bool lossy = std::ranges::find(kLossy, f.format) != std::end(kLossy);
    std::wstring text = name;
    if (f.sampleRate <= 0) return hstring(text);
    if (!text.empty()) text += L" · ";
    if (!lossy && f.bitsPerSample > 0) text += std::to_wstring(f.bitsPerSample) + L"-bit / ";
    return hstring(text + rate);
}

ui::BrowsePage albumPage(const LibraryAlbumPage& page, const Texts& texts) {
    ui::BrowsePage out{page.revision, page.total, {}};
    for (const auto& a : page.albums) {
        out.rows.push_back({a.id, {}, orUnknown(a.title, texts), ui::utf8(a.artist), {}, counted(texts.tracks, a.tracks)});
        out.rows.back().favorite = a.favorite;
    }
    return out;
}

hstring titleOf(const LibraryTrackRow& t, const Texts& texts) {
    return titleOf(t.title, t.name.empty() ? t.path : t.name, texts);
}

hstring duration(int64_t ms) {
    return hstring(ui::formatTime(double(ms) / 1000.0));
}

ui::BrowseRow trackRow(const LibraryTrackRow& t, const Texts& texts) {
    ui::BrowseRow row{t.id, t.path, titleOf(t, texts), ui::utf8(t.artist), ui::utf8(t.album), duration(t.durationMs)};
    row.file = t.track.file;
    row.favorite = t.favorite;
    return row;
}

ui::BrowseRow albumTrackRow(const LibraryTrackRow& t, bool discs, const Texts& texts) {
    std::wstring number;
    if (t.number) number = (discs ? std::to_wstring(t.disc) + L"-" : std::wstring{}) + std::to_wstring(t.number) + L". ";
    ui::BrowseRow row{
        t.id, t.path, hstring(number + std::wstring(titleOf(t, texts))), ui::utf8(t.artist), {}, duration(t.durationMs)};
    row.file = t.track.file;
    row.marks = true;
    row.favorite = t.favorite;
    return row;
}

// Ctrl- and Shift-clicks change the selection; they do not open or play.
bool selecting() {
    return GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_SHIFT) < 0;
}

Windows::Foundation::IInspectable item(const ui::BrowseRow& row) {
    auto item = make<BrowseItem>();
    get_self<BrowseItem>(item)->fill(row);
    return item;
}

ScrollViewer scrollViewerOf(DependencyObject const& root) {
    std::vector<DependencyObject> pending{root};
    for (size_t i = 0; i < pending.size(); ++i) {
        if (auto viewer = pending[i].try_as<ScrollViewer>()) return viewer;
        for (int32_t child = 0; child < Media::VisualTreeHelper::GetChildrenCount(pending[i]); ++child)
            pending.push_back(Media::VisualTreeHelper::GetChild(pending[i], child));
    }
    return nullptr;
}

} // namespace

void LibraryView::InitializeComponent() {
    LibraryViewT::InitializeComponent();
    ToolTipService::SetToolTip(DescendingToggle(), box_value(tr(L"Library_Descending")));
    Automation::AutomationProperties::SetName(DescendingToggle(), tr(L"Library_Descending"));
    Automation::AutomationProperties::SetName(BackButton(), tr(L"Library_Back"));
}

void LibraryView::attach(std::shared_ptr<IServer> server, std::function<void(Command)> submit, ui::PlaylistMenu playlistMenu,
                         std::function<void(ui::SourceFetch, std::string)> savePlaylist) {
    server_ = std::move(server);
    submit_ = std::move(submit);
    playlistMenu_ = std::move(playlistMenu);
    savePlaylist_ = std::move(savePlaylist);
    if (playlistMenu_)
        playlistMenu_(AlbumMenu(), [this]() -> ui::SourceFetch {
            return [album = current_.album] { return std::vector<PlaylistSource>{{PlaylistSource::Kind::Album, album, {}, {}}}; };
        });
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
    static constexpr std::pair<std::wstring_view, SmartList> kSmart[] = {{L"smart:added", SmartList::RecentlyAdded},
                                                                         {L"smart:mostPlayed", SmartList::MostPlayed},
                                                                         {L"smart:recentlyPlayed", SmartList::RecentlyPlayed},
                                                                         {L"smart:favorites", SmartList::Favorites},
                                                                         {L"smart:neverPlayed", SmartList::NeverPlayed}};
    for (const auto& [name, list] : kSmart)
        if (tag == name) view.smart = list;
    view.favoriteAlbums = tag == L"smart:favoriteAlbums";
    if (tag == L"albums" || view.favoriteAlbums)
        view.mode = Mode::Albums;
    else if (tag == L"tracks" || view.smart)
        view.mode = Mode::Tracks;
    else if (tag == L"folders")
        view.mode = Mode::Folders;
    else {
        view.mode = Mode::Groups;
        view.field = tag == L"composers" ? LibraryGroupField::Composer
                     : tag == L"genres"  ? LibraryGroupField::Genre
                     : tag == L"years"   ? LibraryGroupField::Year
                                         : LibraryGroupField::Artist;
    }
    back_.clear();
    leave_ = nullptr;
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

void LibraryView::showAlbum(int64_t album, hstring const& title, std::function<void()> leave, bool keepHistory) {
    if (!server_ || album <= 0) return;
    View view;
    view.mode = Mode::AlbumDetail;
    view.album = album;
    view.title = title;
    if (!keepHistory) {
        back_.clear();
        leave_ = std::move(leave);
    }
    open(std::move(view), keepHistory);
    refreshTimer_.Start();
}

void LibraryView::showArtist(hstring const& artist, std::function<void()> leave, bool keepHistory) {
    if (!server_ || artist.empty()) return;
    View view;
    view.mode = Mode::Albums;
    view.filter = LibraryGroupFilter{LibraryGroupField::Artist, camel::toUtf8(std::wstring_view(artist.c_str(), artist.size()))};
    view.title = artist;
    if (!keepHistory) {
        back_.clear();
        leave_ = std::move(leave);
    }
    open(std::move(view), keepHistory);
    refreshTimer_.Start();
}

void LibraryView::showAlbumList(hstring const& title, ui::BrowseFetch fetch, std::function<void()> leave) {
    View view;
    view.mode = Mode::Albums;
    view.title = title;
    view.fetch = std::move(fetch);
    back_.clear();
    leave_ = std::move(leave);
    open(std::move(view), false);
    refreshTimer_.Start();
}

void LibraryView::showArtistList(hstring const& title, ui::BrowseFetch fetch, std::function<void()> leave) {
    View view;
    view.mode = Mode::Groups;
    view.field = LibraryGroupField::Artist;
    view.title = title;
    view.fetch = std::move(fetch);
    back_.clear();
    leave_ = std::move(leave);
    open(std::move(view), false);
    refreshTimer_.Start();
}

void LibraryView::showTrackList(hstring const& title, ui::BrowseFetch fetch, std::function<void()> leave) {
    View view;
    view.mode = Mode::Tracks;
    view.title = title;
    view.fetch = std::move(fetch);
    back_.clear();
    leave_ = std::move(leave);
    open(std::move(view), false);
    refreshTimer_.Start();
}

void LibraryView::showSearch(hstring const& query, std::function<void()> leave) {
    if (!server_ || query.empty()) return;
    View view;
    view.mode = Mode::Search;
    view.query = camel::toUtf8(std::wstring_view(query.c_str(), query.size()));
    std::wstring title(tr(L"Search_Title"));
    if (const auto at = title.find(L"{0}"); at != std::wstring::npos) title.replace(at, 3, query.c_str());
    view.title = hstring(title);
    const bool keepHistory = !leave && (collection_ || current_.mode == Mode::AlbumDetail || current_.mode == Mode::Folders);
    if (!keepHistory) back_.clear();
    if (leave) leave_ = std::move(leave);
    open(std::move(view), keepHistory);
    refreshTimer_.Start();
}

// Albums and artists of a search; the matching tracks page through the list below them.
void LibraryView::loadSearchGroups() {
    constexpr int64_t kShown = 50;
    const int64_t requested = camel::measure::now();
    background([this, server = server_, query = current_.query, texts = Texts{}, requested] {
        auto results = std::make_shared<LibrarySearchResults>(server->searchLibrary(query, 0, kShown, std::nullopt));
        return std::function<void()>([this, results, texts, requested] {
            auto albums = single_threaded_observable_vector<Windows::Foundation::IInspectable>();
            for (const auto& a : results->albums)
                albums.Append(item({a.id, {}, orUnknown(a.title, texts), ui::utf8(a.artist), {}, {}}));
            auto artists = single_threaded_observable_vector<Windows::Foundation::IInspectable>();
            for (const auto& name : results->artists) artists.Append(item({0, name, orUnknown(name, texts), {}, {}, {}}));
            SearchAlbums().ItemsSource(albums);
            SearchArtists().ItemsSource(artists);
            const auto header = [](const wchar_t* key, int64_t total) { return tr(key) + L" · " + to_hstring(total); };
            SearchAlbumsHeader().Text(header(L"Search_Albums", results->totalAlbums));
            SearchArtistsHeader().Text(header(L"Search_Artists", results->totalArtists));
            SearchTracksHeader().Text(header(L"Search_Tracks", results->tracks.total));
            const auto shown = [](int64_t total) { return total ? Visibility::Visible : Visibility::Collapsed; };
            SearchAlbumsHeader().Visibility(shown(results->totalAlbums));
            SearchAlbums().Visibility(shown(results->totalAlbums));
            SearchArtistsHeader().Visibility(shown(results->totalArtists));
            SearchArtists().Visibility(shown(results->totalArtists));
            SearchTracksHeader().Visibility(shown(results->tracks.total));
            HeaderDetail().Text(counted(texts.tracks, results->tracks.total) + L" · " +
                                counted(texts.albums, results->totalAlbums) + L" · " +
                                counted(texts.artists, results->totalArtists));
            const bool none = !results->tracks.total && !results->totalAlbums && !results->totalArtists;
            EmptyText().Text(tr(L"Search_None"));
            EmptyText().Visibility(none ? Visibility::Visible : Visibility::Collapsed);
            if (camel::measure::measuring())
                camel::log::info("measure: search results page in %.1f ms",
                                 camel::measure::milliseconds(requested, camel::measure::now()));
        });
    });
}

void LibraryView::showEmpty(int64_t total) {
    // An album always has tracks; the search groups or Home explain an empty list.
    if (current_.mode == Mode::AlbumDetail || current_.mode == Mode::Search || current_.fetch) return;
    if (current_.smartList()) {
        EmptyText().Text(tr(L"Smart_Empty"));
        EmptyText().Visibility(total == 0 ? Visibility::Visible : Visibility::Collapsed);
        return;
    }
    const bool noRoots = hasRoots_ && !*hasRoots_;
    const bool pending = total == 0 && current_.mode == Mode::Albums && !current_.filter;
    if (total > 0 || (!noRoots && !pending)) {
        EmptyText().Visibility(Visibility::Collapsed);
        return;
    }
    EmptyText().Text(noRoots ? tr(L"Library_Empty/Text") : tr(L"Library_AlbumsPending"));
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
    AlbumFormats().Children().Clear();
    AlbumFormats().Visibility(Visibility::Collapsed);
    ListActions().Visibility(view.smartList() ? Visibility::Visible : Visibility::Collapsed);
    EmptyText().Visibility(Visibility::Collapsed);
    restoreOffset_.reset();
    if (push) {
        back_.push_back(current_);
        if (const auto viewer = scrollViewerOf(listOf(current_.mode))) back_.back().offset = viewer.VerticalOffset();
    }
    current_ = std::move(view);
    ++generation_;
    HeaderTitle().Text(current_.title);
    HeaderDetail().Text(current_.detail);
    BackButton().Visibility(back_.empty() && !leave_ ? Visibility::Collapsed : Visibility::Visible);
    SearchGroups().Visibility(current_.mode == Mode::Search ? Visibility::Visible : Visibility::Collapsed);
    SearchAlbums().ItemsSource(nullptr);
    SearchArtists().ItemsSource(nullptr);
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
    case Mode::Search:
        loadSearchGroups();
        startCollection();
        break;
    default:
        startCollection();
        break;
    }
}

void LibraryView::showList(Mode mode) {
    AlbumGrid().Visibility(mode == Mode::Albums ? Visibility::Visible : Visibility::Collapsed);
    RowList().Visibility(mode == Mode::Groups || mode == Mode::Tracks || mode == Mode::AlbumDetail || mode == Mode::Search
                             ? Visibility::Visible
                             : Visibility::Collapsed);
    FolderList().Visibility(mode == Mode::Folders ? Visibility::Visible : Visibility::Collapsed);
    FolderTrail().Visibility(mode == Mode::Folders && current_.folder ? Visibility::Visible : Visibility::Collapsed);
}

ListViewBase LibraryView::listOf(Mode mode) {
    if (mode == Mode::Albums) return AlbumGrid();
    return mode == Mode::Folders ? FolderList().as<ListViewBase>() : RowList().as<ListViewBase>();
}

void LibraryView::restoreOffset() {
    const auto offset = std::exchange(restoreOffset_, std::nullopt);
    if (!offset || *offset <= 0) return;
    const auto list = listOf(current_.mode);
    list.UpdateLayout();
    if (const auto viewer = scrollViewerOf(list)) viewer.ChangeView(nullptr, *offset, nullptr, true);
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
        keys = {L"Library_SortTitle", L"Library_SortArtist",   L"Library_SortAlbum",
                L"Library_SortYear",  L"Library_SortDuration", L"Library_SortAdded"};
        selected = int(trackSort_);
    }
    if (current_.fetch || current_.smartList()) keys.clear(); // Home's and smart lists keep their own order
    for (const auto* key : keys) SortCombo().Items().Append(box_value(tr(key)));
    SortCombo().Visibility(keys.empty() ? Visibility::Collapsed : Visibility::Visible);
    if (!keys.empty()) SortCombo().SelectedIndex(selected);
    DescendingToggle().Visibility(current_.mode == Mode::Tracks && !current_.smart && !current_.fetch ? Visibility::Visible
                                                                                                      : Visibility::Collapsed);
    DescendingToggle().IsChecked(descending_);
    populating_ = false;
}

void LibraryView::startCollection() {
    const Texts texts;
    auto server = server_;
    ui::BrowseFetch fetch;
    if (current_.fetch) {
        fetch = current_.fetch;
    } else if (current_.smart) {
        fetch = [server, texts, list = *current_.smart](int64_t offset, int64_t limit, std::optional<uint64_t> revision) {
            const auto page = server->smartTracks(list, offset, limit, revision);
            ui::BrowsePage out{page.revision, page.total, {}};
            for (const auto& t : page.tracks) {
                out.rows.push_back(trackRow(t, texts));
                out.rows.back().marks = true;
            }
            return out;
        };
    } else if (current_.favoriteAlbums) {
        fetch = [server, texts](int64_t offset, int64_t limit, std::optional<uint64_t> revision) {
            return albumPage(server->favoriteAlbums(offset, limit, revision), texts);
        };
    } else if (current_.mode == Mode::Albums) {
        fetch = [server, texts, sort = albumSort_, filter = current_.filter](int64_t offset, int64_t limit,
                                                                             std::optional<uint64_t> revision) {
            return albumPage(server->browseAlbums(sort, filter, offset, limit, revision), texts);
        };
    } else if (current_.mode == Mode::Groups) {
        fetch = [server, texts, field = current_.field](int64_t offset, int64_t limit, std::optional<uint64_t> revision) {
            const auto page = server->browseGroups(field, offset, limit, revision);
            ui::BrowsePage out{page.revision, page.total, {}};
            // Tracks not yet grouped into albums would make a zero count misleading.
            for (const auto& g : page.groups)
                out.rows.push_back({0,
                                    g.name,
                                    orUnknown(g.name, texts),
                                    g.albums ? counted(texts.albums, g.albums) : hstring{},
                                    {},
                                    counted(texts.tracks, g.tracks)});
            return out;
        };
    } else if (current_.mode == Mode::Search) {
        fetch = [server, texts, query = current_.query](int64_t offset, int64_t limit, std::optional<uint64_t> revision) {
            const auto page = server->searchTracks(query, offset, limit, revision);
            ui::BrowsePage out{page.revision, page.total, {}};
            for (const auto& t : page.tracks) {
                out.rows.push_back({t.id, t.path, titleOf(t.title, t.path, texts), ui::utf8(t.artist), ui::utf8(t.album), {}});
                out.rows.back().file = t.file;
            }
            return out;
        };
    } else {
        fetch = [server, texts, sort = trackSort_, descending = descending_](int64_t offset, int64_t limit,
                                                                             std::optional<uint64_t> revision) {
            const auto page = server->browseTrackRows(sort, descending, offset, limit, revision);
            ui::BrowsePage out{page.revision, page.total, {}};
            for (const auto& t : page.tracks) {
                out.rows.push_back(trackRow(t, texts));
                out.rows.back().marks = true;
            }
            return out;
        };
    }
    fetch_ = fetch;
    collection_ = make_self<ui::LibraryCollection>(DispatcherQueue(), std::move(fetch));
    const auto source = collection_.as<Windows::Foundation::Collections::IObservableVector<Windows::Foundation::IInspectable>>();
    (current_.mode == Mode::Albums ? AlbumGrid().as<ItemsControl>() : RowList().as<ItemsControl>()).ItemsSource(source);
    collection_->start([this](int64_t total) {
        showEmpty(total);
        restoreOffset();
    });
}

void LibraryView::loadAlbumDetail() {
    // The album wall's thumbnail, so the cover needs no reading of the music storage.
    ui::loadArtwork(DetailCover(), server_, {ArtworkKind::Album, current_.album, {}}, detailArtwork_);
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
            showAlbumFavorite(a.favorite);
            showAlbumFormats(detail->formats);
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
            restoreOffset();
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
    camel::measure::markPlayRequested();
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
        const auto track =
            std::find_if(detailTracks_.begin(), detailTracks_.end(), [&](const auto& t) { return t.id == row.id; });
        if (track != detailTracks_.end()) queueTracks({*track}, playNext);
    } else if ((current_.mode == Mode::Tracks || current_.mode == Mode::Folders || current_.mode == Mode::Search) &&
               !row.value.empty()) {
        Command command;
        command.kind = playNext ? CommandKind::QueuePlayNext : CommandKind::QueueAdd;
        command.sources.emplace_back(row.value);
        if (submit_) submit_(std::move(command));
    }
}

void LibraryView::queueFolder(int64_t folder, bool playNext, bool playNow) {
    if (!server_ || folder <= 0) return;
    background([this, server = server_, folder, playNext, playNow] {
        std::vector<int64_t> pending{folder};
        std::vector<LibraryTrackRow> tracks;
        for (size_t i = 0; i < pending.size(); ++i) {
            const auto listing = server->browseFolder(pending[i]);
            tracks.insert(tracks.end(), listing.tracks.begin(), listing.tracks.end());
            for (const auto& child : listing.folders) pending.push_back(child.id);
        }
        return std::function<void()>([this, tracks = std::move(tracks), playNext, playNow] {
            if (playNow)
                playTracks(tracks, 0);
            else
                queueTracks(tracks, playNext);
        });
    });
}

std::optional<PlaylistSource> LibraryView::sourceOf(Target target, Mode mode, LibraryGroupField field, const ui::BrowseRow& row) {
    using Kind = PlaylistSource::Kind;
    if (target == Target::Albums) {
        if (row.id) return PlaylistSource{Kind::Album, row.id, {}, {}};
    } else if (target == Target::Artists || mode == Mode::Groups) {
        if (row.value.empty()) return std::nullopt;
        if (target == Target::Artists || field == LibraryGroupField::Artist)
            return PlaylistSource{Kind::Artist, 0, {}, row.value};
        if (field == LibraryGroupField::Composer) return PlaylistSource{Kind::Composer, 0, {}, row.value};
        if (field == LibraryGroupField::Genre) return PlaylistSource{Kind::Genre, 0, {}, row.value};
        size_t used = 0;
        try {
            const auto year = std::stoll(row.value, &used);
            if (used == row.value.size() && year) return PlaylistSource{Kind::Year, year, {}, {}};
        } catch (const std::exception&) {
        }
    } else if (mode == Mode::Folders && row.folder) {
        if (row.id) return PlaylistSource{Kind::Folder, row.id, {}, {}};
    } else if (row.file) {
        return PlaylistSource{Kind::Track, 0, {row.file}, {}};
    }
    return std::nullopt;
}

std::function<std::vector<ui::BrowseRow>()>
LibraryView::selectionRows(ListViewBase const& list, Windows::Foundation::IInspectable const& clicked, bool paged) const {
    std::vector<std::pair<int64_t, int64_t>> ranges; // first index, count
    bool clickedSelected = false;
    uint32_t clickedIndex = 0;
    const bool found = list.Items().IndexOf(clicked, clickedIndex);
    for (const auto& range : list.SelectedRanges()) {
        ranges.emplace_back(range.FirstIndex(), int64_t(range.Length()));
        if (found && std::cmp_greater_equal(clickedIndex, range.FirstIndex()) &&
            std::cmp_less_equal(clickedIndex, range.LastIndex()))
            clickedSelected = true;
    }
    if (!clickedSelected) return [row = ui::LibraryCollection::rowOf(clicked)] { return std::vector<ui::BrowseRow>{row}; };
    if (paged && fetch_) {
        return [ranges, fetch = fetch_] {
            std::vector<ui::BrowseRow> rows;
            for (const auto& [first, count] : ranges)
                for (int64_t offset = first; offset < first + count; offset += 500) {
                    auto page = fetch(offset, std::min<int64_t>(500, first + count - offset), std::nullopt);
                    rows.insert(rows.end(), page.rows.begin(), page.rows.end());
                    if (page.rows.empty()) break;
                }
            return rows;
        };
    }
    std::vector<ui::BrowseRow> rows;
    const auto items = list.Items();
    for (const auto& [first, count] : ranges)
        for (int64_t i = first; i < first + count && std::cmp_less(i, items.Size()); ++i)
            rows.push_back(ui::LibraryCollection::rowOf(items.GetAt(uint32_t(i))));
    return [rows] { return rows; };
}

ui::SourceFetch LibraryView::menuSources() const {
    if (!menuItem_ || !menuList_) return {};
    return [rows = selectionRows(menuList_, menuItem_, menuPaged_), target = menuTarget_, mode = current_.mode,
            field = current_.field] {
        std::vector<PlaylistSource> sources;
        for (const auto& row : rows())
            if (auto source = sourceOf(target, mode, field, row)) sources.push_back(std::move(*source));
        return sources;
    };
}

void LibraryView::queueSelection(bool playNext) {
    if (!menuItem_ || !menuList_ || !server_) return;
    background([this, server = server_, rows = selectionRows(menuList_, menuItem_, menuPaged_),
                folders = current_.mode == Mode::Folders, playNext] {
        std::vector<LibraryTrackRow> tracks;
        for (const auto& row : rows()) {
            if (folders && row.folder) {
                std::vector<int64_t> pending{row.id};
                for (size_t i = 0; i < pending.size(); ++i) {
                    const auto listing = server->browseFolder(pending[i]);
                    tracks.insert(tracks.end(), listing.tracks.begin(), listing.tracks.end());
                    for (const auto& child : listing.folders) pending.push_back(child.id);
                }
            } else if (!row.value.empty()) {
                LibraryTrackRow track;
                track.path = row.value;
                tracks.push_back(std::move(track));
            }
        }
        return std::function<void()>([this, tracks = std::move(tracks), playNext] { queueTracks(tracks, playNext); });
    });
}

void LibraryView::markSelection(bool favorite) {
    if (!menuItem_ || !menuList_ || !submit_) return;
    background([this, rows = selectionRows(menuList_, menuItem_, menuPaged_), favorite, paged = menuPaged_] {
        std::vector<LibraryTrackKey> tracks;
        for (const auto& row : rows())
            if (row.file && !row.folder) tracks.push_back({row.file});
        return std::function<void()>([this, tracks = std::move(tracks), favorite, paged] {
            if (tracks.empty()) return;
            submit_(ui::favoriteCommand(tracks, favorite));
            if (paged) {
                reloadWhenIdle();
                return;
            }
            for (const auto& value : menuList_.Items()) {
                auto* item = get_self<BrowseItem>(value.as<CamelPlayer::BrowseItem>());
                if (std::ranges::find(tracks, LibraryTrackKey{item->row().file}) != tracks.end()) item->setFavorite(favorite);
            }
        });
    });
}

void LibraryView::resetSelection() {
    if (!menuItem_ || !menuList_ || !submit_) return;
    background([this, rows = selectionRows(menuList_, menuItem_, menuPaged_)] {
        std::vector<LibraryTrackKey> tracks;
        for (const auto& row : rows())
            if (row.file && !row.folder && std::ranges::find(tracks, LibraryTrackKey{row.file}) == tracks.end())
                tracks.push_back({row.file});
        return std::function<void()>([this, tracks = std::move(tracks)] {
            if (!tracks.empty()) confirmReset(tracks);
        });
    });
}

winrt::fire_and_forget LibraryView::confirmReset(std::vector<LibraryTrackKey> tracks) {
    auto strong = get_strong();
    ContentDialog dialog;
    dialog.XamlRoot(XamlRoot());
    dialog.Title(box_value(tr(L"Library_ResetStatisticsTitle")));
    dialog.Content(box_value(ui::format(L"Library_ResetStatisticsText", {std::to_wstring(tracks.size())})));
    dialog.PrimaryButtonText(tr(L"Library_ResetStatisticsPrimary"));
    dialog.CloseButtonText(tr(L"Playlist_Cancel"));
    dialog.DefaultButton(ContentDialogButton::Close);
    if (co_await dialog.ShowAsync() != ContentDialogResult::Primary || !submit_) co_return;
    Command command;
    command.kind = CommandKind::ResetPlayStatistics;
    command.tracks = std::move(tracks);
    submit_(std::move(command));
}

void LibraryView::markAlbums(bool favorite) {
    if (!menuItem_ || !menuList_ || !submit_) return;
    background([this, rows = selectionRows(menuList_, menuItem_, menuPaged_), favorite, paged = menuPaged_] {
        std::vector<int64_t> albums;
        for (const auto& row : rows())
            if (row.id) albums.push_back(row.id);
        return std::function<void()>([this, albums = std::move(albums), favorite, paged] {
            if (albums.empty()) return;
            submit_(ui::albumFavoriteCommand(albums, favorite));
            if (paged) {
                reloadWhenIdle();
                return;
            }
            for (const auto& value : menuList_.Items()) {
                auto* item = get_self<BrowseItem>(value.as<CamelPlayer::BrowseItem>());
                if (std::ranges::find(albums, item->row().id) != albums.end()) item->setFavorite(favorite);
            }
        });
    });
}

void LibraryView::reloadWhenIdle() {
    background([this, server = server_] {
        server->waitIdle();
        return std::function<void()>([this] {
            if (collection_) collection_->refresh();
        });
    });
}

std::function<std::vector<ui::BrowseRow>()> LibraryView::listRows() const {
    return [fetch = fetch_] {
        std::vector<ui::BrowseRow> rows;
        if (!fetch) return rows;
        for (int64_t offset = 0;;) {
            auto page = fetch(offset, 500, std::nullopt);
            rows.insert(rows.end(), page.rows.begin(), page.rows.end());
            offset += int64_t(page.rows.size());
            if (page.rows.empty() || offset >= page.total) return rows;
        }
    };
}

// Replaces the queue with the whole list: its tracks, or the tracks of its albums in album order.
void LibraryView::playList(bool shuffle) {
    if (!submit_ || !server_) return;
    background([server = server_, rows = listRows(), albums = current_.favoriteAlbums, shuffle, this] {
        std::vector<std::string> paths;
        for (const auto& row : rows()) {
            if (!albums) {
                if (!row.value.empty()) paths.push_back(row.value);
                continue;
            }
            for (const auto& t : server->albumDetail(row.id).tracks) paths.push_back(t.path);
        }
        if (shuffle) std::ranges::shuffle(paths, std::mt19937{std::random_device{}()});
        return std::function<void()>([this, paths = std::move(paths)] {
            if (paths.empty()) return;
            Command command;
            command.kind = CommandKind::QueueReplace;
            for (const auto& path : paths) command.sources.emplace_back(path);
            camel::measure::markPlayRequested();
            submit_(std::move(command));
        });
    });
}

void LibraryView::OnPlayListClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    playList(false);
}

void LibraryView::OnShuffleListClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    playList(true);
}

// A playlist named after the list, holding what it lists now.
void LibraryView::OnSaveListClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    if (!savePlaylist_) return;
    const auto title = current_.title;
    savePlaylist_(
        [rows = listRows(), albums = current_.favoriteAlbums] {
            std::vector<PlaylistSource> sources;
            for (const auto& row : rows()) {
                if (albums)
                    sources.push_back({PlaylistSource::Kind::Album, row.id, {}, {}});
                else if (row.file)
                    sources.push_back({PlaylistSource::Kind::Track, 0, {row.file}, {}});
            }
            return sources;
        },
        camel::toUtf8(std::wstring_view(title.c_str(), title.size())));
}

void LibraryView::showAlbumFormats(const std::vector<LibraryAlbumFormat>& formats) {
    AlbumFormats().Children().Clear();
    for (const auto& f : formats) {
        const auto label = formatLabel(f);
        if (label.empty()) continue;
        Border badge;
        badge.Padding({8, 2, 8, 2});
        badge.CornerRadius({10, 10, 10, 10});
        badge.BorderThickness({1, 1, 1, 1});
        const auto resources = Application::Current().Resources();
        badge.Background(resources.Lookup(box_value(L"ControlFillColorSecondaryBrush")).as<Media::Brush>());
        badge.BorderBrush(resources.Lookup(box_value(L"ControlStrokeColorDefaultBrush")).as<Media::Brush>());
        TextBlock text;
        text.Text(label);
        text.Style(resources.Lookup(box_value(L"CaptionTextBlockStyle")).as<Microsoft::UI::Xaml::Style>());
        badge.Child(text);
        if (formats.size() > 1) ToolTipService::SetToolTip(badge, box_value(counted(tr(L"Library_Tracks"), f.tracks)));
        AlbumFormats().Children().Append(badge);
    }
    AlbumFormats().Visibility(AlbumFormats().Children().Size() ? Visibility::Visible : Visibility::Collapsed);
}

void LibraryView::showAlbumFavorite(bool favorite) {
    AlbumFavoriteButton().IsChecked(favorite);
    AlbumFavoriteIcon().Glyph(favorite ? L"\xE00B" : L"\xE006");
}

void LibraryView::OnFavoriteClick(Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&) {
    ui::rowFavoriteClicked(sender, submit_);
}

void LibraryView::OnAlbumFavoriteClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    const bool favorite = AlbumFavoriteButton().IsChecked() && AlbumFavoriteButton().IsChecked().Value();
    showAlbumFavorite(favorite);
    if (submit_ && current_.album) submit_(ui::albumFavoriteCommand({current_.album}, favorite));
}

void LibraryView::showMenu(MenuFlyout const& menu, FrameworkElement const& element, Input::ContextRequestedEventArgs const& e) {
    Primitives::FlyoutShowOptions options;
    if (Windows::Foundation::Point point; e.TryGetPosition(element, point)) options.Position(point);
    menu.ShowAt(element, options);
    e.Handled(true);
}

void LibraryView::OnRowContextRequested(UIElement const& sender, Input::ContextRequestedEventArgs const& e) {
    const auto element = sender.as<FrameworkElement>();
    const auto item = element.DataContext().try_as<CamelPlayer::BrowseItem>();
    if (!item) return;
    const auto& row = ui::LibraryCollection::rowOf(item);
    const bool group = current_.mode == Mode::Groups && !row.value.empty();
    if (!group && !item.IsTrack() && (current_.mode != Mode::Folders || !item.IsFolder())) return;
    menuRow_ = row;
    menuItem_ = item;
    menuList_ = current_.mode == Mode::Folders ? FolderList().as<ListViewBase>() : RowList().as<ListViewBase>();
    menuTarget_ = Target::Rows;
    menuPaged_ = current_.mode == Mode::Groups || current_.mode == Mode::Tracks || current_.mode == Mode::Search;
    if (!rowMenu_) {
        rowMenu_ = MenuFlyout();
        MenuFlyoutItem playNow;
        playNow.Text(tr(L"Library_MenuPlayNow"));
        FontIcon playIcon;
        playIcon.Glyph(L"\xE768");
        playNow.Icon(playIcon);
        playNow.Click([this](auto&&, auto&&) { queueFolder(menuRow_.id, false, true); });
        rowMenu_.Items().Append(playNow);
        for (const auto& [key, glyph, playNext] :
             {std::tuple{L"Library_MenuPlayNext", L"\xE893", true}, std::tuple{L"Library_MenuAddQueue", L"\xE710", false}}) {
            MenuFlyoutItem entry;
            entry.Text(tr(key));
            FontIcon icon;
            icon.Glyph(glyph);
            entry.Icon(icon);
            entry.Click([this, playNext](auto&&, auto&&) { queueSelection(playNext); });
            rowMenu_.Items().Append(entry);
        }
        if (playlistMenu_) playlistMenu_(rowMenu_, [this] { return menuSources(); });
        rowMarks_ = ui::appendFavoriteMenu(rowMenu_, [this](bool favorite) { markSelection(favorite); });
        MenuFlyoutItem reset;
        reset.Text(tr(L"Library_MenuResetStatistics"));
        FontIcon resetIcon;
        resetIcon.Glyph(L"\xE72C");
        reset.Icon(resetIcon);
        reset.Click([this](auto&&, auto&&) { resetSelection(); });
        rowMenu_.Items().Append(reset);
        rowMarks_.push_back(reset);
    }
    for (const auto& entry : rowMarks_) entry.Visibility(item.IsTrack() ? Visibility::Visible : Visibility::Collapsed);
    // Groups have no queue actions; their first playlist entry follows a separator that is hidden with them.
    const auto queueVisibility = group ? Visibility::Collapsed : Visibility::Visible;
    rowMenu_.Items().GetAt(0).Visibility(menuRow_.folder ? Visibility::Visible : Visibility::Collapsed);
    for (uint32_t i = 1; i < 4 && i < rowMenu_.Items().Size(); ++i) rowMenu_.Items().GetAt(i).Visibility(queueVisibility);
    if (group && current_.field == LibraryGroupField::Year && !sourceOf(Target::Rows, Mode::Groups, LibraryGroupField::Year, row))
        return;
    showMenu(rowMenu_, element, e);
}

void LibraryView::OnCardContextRequested(UIElement const& sender, Input::ContextRequestedEventArgs const& e) {
    const auto element = sender.as<FrameworkElement>();
    const auto item = element.DataContext().try_as<CamelPlayer::BrowseItem>();
    if (!item || !playlistMenu_) return;
    const bool artist = unbox_value_or<hstring>(element.Tag(), L"") == L"artist";
    if (!sourceOf(artist ? Target::Artists : Target::Albums, current_.mode, current_.field, ui::LibraryCollection::rowOf(item)))
        return;
    menuItem_ = item;
    menuTarget_ = artist ? Target::Artists : Target::Albums;
    menuPaged_ = !artist && current_.mode == Mode::Albums;
    menuList_ = artist                          ? SearchArtists().as<ListViewBase>()
                : current_.mode == Mode::Search ? SearchAlbums().as<ListViewBase>()
                                                : AlbumGrid().as<ListViewBase>();
    if (!cardMenu_) {
        cardMenu_ = MenuFlyout();
        playlistMenu_(cardMenu_, [this] { return menuSources(); });
        cardMenu_.Items().RemoveAt(0); // the separator before the playlist entries
        cardMarksFirst_ = cardMenu_.Items().Size();
        ui::appendAlbumFavoriteMenu(cardMenu_, [this](bool favorite) { markAlbums(favorite); });
    }
    for (uint32_t i = cardMarksFirst_; i < cardMenu_.Items().Size(); ++i)
        cardMenu_.Items().GetAt(i).Visibility(artist ? Visibility::Collapsed : Visibility::Visible);
    showMenu(cardMenu_, element, e);
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
        if (auto self = weak.get())
            if (auto image = target.get()) self->loadRowArtwork(image);
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
    if (back_.empty() && leave_) {
        const auto leave = std::exchange(leave_, nullptr);
        leave();
        return;
    }
    if (back_.empty()) return;
    auto previous = std::move(back_.back());
    back_.pop_back();
    const double offset = previous.offset;
    open(std::move(previous), false);
    restoreOffset_ = offset;
}

void LibraryView::OnSortChanged(Windows::Foundation::IInspectable const&, SelectionChangedEventArgs const&) {
    const int index = SortCombo().SelectedIndex();
    if (populating_ || index < 0) return;
    if (current_.mode == Mode::Albums)
        albumSort_ = AlbumSort(index);
    else if (current_.mode == Mode::Tracks)
        trackSort_ = TrackSort(index);
    open(current_, false);
}

void LibraryView::OnDescendingClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    descending_ = DescendingToggle().IsChecked().Value();
    open(current_, false);
}

void LibraryView::OnAlbumClick(Windows::Foundation::IInspectable const&, ItemClickEventArgs const& e) {
    if (selecting()) return;
    const auto& row = ui::LibraryCollection::rowOf(e.ClickedItem());
    if (!row.id) return; // still a placeholder
    View view;
    view.mode = Mode::AlbumDetail;
    view.album = row.id;
    view.title = row.title;
    open(std::move(view), true);
}

void LibraryView::OnRowClick(Windows::Foundation::IInspectable const&, ItemClickEventArgs const& e) {
    if (selecting()) return;
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
        const auto found =
            std::find_if(detailTracks_.begin(), detailTracks_.end(), [&](const auto& t) { return t.id == row.id; });
        if (found != detailTracks_.end()) playTracks(detailTracks_, size_t(found - detailTracks_.begin()));
    } else if ((current_.mode == Mode::Tracks || current_.mode == Mode::Search) && !row.value.empty()) {
        LibraryTrackRow track;
        track.path = row.value;
        playTracks({track}, 0);
    }
}

void LibraryView::OnSearchRowScroll(Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&) {
    const auto button = sender.as<Button>();
    const std::wstring tag(unbox_value_or<hstring>(button.Tag(), L""));
    const auto list = tag.starts_with(L"albums") ? SearchAlbums().as<DependencyObject>() : SearchArtists().as<DependencyObject>();
    if (const auto viewer = scrollViewerOf(list)) {
        const bool forward = tag.ends_with(L"-forward");
        const double step = viewer.ViewportWidth() * 0.8;
        viewer.ChangeView(std::max(0.0, viewer.HorizontalOffset() + (forward ? step : -step)), nullptr, nullptr);
    }
}

void LibraryView::OnSearchArtistClick(Windows::Foundation::IInspectable const&, ItemClickEventArgs const& e) {
    const auto& row = ui::LibraryCollection::rowOf(e.ClickedItem());
    if (row.value.empty()) return;
    View view;
    view.mode = Mode::Albums;
    view.filter = LibraryGroupFilter{LibraryGroupField::Artist, row.value};
    view.title = row.title;
    open(std::move(view), true);
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
    if (selecting()) return;
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
