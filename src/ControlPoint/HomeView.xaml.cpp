// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pch.h"

#include "HomeView.xaml.h"
#if __has_include("HomeView.g.cpp")
#include "HomeView.g.cpp"
#endif

#include <future>

#include "ArtworkLoader.h"
#include "BrowseItem.h"
#include "UiText.h"
#include "Common/util/Log.h"
#include "Common/util/Measure.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace camel::server;
using ui::tr;

namespace winrt::CamelPlayer::implementation {
namespace {

constexpr double kCoverSlot = 176; // a 160 px tile, its gap and its container margin
constexpr int64_t kRowItems = 30;

hstring formatted(const wchar_t* key, std::initializer_list<int64_t> values) {
    std::wstring text(tr(key));
    int index = 0;
    for (const auto value : values) {
        const std::wstring marker = L"{" + std::to_wstring(index++) + L"}";
        if (const auto at = text.find(marker); at != std::wstring::npos) text.replace(at, marker.size(), std::to_wstring(value));
    }
    return hstring(text);
}

Windows::Foundation::IInspectable item(const ui::BrowseRow& row) {
    auto result = make<BrowseItem>();
    get_self<BrowseItem>(result)->fill(row);
    return result;
}

enum class Start { Day, Month, Year };

// Local midnight `daysBack` days before today, or the first day of this month or year, as UTC milliseconds.
int64_t localStartMs(int daysBack, Start from = Start::Day) {
    SYSTEMTIME local{};
    GetLocalTime(&local);
    local.wHour = local.wMinute = local.wSecond = local.wMilliseconds = 0;
    if (from != Start::Day) local.wDay = 1;
    if (from == Start::Year) local.wMonth = 1;
    FILETIME file{};
    SystemTimeToFileTime(&local, &file);
    ULARGE_INTEGER ticks{{file.dwLowDateTime, file.dwHighDateTime}};
    ticks.QuadPart -= uint64_t(daysBack) * 24 * 3600 * 10'000'000ULL;
    file = {ticks.LowPart, ticks.HighPart};
    SYSTEMTIME start{}, utc{};
    FileTimeToSystemTime(&file, &start);
    TzSpecificLocalTimeToSystemTime(nullptr, &start, &utc);
    SystemTimeToFileTime(&utc, &file);
    ticks = {{file.dwLowDateTime, file.dwHighDateTime}};
    return int64_t((ticks.QuadPart - 116'444'736'000'000'000ULL) / 10'000);
}

struct HomeData {
    PlayOverview today, week, month, year;
    PlayStatistics all;
    LibraryAlbumPage played, added;
    int64_t tracks = 0, artists = 0;
};

} // namespace

void HomeView::attach(std::shared_ptr<IServer> server, Handlers handlers) {
    server_ = std::move(server);
    handlers_ = std::move(handlers);
}

winrt::fire_and_forget HomeView::refresh() {
    if (!server_) co_return;
    const uint64_t generation = ++generation_;
    auto weak = get_weak();
    auto server = server_;
    auto queue = DispatcherQueue();
    co_await winrt::resume_background();
    const auto queryStart = camel::measure::now();
    auto data = std::make_shared<HomeData>();
    try {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        const auto overview = [&](std::optional<int64_t> from, int64_t top) {
            PlayStatisticsQuery query;
            query.period.fromMs = from;
            query.topLimit = top;
            query.historyLimit = 0;
            return server->playStatistics(query);
        };
        // Albums, tracks and artists each order the whole library first (a few hundred ms on a large one), so they run
        // on their own readers and Home waits for the slowest instead of the sum.
        auto added = std::async(std::launch::async,
                                [&] { return server->browseAlbums(AlbumSort::Added, std::nullopt, 0, kRowItems, std::nullopt); });
        auto tracks = std::async(std::launch::async,
                                 [&] { return server->browseTrackRows(TrackSort::Title, false, 0, 1, std::nullopt).total; });
        auto artists = std::async(std::launch::async,
                                  [&] { return server->browseGroups(LibraryGroupField::Artist, 0, 1, std::nullopt).total; });
        data->today = overview(localStartMs(0), 1).overview;
        data->week = overview(localStartMs((now.wDayOfWeek + 6) % 7), 1).overview; // weeks start on Monday
        data->month = overview(localStartMs(0, Start::Month), 1).overview;
        data->year = overview(localStartMs(0, Start::Year), 1).overview;
        data->all = overview(std::nullopt, kRowItems);
        data->played = server->recentlyPlayedAlbums(0, kRowItems);
        data->added = added.get();
        data->tracks = tracks.get();
        data->artists = artists.get();
    } catch (const std::exception&) {
        co_return; // no library yet, or it changed while reading; the next visit rereads
    }
    if (camel::measure::measuring())
        camel::log::info("measure: home data read in %.0f ms, %.0f ms after process start",
                         camel::measure::milliseconds(queryStart, camel::measure::now()), camel::measure::sinceProcessStart());
    queue.TryEnqueue([weak, generation, data] {
        auto self = weak.get();
        if (!self || generation != self->generation_) return;
        // Minutes under an hour, hours and minutes within a day, then days and hours.
        const auto time = [](const PlayOverview& o) {
            const int64_t minutes = o.listenedMs / 60'000;
            if (minutes < 60) return formatted(L"Home_Minutes", {minutes});
            return formatted(L"Home_HoursMinutes", {minutes / 60, minutes % 60}); // hours, never days
        };
        const auto plays = [](const PlayOverview& o) { return formatted(L"Home_Plays", {o.plays}); };
        self->TodayTime().Text(time(data->today));
        self->TodayPlays().Text(plays(data->today));
        self->WeekTime().Text(time(data->week));
        self->WeekPlays().Text(plays(data->week));
        self->MonthTime().Text(time(data->month));
        self->MonthPlays().Text(plays(data->month));
        self->YearTime().Text(time(data->year));
        self->YearPlays().Text(plays(data->year));
        self->AllTime().Text(time(data->all.overview));
        self->AllPlays().Text(plays(data->all.overview));

        const auto albums = [](const LibraryAlbumPage& page) {
            std::vector<Windows::Foundation::IInspectable> out;
            out.reserve(page.albums.size());
            for (const auto& a : page.albums) out.push_back(item({a.id, {}, ui::utf8(a.title), ui::utf8(a.artist), {}, {}}));
            return out;
        };
        self->rows_[L"PlayedRow"] = albums(data->played);
        self->rows_[L"AddedRow"] = albums(data->added);
        std::vector<Windows::Foundation::IInspectable> top;
        for (const auto& a : data->all.albums)
            if (a.plays > 0) // listened to, but never long enough to count as a play
                top.push_back(
                    item({a.album,
                          {},
                          ui::utf8(a.name),
                          hstring(std::wstring(ui::utf8(a.artist)) + L" · " + std::wstring(formatted(L"Home_Plays", {a.plays}))),
                          {},
                          {}}));
        self->rows_[L"TopAlbumsRow"] = std::move(top);
        auto artists = single_threaded_observable_vector<Windows::Foundation::IInspectable>();
        for (size_t i = 0; i < data->all.artists.size() && artists.Size() < 10; ++i) {
            const auto& a = data->all.artists[i];
            if (a.plays > 0) artists.Append(item({0, a.name, ui::utf8(a.name), formatted(L"Home_Plays", {a.plays}), {}, {}}));
        }
        self->TopArtistsRow().ItemsSource(artists);
        for (const auto& row : {self->PlayedRow(), self->AddedRow(), self->TopAlbumsRow()}) self->fill(row);
        self->PlayedEmpty().Visibility(data->played.albums.empty() ? Visibility::Visible : Visibility::Collapsed);

        hstring counts = formatted(L"Library_Albums", {data->added.total}) + L" · " +
                         formatted(L"Library_Tracks", {data->tracks}) + L" · " +
                         formatted(L"Search_ArtistCount", {data->artists});
        self->LibraryCounts().Text(data->tracks ? counts : tr(L"Library_Empty/Text"));
        if (camel::measure::measuring())
            camel::log::info("measure: home shown %.0f ms after process start", camel::measure::sinceProcessStart());
    });
}

void HomeView::fill(ItemsControl const& row) {
    const auto found = rows_.find(std::wstring(row.Name()));
    if (found == rows_.end()) return;
    const auto fit = size_t(std::max(1.0, std::floor((row.ActualWidth() + 4) / kCoverSlot)));
    auto shown = single_threaded_observable_vector<Windows::Foundation::IInspectable>();
    for (size_t i = 0; i < found->second.size() && i < fit; ++i) shown.Append(found->second[i]);
    row.ItemsSource(shown);
}

void HomeView::OnRowSizeChanged(Windows::Foundation::IInspectable const& sender, SizeChangedEventArgs const& e) {
    if (std::floor(e.PreviousSize().Width / kCoverSlot) != std::floor(e.NewSize().Width / kCoverSlot))
        fill(sender.as<ItemsControl>());
}

void HomeView::OnAlbumClick(Windows::Foundation::IInspectable const&, ItemClickEventArgs const& e) {
    const auto& row = get_self<BrowseItem>(e.ClickedItem().as<CamelPlayer::BrowseItem>())->row();
    if (row.id > 0 && handlers_.openAlbum) handlers_.openAlbum(row.id, row.title);
}

void HomeView::OnArtistClick(Windows::Foundation::IInspectable const&, ItemClickEventArgs const& e) {
    const auto& row = get_self<BrowseItem>(e.ClickedItem().as<CamelPlayer::BrowseItem>())->row();
    if (!row.value.empty() && handlers_.openArtist) handlers_.openArtist(ui::utf8(row.value));
}

void HomeView::OnViewAllClick(Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&) {
    if (handlers_.viewAll) handlers_.viewAll(unbox_value_or<hstring>(sender.as<FrameworkElement>().Tag(), L""));
}

void HomeView::OnStatTileTapped(Windows::Foundation::IInspectable const& sender, Input::TappedRoutedEventArgs const&) {
    if (handlers_.openStats) handlers_.openStats(unbox_value_or<hstring>(sender.as<FrameworkElement>().Tag(), L""));
}

void HomeView::OnCoverLoaded(Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&) {
    const auto image = sender.as<Image>();
    const auto value = image.DataContext().try_as<CamelPlayer::BrowseItem>();
    if (!value || !server_) return;
    const auto& row = get_self<BrowseItem>(value)->row();
    if (camel::measure::measuring() && !covers_.contains(winrt::get_abi(image)))
        image.RegisterPropertyChangedCallback(Image::SourceProperty(), [](DependencyObject const& sender,
                                                                          DependencyProperty const&) {
            if (sender.as<Image>().Source())
                camel::log::info("measure: home cover shown %.0f ms after process start", camel::measure::sinceProcessStart());
        });
    auto& token = covers_[winrt::get_abi(image)];
    token.cancel();
    token = {};
    if (row.id > 0) ui::loadArtwork(image, server_, {ArtworkKind::Album, row.id, {}}, token);
}

void HomeView::OnCoverUnloaded(Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&) {
    const auto image = sender.as<Image>();
    if (const auto found = covers_.find(winrt::get_abi(image)); found != covers_.end()) {
        found->second.cancel();
        covers_.erase(found);
    }
    image.Source(nullptr);
}

void HomeView::cancelCovers() {
    for (auto& [image, token] : covers_) token.cancel();
    covers_.clear();
}

} // namespace winrt::CamelPlayer::implementation
