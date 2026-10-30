// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pch.h"

#include "HomeView.xaml.h"
#if __has_include("HomeView.g.cpp")
#include "HomeView.g.cpp"
#endif

#include <filesystem>
#include <future>

#include <winrt/Microsoft.UI.Xaml.Shapes.h>

#include "ArtworkLoader.h"
#include "BrowseItem.h"
#include "CardSize.h"
#include "TrackMarks.h"
#include "UiText.h"
#include "Common/util/Log.h"
#include "Common/util/Measure.h"
#include "Common/util/Text.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace camel::server;
using ui::tr;

namespace winrt::CamelPlayer::implementation {
namespace {

constexpr double kCoverExtra = 16;    // the gap after a tile and its container margin
constexpr double kCoverRowExtra = 90; // a row's height beyond its covers
constexpr int64_t kRowItems = 30;
constexpr int64_t kListRows = 10;

hstring formatted(const wchar_t* key, std::initializer_list<int64_t> values) {
    std::wstring text(tr(key));
    int index = 0;
    for (const auto value : values) {
        const std::wstring marker = L"{" + std::to_wstring(index++) + L"}";
        if (const auto at = text.find(marker); at != std::wstring::npos) text.replace(at, marker.size(), std::to_wstring(value));
    }
    return hstring(text);
}

// Minutes under an hour, then hours and minutes, never days.
hstring listeningTime(int64_t ms) {
    const int64_t minutes = ms / 60'000;
    if (minutes < 60) return formatted(L"Home_Minutes", {minutes});
    return formatted(L"Home_HoursMinutes", {minutes / 60, minutes % 60});
}

hstring plays(int64_t n) {
    return formatted(L"Home_Plays", {n});
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

using Date = std::chrono::year_month_day;

Date today() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    return std::chrono::year{now.wYear} / std::chrono::month{now.wMonth} / std::chrono::day{now.wDay};
}

// Local midnight of a date, as UTC milliseconds.
int64_t midnightMs(Date date) {
    SYSTEMTIME local{};
    local.wYear = WORD(int(date.year()));
    local.wMonth = WORD(unsigned(date.month()));
    local.wDay = WORD(unsigned(date.day()));
    SYSTEMTIME utc{};
    TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc);
    FILETIME file{};
    SystemTimeToFileTime(&utc, &file);
    const ULARGE_INTEGER ticks{{file.dwLowDateTime, file.dwHighDateTime}};
    return int64_t((ticks.QuadPart - 116'444'736'000'000'000ULL) / 10'000);
}

Date parseDate(const std::string& text) {
    int y = 0, m = 0, d = 0;
    sscanf_s(text.c_str(), "%d-%d-%d", &y, &m, &d);
    return std::chrono::year{y} / std::chrono::month{unsigned(m)} / std::chrono::day{unsigned(d)};
}

std::wstring dateText(Date date) {
    wchar_t text[16];
    swprintf_s(text, L"%04d-%02u-%02u", int(date.year()), unsigned(date.month()), unsigned(date.day()));
    return text;
}

Date mondayOf(Date date) {
    const std::chrono::weekday weekday{std::chrono::sys_days(date)};
    return Date(std::chrono::sys_days(date) - std::chrono::days((weekday.c_encoding() + 6) % 7));
}

// A play's local start time, as the recorded UTC offset gave it.
hstring startedText(int64_t startedMs, int32_t offsetMinutes) {
    const auto local = std::chrono::sys_seconds(std::chrono::seconds(startedMs / 1000 + int64_t(offsetMinutes) * 60));
    const auto day = std::chrono::floor<std::chrono::days>(local);
    const std::chrono::hh_mm_ss time(local - day);
    wchar_t text[32];
    swprintf_s(text, L"%s %02d:%02d", dateText(Date(day)).c_str(), int(time.hours().count()), int(time.minutes().count()));
    return text;
}

hstring titleOf(const std::string& title, const std::string& name, const std::string& path) {
    if (!title.empty()) return ui::utf8(title);
    return hstring(std::filesystem::path(camel::toWide(name.empty() ? path : name)).stem().wstring());
}

// What a card's period ranks and lists, and the stretch of days, weeks or months its chart covers: today shows the
// last 7 days, this week and month their days, this year its months, and all time its months since the first play.
struct Range {
    std::optional<Date> from;
    Date chartFirst, chartLast;
    PlayBucketSize step = PlayBucketSize::Day;
};

Range rangeOf(std::wstring_view period) {
    const auto now = today();
    if (period == L"today") return {now, Date(std::chrono::sys_days(now) - std::chrono::days(6)), now, PlayBucketSize::Day};
    if (period == L"week") {
        const auto monday = mondayOf(now);
        return {monday, monday, Date(std::chrono::sys_days(monday) + std::chrono::days(6)), PlayBucketSize::Day};
    }
    if (period == L"month") {
        const Date first = now.year() / now.month() / std::chrono::day{1};
        return {first, first, Date(now.year() / now.month() / std::chrono::last), PlayBucketSize::Day};
    }
    const Date january = now.year() / std::chrono::January / std::chrono::day{1};
    if (period == L"year")
        return {january, january, now.year() / std::chrono::December / std::chrono::day{1}, PlayBucketSize::Month};
    return {std::nullopt, january, now.year() / now.month() / std::chrono::day{1}, PlayBucketSize::Month};
}

// A ranking's value: plays, or listening time; rows heard but never long enough to count as a play are left out.
hstring measured(bool byTime, int64_t count, int64_t ms) {
    return byTime ? listeningTime(ms) : plays(count);
}

bool counted(bool byTime, int64_t count, int64_t ms) {
    return byTime ? ms > 0 : count > 0;
}

ui::BrowseRow historyRow(const PlayHistoryEntry& h) {
    ui::BrowseRow row{h.event,
                      h.path,
                      titleOf(h.title, h.name, h.path),
                      ui::utf8(h.artist),
                      startedText(h.startedMs, h.utcOffsetMinutes),
                      hstring(ui::formatTime(double(h.heardMs) / 1000.0))};
    row.file = h.track.file;
    row.unavailable = !h.available;
    return row;
}

struct HomeData {
    PlayOverview today, week, month, year, all;
    std::optional<PlayRankedGroup> monthAlbum; // the month's most played album
    LibraryAlbumPage played, added;
    int64_t tracks = 0, artists = 0;
};

struct PeriodData {
    PlayStatistics ranked;
    std::vector<PlayBucket> buckets;
};

} // namespace

void HomeView::attach(std::shared_ptr<IServer> server, Handlers handlers) {
    server_ = std::move(server);
    handlers_ = std::move(handlers);
    using namespace std::chrono_literals;
    changeTimer_ = DispatcherQueue().CreateTimer();
    changeTimer_.Interval(3s); // the library pages' pace
    changeTimer_.Tick([this](auto&&, auto&&) { refreshIfChanged(); });
    changeTimer_.Start();
    MetricCombo().Items().Append(box_value(tr(L"Home_ByPlays")));
    MetricCombo().Items().Append(box_value(tr(L"Home_ByTime")));
    MetricCombo().SelectedIndex(0);
    SummaryMore().Text(tr(L"Home_ViewAll/Content"));
    showTiles();
}

void HomeView::show(bool listening) {
    if (listening == listening_) return;
    listening_ = listening;
    HomePart().Visibility(listening ? Visibility::Collapsed : Visibility::Visible);
    ListeningPart().Visibility(listening ? Visibility::Visible : Visibility::Collapsed);
    Scroller().ChangeView(nullptr, 0.0, nullptr, true);
}

void HomeView::OnMonthSummaryClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    if (handlers_.openListening) handlers_.openListening();
}

void HomeView::setActive(bool active) {
    if (!changeTimer_) return;
    if (active)
        changeTimer_.Start();
    else
        changeTimer_.Stop();
}

// A scan adds albums and tracks while Home shows; the album order's revision tells when to reread.
winrt::fire_and_forget HomeView::refreshIfChanged() {
    if (!server_ || probing_ || Visibility() != Visibility::Visible) co_return;
    probing_ = true;
    auto weak = get_weak();
    auto server = server_;
    auto queue = DispatcherQueue();
    co_await winrt::resume_background();
    std::optional<uint64_t> revision;
    try {
        revision = server->browseAlbums(AlbumSort::Added, std::nullopt, 0, 1, std::nullopt).revision;
    } catch (const std::exception&) {
    }
    queue.TryEnqueue([weak, revision] {
        auto self = weak.get();
        if (!self) return;
        self->probing_ = false;
        if (!revision || *revision == self->revision_) return;
        self->revision_ = *revision;
        self->refresh();
    });
}

winrt::fire_and_forget HomeView::refresh() {
    if (!server_) co_return;
    if (listening_) loadPeriod();
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
        const auto statistics = [&](std::optional<int64_t> from) {
            PlayStatisticsQuery query;
            query.period.fromMs = from;
            query.topLimit = 1;
            query.historyLimit = 0;
            return server->playStatistics(query);
        };
        const auto overview = [&](std::optional<int64_t> from) { return statistics(from).overview; };
        // Albums, tracks and artists each order the whole library first (a few hundred ms on a large one), so they run
        // on their own readers and Home waits for the slowest instead of the sum.
        auto added = std::async(std::launch::async,
                                [&] { return server->browseAlbums(AlbumSort::Added, std::nullopt, 0, kRowItems, std::nullopt); });
        auto tracks = std::async(std::launch::async, [&] {
            return server->browseTrackRows(TrackSort::Title, false, std::nullopt, 0, 1, std::nullopt).total;
        });
        auto artists = std::async(
            std::launch::async, [&] { return server->browseGroups(LibraryGroupField::Artist, false, 0, 1, std::nullopt).total; });
        data->today = overview(localStartMs(0));
        data->week = overview(localStartMs((now.wDayOfWeek + 6) % 7)); // weeks start on Monday
        const auto month = statistics(localStartMs(0, Start::Month));
        data->month = month.overview;
        if (!month.albums.empty()) data->monthAlbum = month.albums.front();
        data->year = overview(localStartMs(0, Start::Year));
        data->all = overview(std::nullopt);
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
        self->revision_ = data->added.revision;
        self->TodayTime().Text(listeningTime(data->today.listenedMs));
        self->TodayPlays().Text(plays(data->today.plays));
        self->WeekTime().Text(listeningTime(data->week.listenedMs));
        self->WeekPlays().Text(plays(data->week.plays));
        self->MonthTime().Text(listeningTime(data->month.listenedMs));
        self->MonthPlays().Text(plays(data->month.plays));
        self->YearTime().Text(listeningTime(data->year.listenedMs));
        self->YearPlays().Text(plays(data->year.plays));
        self->AllTime().Text(listeningTime(data->all.listenedMs));
        self->AllPlays().Text(plays(data->all.plays));
        self->showMonthSummary(data->month, data->monthAlbum);

        const auto albums = [](const LibraryAlbumPage& page) {
            std::vector<Windows::Foundation::IInspectable> out;
            out.reserve(page.albums.size());
            for (const auto& a : page.albums) out.push_back(item({a.id, {}, ui::utf8(a.title), ui::utf8(a.artist), {}, {}}));
            return out;
        };
        self->rows_[L"PlayedRow"] = albums(data->played);
        self->rows_[L"AddedRow"] = albums(data->added);
        for (const auto& row : {self->PlayedRow(), self->AddedRow()}) self->fill(row);
        self->PlayedEmpty().Visibility(data->played.albums.empty() ? Visibility::Visible : Visibility::Collapsed);

        hstring counts = formatted(L"Library_Albums", {data->added.total}) + L" · " +
                         formatted(L"Library_Tracks", {data->tracks}) + L" · " +
                         formatted(L"Search_ArtistCount", {data->artists});
        self->LibraryCounts().Text(data->tracks ? counts : tr(L"Library_Empty/Text"));
        if (camel::measure::measuring())
            camel::log::info("measure: home shown %.0f ms after process start", camel::measure::sinceProcessStart());
    });
}

void HomeView::showMonthSummary(const PlayOverview& month, const std::optional<PlayRankedGroup>& album) {
    const hstring totals = listeningTime(month.listenedMs) + L" · " + plays(month.plays);
    SummaryTotals().Text(totals);
    hstring line;
    if (album) {
        const hstring title = album->name.empty() ? tr(L"Library_Unknown") : ui::utf8(album->name);
        line = ui::format(L"Home_SummaryAlbum", {std::wstring(title), std::wstring(ui::utf8(album->artist))});
    }
    SummaryAlbum().Text(line);
    SummaryAlbum().Visibility(album ? Visibility::Visible : Visibility::Collapsed);
    SummaryCoverFrame().Visibility(album ? Visibility::Visible : Visibility::Collapsed);
    auto& token = covers_[winrt::get_abi(SummaryCover())];
    token.cancel();
    token = {};
    SummaryCover().Source(nullptr);
    if (album && album->album > 0) ui::loadArtwork(SummaryCover(), server_, {ArtworkKind::Album, album->album, {}}, token);
    Automation::AutomationProperties::SetName(MonthSummary(),
                                              tr(L"Home_Month/Text") + L", " + totals + (album ? L", " + line : hstring{}));
}

PlayMetric HomeView::metric() {
    return MetricCombo().SelectedIndex() == 1 ? PlayMetric::ListeningTime : PlayMetric::Plays;
}

winrt::fire_and_forget HomeView::loadPeriod() {
    if (!server_) co_return;
    const uint64_t generation = ++periodGeneration_;
    const auto range = rangeOf(period_);
    const auto byTime = metric() == PlayMetric::ListeningTime;
    auto weak = get_weak();
    auto server = server_;
    auto queue = DispatcherQueue();
    co_await winrt::resume_background();
    auto data = std::make_shared<PeriodData>();
    try {
        PlayStatisticsQuery ranking;
        if (range.from) ranking.period.fromMs = midnightMs(*range.from);
        ranking.metric = byTime ? PlayMetric::ListeningTime : PlayMetric::Plays;
        ranking.topLimit = kRowItems;
        ranking.historyLimit = kListRows;
        data->ranked = server->playStatistics(ranking);
        PlayStatisticsQuery chart;
        if (range.from) chart.period.fromMs = midnightMs(range.chartFirst);
        chart.bucket = range.step;
        chart.topLimit = 1;
        chart.historyLimit = 0;
        data->buckets = server->playStatistics(chart).buckets;
    } catch (const std::exception& e) {
        camel::log::warn("home: %s", e.what());
        co_return;
    }
    queue.TryEnqueue([weak, generation, data, range, byTime] {
        auto self = weak.get();
        if (!self || generation != self->periodGeneration_) return;
        const auto& stats = data->ranked;
        const auto value = [&](int64_t count, int64_t ms) { return measured(byTime, count, ms); };
        const auto heard = [&](int64_t count, int64_t ms) { return counted(byTime, count, ms); };

        self->chart_ = {range.chartFirst, range.chartLast, range.step, data->buckets};
        if (!range.from && !data->buckets.empty()) self->chart_.first = parseDate(data->buckets.front().start);
        self->drawChart();
        const bool empty = stats.overview.plays == 0 && stats.overview.listenedMs == 0;
        self->PeriodEmpty().Visibility(empty ? Visibility::Visible : Visibility::Collapsed);
        self->PeriodLists().Visibility(empty ? Visibility::Collapsed : Visibility::Visible);

        std::vector<Windows::Foundation::IInspectable> albums;
        for (const auto& a : stats.albums)
            if (heard(a.plays, a.listenedMs))
                albums.push_back(
                    item({a.album,
                          {},
                          ui::utf8(a.name),
                          hstring(std::wstring(ui::utf8(a.artist)) + L" · " + std::wstring(value(a.plays, a.listenedMs))),
                          {},
                          {}}));
        self->rows_[L"TopAlbumsRow"] = std::move(albums);
        self->fill(self->TopAlbumsRow());

        auto artists = single_threaded_observable_vector<Windows::Foundation::IInspectable>();
        for (size_t i = 0; i < stats.artists.size() && artists.Size() < kListRows; ++i) {
            const auto& a = stats.artists[i];
            if (heard(a.plays, a.listenedMs))
                artists.Append(item({0, a.name, ui::utf8(a.name), value(a.plays, a.listenedMs), {}, {}}));
        }
        self->TopArtistsRow().ItemsSource(artists);

        auto tracks = single_threaded_observable_vector<Windows::Foundation::IInspectable>();
        for (size_t i = 0; i < stats.tracks.size() && tracks.Size() < kListRows; ++i) {
            const auto& t = stats.tracks[i];
            if (!heard(t.plays, t.listenedMs)) continue;
            ui::BrowseRow row{t.track.file,
                              t.path,
                              titleOf(t.title, t.name, t.path),
                              ui::utf8(t.artist),
                              to_hstring(i + 1),
                              value(t.plays, t.listenedMs)};
            row.file = t.track.file;
            row.unavailable = !t.available;
            tracks.Append(item(row));
        }
        self->TopTracksList().ItemsSource(tracks);

        auto history = single_threaded_observable_vector<Windows::Foundation::IInspectable>();
        for (const auto& h : stats.history) history.Append(item(historyRow(h)));
        self->HistoryList().ItemsSource(history);
        self->HistoryHeader().Text(ui::format(L"Home_History", {std::to_wstring(stats.historyTotal)}));
    });
}

// The chosen card is outlined in the accent color; padding makes up for its wider border so its text stays put.
void HomeView::showTiles() {
    const auto accent = Application::Current().Resources().Lookup(box_value(L"AccentFillColorDefaultBrush")).as<Media::Brush>();
    for (const auto& child : Tiles().Children()) {
        const auto tile = child.as<Border>();
        const bool chosen = unbox_value_or<hstring>(tile.Tag(), L"") == period_;
        if (chosen) {
            tile.BorderBrush(accent);
        } else {
            tile.ClearValue(Border::BorderBrushProperty());
        }
        tile.BorderThickness(ThicknessHelper::FromUniformLength(chosen ? 2 : 1));
        tile.Padding(chosen ? Thickness{15, 11, 15, 11} : Thickness{16, 12, 16, 12});
    }
}

// Listening time per day, week or month; empty days, weeks or months in between show as gaps on the baseline.
void HomeView::drawChart() {
    Chart().Children().Clear();
    ChartAxis().Children().Clear();
    if (chart_.buckets.empty()) return;
    const auto step = chart_.step;
    const auto next = [&](Date d) {
        if (step == PlayBucketSize::Day) return Date(std::chrono::sys_days(d) + std::chrono::days(1));
        if (step == PlayBucketSize::Week) return Date(std::chrono::sys_days(d) + std::chrono::days(7));
        return Date(d + std::chrono::months(1));
    };
    std::vector<std::pair<Date, const PlayBucket*>> bars;
    size_t at = 0;
    for (auto d = chart_.first; std::chrono::sys_days(d) <= std::chrono::sys_days(chart_.last) && bars.size() < 2000;
         d = next(d)) {
        while (at < chart_.buckets.size() &&
               std::chrono::sys_days(parseDate(chart_.buckets[at].start)) < std::chrono::sys_days(d))
            ++at;
        const PlayBucket* bucket = nullptr;
        if (at < chart_.buckets.size() && parseDate(chart_.buckets[at].start) == d) bucket = &chart_.buckets[at++];
        bars.emplace_back(d, bucket);
    }
    if (bars.empty()) return;
    int64_t most = 1;
    for (const auto& b : chart_.buckets) most = std::max(most, b.listenedMs);
    const double width = Chart().ActualWidth(), height = Chart().ActualHeight();
    if (width <= 0 || height <= 0) return;
    const auto resource = [](const wchar_t* key) { return Application::Current().Resources().Lookup(box_value(key)); };
    const auto accent = resource(L"AccentFillColorDefaultBrush").as<Media::Brush>();
    const auto secondary = resource(L"TextFillColorSecondaryBrush").as<Media::Brush>();
    const auto divider = resource(L"DividerStrokeColorDefaultBrush").as<Media::Brush>();
    const auto text = [&](std::wstring_view value) {
        TextBlock block;
        // The caption size, with a fixed line height: a block not yet shown measures its width but not its height.
        block.FontSize(12);
        block.LineHeight(16);
        block.LineStackingStrategy(LineStackingStrategy::BlockLineHeight);
        block.Foreground(secondary);
        block.Text(value);
        block.Measure({std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()});
        return block;
    };
    const auto line = [&](double y) {
        Shapes::Line rule;
        rule.X2(width);
        rule.Y1(y);
        rule.Y2(y);
        rule.Stroke(divider);
        rule.StrokeThickness(1);
        Chart().Children().Append(rule);
    };

    // The longest bucket's time labels a rule at the top; bars scale to the space below it.
    Chart().Children().Append(text(listeningTime(most)));
    const double top = 20;
    line(top);
    line(height - 0.5);

    // A few bars stay at their full width from the left instead of spreading across the chart.
    const double slot = std::min(width / double(bars.size()), 64.0);
    const bool years = bars.front().first.year() != bars.back().first.year();
    const auto label = [&](Date d) {
        wchar_t value[16];
        if (step == PlayBucketSize::Month)
            swprintf_s(value, L"%04d-%02u", int(d.year()), unsigned(d.month()));
        else if (years)
            return dateText(d);
        else
            swprintf_s(value, L"%02u-%02u", unsigned(d.month()), unsigned(d.day()));
        return std::wstring(value);
    };
    // Every bar's date when they fit, otherwise every few: by week, fortnight or quarter for days and weeks.
    const double room = text(label(bars.back().first)).DesiredSize().Width + 12;
    size_t every = 1;
    for (const size_t candidate : step == PlayBucketSize::Month  ? std::vector<size_t>{1, 2, 3, 6, 12}
                                  : step == PlayBucketSize::Week ? std::vector<size_t>{1, 2, 4, 13, 26, 52}
                                                                 : std::vector<size_t>{1, 2, 7, 14, 28, 91, 182, 364}) {
        every = candidate;
        if (slot * double(candidate) >= room) break;
    }
    every = std::max(every, size_t(std::ceil(room / slot)));
    double ticksEnd = 0; // a date pushed in from the chart's edge can reach the next one, which then stays out
    for (size_t i = 0; i < bars.size(); ++i) {
        const auto* bucket = bars[i].second;
        const double center = slot * (double(i) + 0.5);
        Shapes::Rectangle bar;
        bar.Width(std::max(slot * 0.7, 1.0));
        bar.Height(bucket ? std::max(2.0, (height - top) * double(bucket->listenedMs) / double(most)) : 0.0);
        bar.Fill(accent);
        bar.RadiusX(2);
        bar.RadiusY(2);
        Canvas::SetLeft(bar, center - bar.Width() / 2);
        Canvas::SetTop(bar, height - bar.Height());
        const auto when = step == PlayBucketSize::Month ? label(bars[i].first) : dateText(bars[i].first);
        ToolTipService::SetToolTip(
            bar, box_value(hstring(when + L" · " + std::wstring(listeningTime(bucket ? bucket->listenedMs : 0)) + L" · " +
                                   std::wstring(plays(bucket ? bucket->plays : 0)))));
        Chart().Children().Append(bar);
        if (i % every != 0) continue;
        const auto tick = text(label(bars[i].first));
        const double tickWidth = tick.DesiredSize().Width;
        const double left = std::clamp(center - tickWidth / 2, 0.0, std::max(0.0, width - tickWidth));
        if (left < ticksEnd) continue;
        Canvas::SetLeft(tick, left);
        ChartAxis().Children().Append(tick);
        ticksEnd = left + tickWidth + 8;
    }
}

void HomeView::OnChartSizeChanged(Windows::Foundation::IInspectable const&, SizeChangedEventArgs const&) {
    drawChart();
}

void HomeView::fill(ItemsControl const& row) {
    const auto found = rows_.find(std::wstring(row.Name()));
    if (found == rows_.end()) return;
    const auto fit = ui::fitCovers(row.ActualWidth() + 4, kCoverExtra).columns;
    auto shown = single_threaded_observable_vector<Windows::Foundation::IInspectable>();
    for (size_t i = 0; i < found->second.size() && i < fit; ++i) shown.Append(found->second[i]);
    row.ItemsSource(shown);
}

// The last tile's container margin may hang past the row, so it does not count against the width.
void HomeView::OnRowSizeChanged(Windows::Foundation::IInspectable const& sender, SizeChangedEventArgs const& e) {
    if (e.NewSize().Width <= 0) return; // a row of the page that is hidden
    const auto fit = ui::fitCovers(e.NewSize().Width + 4, kCoverExtra);
    Resources().Lookup(box_value(L"HomeCards")).as<CamelPlayer::CardSize>().Cover(fit.cover);
    for (const auto& row : {PlayedRow(), AddedRow(), TopAlbumsRow()}) row.Height(fit.cover + kCoverRowExtra);
    if (ui::fitCovers(e.PreviousSize().Width + 4, kCoverExtra).columns != fit.columns) fill(sender.as<ItemsControl>());
}

void HomeView::OnAlbumClick(Windows::Foundation::IInspectable const&, ItemClickEventArgs const& e) {
    const auto& row = get_self<BrowseItem>(e.ClickedItem().as<CamelPlayer::BrowseItem>())->row();
    if (row.id > 0 && handlers_.openAlbum) handlers_.openAlbum(row.id, row.title);
}

void HomeView::OnArtistClick(Windows::Foundation::IInspectable const&, ItemClickEventArgs const& e) {
    const auto& row = get_self<BrowseItem>(e.ClickedItem().as<CamelPlayer::BrowseItem>())->row();
    if (!row.value.empty() && handlers_.openArtist) handlers_.openArtist(ui::utf8(row.value));
}

void HomeView::OnArtistContextRequested(UIElement const& sender, Input::ContextRequestedEventArgs const& e) {
    const auto element = sender.as<FrameworkElement>();
    const auto item = element.DataContext().try_as<CamelPlayer::BrowseItem>();
    if (!item || !handlers_.submit) return;
    menuArtist_ = get_self<BrowseItem>(item)->row().value;
    if (menuArtist_.empty()) return;
    if (!artistMenu_) {
        artistMenu_ = MenuFlyout();
        ui::appendArtistFavoriteMenu(artistMenu_, [this](bool favorite) {
            handlers_.submit(ui::artistFavoriteCommand({{LibraryGroupField::Artist, menuArtist_}}, favorite));
        });
        artistMenu_.Items().RemoveAt(0); // the separator, with nothing above it
    }
    Primitives::FlyoutShowOptions options;
    if (Windows::Foundation::Point point; e.TryGetPosition(element, point)) options.Position(point);
    artistMenu_.ShowAt(element, options);
    e.Handled(true);
}

void HomeView::OnTrackClick(Windows::Foundation::IInspectable const&, ItemClickEventArgs const& e) {
    const auto& row = get_self<BrowseItem>(e.ClickedItem().as<CamelPlayer::BrowseItem>())->row();
    if (row.value.empty() || row.unavailable || !handlers_.submit) return;
    Command command;
    command.kind = CommandKind::QueueReplace;
    command.sources.emplace_back(row.value);
    handlers_.submit(std::move(command));
}

ui::BrowseFetch HomeView::periodFetch(std::wstring_view list) {
    PlayPeriod period;
    if (const auto range = rangeOf(period_); range.from) period.fromMs = midnightMs(*range.from);
    auto server = server_;
    if (list == L"history")
        return [server, period](int64_t offset, int64_t limit, std::optional<uint64_t>) {
            PlayStatisticsQuery query;
            query.period = period;
            query.topLimit = 1; // the smallest the Server accepts
            query.historyOffset = offset;
            query.historyLimit = limit;
            const auto stats = server->playStatistics(query);
            ui::BrowsePage out{0, stats.historyTotal, {}};
            for (const auto& h : stats.history) out.rows.push_back(historyRow(h));
            return out;
        };
    // Rankings come whole from the Server, up to the 500 it allows, and are paged here.
    const bool byTime = metric() == PlayMetric::ListeningTime;
    return [server, period, byTime, list = std::wstring(list)](int64_t offset, int64_t limit, std::optional<uint64_t>) {
        PlayStatisticsQuery query;
        query.period = period;
        query.metric = byTime ? PlayMetric::ListeningTime : PlayMetric::Plays;
        query.topLimit = std::clamp<int64_t>(offset + limit, 1, 500);
        query.historyLimit = 0;
        const auto stats = server->playStatistics(query);
        std::vector<ui::BrowseRow> rows;
        if (list == L"topAlbums") {
            for (const auto& a : stats.albums)
                if (counted(byTime, a.plays, a.listenedMs))
                    rows.push_back(
                        {a.album, {}, ui::utf8(a.name), ui::utf8(a.artist), {}, measured(byTime, a.plays, a.listenedMs)});
        } else if (list == L"topArtists") {
            for (const auto& a : stats.artists)
                if (counted(byTime, a.plays, a.listenedMs))
                    rows.push_back({0, a.name, ui::utf8(a.name), {}, {}, measured(byTime, a.plays, a.listenedMs)});
        } else {
            for (const auto& t : stats.tracks) {
                if (!counted(byTime, t.plays, t.listenedMs)) continue;
                rows.push_back({t.track.file, t.path, titleOf(t.title, t.name, t.path), ui::utf8(t.artist), ui::utf8(t.album),
                                measured(byTime, t.plays, t.listenedMs)});
                rows.back().file = t.track.file;
                rows.back().unavailable = !t.available;
            }
        }
        ui::BrowsePage out{0, int64_t(rows.size()), {}};
        for (auto i = size_t(offset); i < rows.size() && std::cmp_less(i, offset + limit); ++i) out.rows.push_back(rows[i]);
        return out;
    };
}

void HomeView::OnViewAllClick(Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&) {
    if (!handlers_.viewAll) return;
    const auto list = unbox_value_or<hstring>(sender.as<FrameworkElement>().Tag(), L"");
    if (list == L"played" || list == L"added") {
        handlers_.viewAll(list, {}, {});
        return;
    }
    // Titled by the list and the chosen card's period, as "Most played albums · This month".
    hstring period;
    for (const auto& child : Tiles().Children())
        if (const auto tile = child.as<Border>(); unbox_value_or<hstring>(tile.Tag(), L"") == period_)
            period = tile.Child().as<StackPanel>().Children().GetAt(0).as<TextBlock>().Text();
    const hstring heading = list == L"topAlbums"    ? tr(L"Home_TopAlbums/Text")
                            : list == L"topArtists" ? tr(L"Home_TopArtists/Text")
                            : list == L"topTracks"  ? tr(L"Home_TopTracks/Text")
                                                    : tr(L"Home_HistoryTitle");
    handlers_.viewAll(list, heading + L" · " + period, periodFetch(list));
}

void HomeView::OnStatTileTapped(Windows::Foundation::IInspectable const& sender, Input::TappedRoutedEventArgs const&) {
    period_ = unbox_value_or<hstring>(sender.as<FrameworkElement>().Tag(), L"month");
    showTiles();
    loadPeriod();
}

void HomeView::OnMetricChanged(Windows::Foundation::IInspectable const&, SelectionChangedEventArgs const&) {
    if (server_) loadPeriod();
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
