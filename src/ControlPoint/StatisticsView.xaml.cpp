// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pch.h"

#include "StatisticsView.xaml.h"
#if __has_include("StatisticsView.g.cpp")
#include "StatisticsView.g.cpp"
#endif

#include <chrono>
#include <filesystem>

#include <winrt/Microsoft.UI.Xaml.Shapes.h>

#include "BrowseItem.h"
#include "Common/util/Log.h"
#include "Common/util/Text.h"
#include "UiText.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace camel::server;
using ui::tr;

namespace winrt::CamelPlayer::implementation {
namespace {

enum class Period : int32_t { Last7Days, Last30Days, ThisYear, AllTime, Custom };
enum class Ranking : int32_t { Tracks, Albums, Artists };

// Local midnight of a date, as UTC milliseconds.
int64_t localMidnightMs(int year, int month, int day) {
    SYSTEMTIME local{};
    local.wYear = WORD(year);
    local.wMonth = WORD(month);
    local.wDay = WORD(day);
    SYSTEMTIME utc{};
    TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc);
    FILETIME file{};
    SystemTimeToFileTime(&utc, &file);
    const ULARGE_INTEGER ticks{{file.dwLowDateTime, file.dwHighDateTime}};
    return int64_t((ticks.QuadPart - 116'444'736'000'000'000ULL) / 10'000);
}

std::chrono::year_month_day today() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    return std::chrono::year{now.wYear} / std::chrono::month{now.wMonth} / std::chrono::day{now.wDay};
}

int64_t midnightMs(std::chrono::year_month_day date) {
    return localMidnightMs(int(date.year()), int(unsigned(date.month())), int(unsigned(date.day())));
}

std::chrono::year_month_day dateOf(Windows::Foundation::DateTime value) {
    const uint64_t ticks = clock::to_file_time(value).value;
    FILETIME utc{DWORD(ticks), DWORD(ticks >> 32)}, local{};
    FileTimeToLocalFileTime(&utc, &local);
    SYSTEMTIME day{};
    FileTimeToSystemTime(&local, &day);
    return std::chrono::year{day.wYear} / std::chrono::month{day.wMonth} / std::chrono::day{day.wDay};
}

std::chrono::year_month_day dateOfMs(int64_t ms) {
    return dateOf(clock::from_file_time(file_time{uint64_t(ms) * 10'000 + 116'444'736'000'000'000ULL}));
}

Windows::Foundation::DateTime dateTimeOf(std::chrono::year_month_day date) {
    const int64_t ms = midnightMs(date);
    return clock::from_file_time(file_time{uint64_t(ms) * 10'000 + 116'444'736'000'000'000ULL});
}

std::chrono::year_month_day parseDate(const std::string& text) {
    int y = 0, m = 0, d = 0;
    sscanf_s(text.c_str(), "%d-%d-%d", &y, &m, &d);
    return std::chrono::year{y} / std::chrono::month{unsigned(m)} / std::chrono::day{unsigned(d)};
}

std::wstring dateText(std::chrono::year_month_day date) {
    wchar_t text[16];
    swprintf_s(text, L"%04d-%02u-%02u", int(date.year()), unsigned(date.month()), unsigned(date.day()));
    return text;
}

hstring listeningTime(int64_t ms) {
    const int64_t minutes = ms / 60'000;
    if (minutes < 60) return ui::format(L"Home_Minutes", {std::to_wstring(minutes)});
    return ui::format(L"Home_HoursMinutes", {std::to_wstring(minutes / 60), std::to_wstring(minutes % 60)});
}

hstring plays(int64_t n) {
    return ui::format(L"Home_Plays", {std::to_wstring(n)});
}

hstring titleOf(const std::string& title, const std::string& name, const std::string& path) {
    if (!title.empty()) return ui::utf8(title);
    return hstring(std::filesystem::path(camel::toWide(name.empty() ? path : name)).stem().wstring());
}

// A play's local start time, as the recorded UTC offset gave it.
hstring startedText(int64_t startedMs, int32_t offsetMinutes) {
    const auto local = std::chrono::sys_seconds(std::chrono::seconds(startedMs / 1000 + int64_t(offsetMinutes) * 60));
    const auto day = std::chrono::floor<std::chrono::days>(local);
    const std::chrono::hh_mm_ss time(local - day);
    wchar_t text[32];
    swprintf_s(text, L"%s %02d:%02d", dateText(std::chrono::year_month_day(day)).c_str(), int(time.hours().count()),
               int(time.minutes().count()));
    return text;
}

Windows::Foundation::IInspectable item(const ui::BrowseRow& row) {
    auto result = make<BrowseItem>();
    get_self<BrowseItem>(result)->fill(row);
    return result;
}

void fill(ComboBox const& combo, std::initializer_list<const wchar_t*> keys, int32_t selected) {
    combo.Items().Clear();
    for (const auto* key : keys) combo.Items().Append(box_value(tr(key)));
    combo.SelectedIndex(selected);
}

} // namespace

void StatisticsView::attach(std::shared_ptr<IServer> server, Handlers handlers) {
    server_ = std::move(server);
    handlers_ = std::move(handlers);
    populating_ = true;
    fill(PeriodCombo(), {L"Stats_Last7Days", L"Stats_Last30Days", L"Stats_ThisYear", L"Stats_AllTime", L"Stats_Custom"},
         int32_t(Period::Last30Days));
    fill(MetricCombo(), {L"Stats_ByPlays", L"Stats_ByTime"}, 0);
    fill(BucketCombo(), {L"Stats_ByDay", L"Stats_ByWeek", L"Stats_ByMonth"}, 0);
    fill(RankCombo(), {L"Stats_TopTracks", L"Stats_TopAlbums", L"Stats_TopArtists"}, 0);
    const auto now = today();
    ToPicker().Date(dateTimeOf(now));
    FromPicker().Date(dateTimeOf(std::chrono::sys_days(now) - std::chrono::days(29)));
    populating_ = false;
}

void StatisticsView::show(Range range) {
    populating_ = true;
    const auto now = today();
    const auto custom = [&](std::chrono::year_month_day from) {
        FromPicker().Date(dateTimeOf(from));
        ToPicker().Date(dateTimeOf(now));
        PeriodCombo().SelectedIndex(int32_t(Period::Custom));
    };
    switch (range) {
    case Range::Today:
        custom(now);
        break;
    case Range::ThisWeek: { // weeks start on Monday, as on Home
        const std::chrono::weekday weekday{std::chrono::sys_days(now)};
        custom(std::chrono::sys_days(now) - std::chrono::days((weekday.c_encoding() + 6) % 7));
        break;
    }
    case Range::ThisMonth:
        custom(now.year() / now.month() / std::chrono::day{1});
        break;
    case Range::ThisYear:
        PeriodCombo().SelectedIndex(int32_t(Period::ThisYear));
        break;
    case Range::AllTime:
        PeriodCombo().SelectedIndex(int32_t(Period::AllTime));
        break;
    case Range::Last7Days:
        PeriodCombo().SelectedIndex(int32_t(Period::Last7Days));
        break;
    case Range::Last30Days:
        PeriodCombo().SelectedIndex(int32_t(Period::Last30Days));
        break;
    }
    BucketCombo().SelectedIndex(range == Range::AllTime ? 2 : range == Range::ThisYear ? 1 : 0);
    populating_ = false;
    load();
}

void StatisticsView::hide() {
    ++generation_;
    HistoryList().ItemsSource(nullptr);
}

PlayPeriod StatisticsView::period() {
    const auto now = today();
    PlayPeriod out;
    switch (Period(PeriodCombo().SelectedIndex())) {
    case Period::Last7Days:
        out.fromMs = midnightMs(std::chrono::sys_days(now) - std::chrono::days(6));
        break;
    case Period::Last30Days:
        out.fromMs = midnightMs(std::chrono::sys_days(now) - std::chrono::days(29));
        break;
    case Period::ThisYear:
        out.fromMs = midnightMs(now.year() / std::chrono::January / std::chrono::day{1});
        break;
    case Period::AllTime:
        break;
    case Period::Custom: {
        auto from = FromPicker().Date() ? dateOf(FromPicker().Date().Value()) : now;
        auto to = ToPicker().Date() ? dateOf(ToPicker().Date().Value()) : now;
        if (std::chrono::sys_days(to) < std::chrono::sys_days(from)) std::swap(from, to);
        out.fromMs = midnightMs(from);
        out.toMs = midnightMs(std::chrono::sys_days(to) + std::chrono::days(1)); // the whole last day
        break;
    }
    }
    return out;
}

void StatisticsView::load() {
    if (!server_) return;
    const bool custom = PeriodCombo().SelectedIndex() == int32_t(Period::Custom);
    FromPicker().Visibility(custom ? Visibility::Visible : Visibility::Collapsed);
    ToPicker().Visibility(custom ? Visibility::Visible : Visibility::Collapsed);
    ++generation_;
    PlayStatisticsQuery query;
    query.period = period();
    query.metric = MetricCombo().SelectedIndex() == 1 ? PlayMetric::ListeningTime : PlayMetric::Plays;
    query.bucket = PlayBucketSize(std::clamp(BucketCombo().SelectedIndex(), 0, 2));
    // The lists grow with the page instead of scrolling on their own, so they show only the first rows.
    query.topLimit = 10;
    query.historyLimit = 10;
    background([this, server = server_, query] {
        auto stats = std::make_shared<PlayStatistics>(server->playStatistics(query));
        return std::function<void()>([this, stats] {
            stats_ = std::move(*stats);
            TimeValue().Text(listeningTime(stats_.overview.listenedMs));
            PlaysValue().Text(to_hstring(stats_.overview.plays));
            TracksValue().Text(to_hstring(stats_.overview.tracks));
            AlbumsValue().Text(to_hstring(stats_.overview.albums));
            ArtistsValue().Text(to_hstring(stats_.overview.artists));
            showRanking();
            showHistory();
            drawChart();
        });
    });
}

void StatisticsView::showRanking() {
    const bool byTime = MetricCombo().SelectedIndex() == 1;
    const auto value = [&](int64_t count, int64_t ms) { return byTime ? listeningTime(ms) : plays(count); };
    auto items = single_threaded_observable_vector<Windows::Foundation::IInspectable>();
    int64_t rank = 0;
    switch (Ranking(RankCombo().SelectedIndex())) {
    case Ranking::Tracks:
        for (const auto& t : stats_.tracks) {
            ui::BrowseRow row{t.track.file,
                              t.path,
                              titleOf(t.title, t.name, t.path),
                              ui::utf8(t.artist),
                              to_hstring(++rank),
                              value(t.plays, t.listenedMs)};
            row.file = t.track.file;
            row.unavailable = !t.available;
            items.Append(item(row));
        }
        break;
    case Ranking::Albums:
        for (const auto& a : stats_.albums)
            items.Append(
                item({a.album, {}, ui::utf8(a.name), ui::utf8(a.artist), to_hstring(++rank), value(a.plays, a.listenedMs)}));
        break;
    case Ranking::Artists:
        for (const auto& a : stats_.artists)
            items.Append(item({0, a.name, ui::utf8(a.name), {}, to_hstring(++rank), value(a.plays, a.listenedMs)}));
        break;
    }
    RankList().ItemsSource(items);
}

void StatisticsView::showHistory() {
    auto items = single_threaded_observable_vector<Windows::Foundation::IInspectable>();
    for (const auto& h : stats_.history) {
        ui::BrowseRow row{h.event,
                          h.path,
                          titleOf(h.title, h.name, h.path),
                          ui::utf8(h.artist),
                          startedText(h.startedMs, h.utcOffsetMinutes),
                          hstring(ui::formatTime(double(h.heardMs) / 1000.0))};
        row.file = h.track.file;
        row.unavailable = !h.available;
        items.Append(item(row));
    }
    HistoryList().ItemsSource(items);
    HistoryHeader().Text(ui::format(L"Stats_History", {std::to_wstring(stats_.historyTotal)}));
}

// Listening time per day, week or month; empty days, weeks or months in between show as gaps on the baseline.
void StatisticsView::drawChart() {
    Chart().Children().Clear();
    ChartFirst().Text(L"");
    ChartLast().Text(L"");
    if (stats_.buckets.empty()) return;
    const auto step = PlayBucketSize(std::clamp(BucketCombo().SelectedIndex(), 0, 2));
    const auto next = [&](std::chrono::year_month_day d) {
        if (step == PlayBucketSize::Day) return std::chrono::year_month_day(std::chrono::sys_days(d) + std::chrono::days(1));
        if (step == PlayBucketSize::Week) return std::chrono::year_month_day(std::chrono::sys_days(d) + std::chrono::days(7));
        return std::chrono::year_month_day(d + std::chrono::months(1));
    };
    const auto align = [&](std::chrono::year_month_day d) {
        if (step == PlayBucketSize::Day) return d;
        if (step == PlayBucketSize::Week) {
            const std::chrono::weekday weekday{std::chrono::sys_days(d)};
            return std::chrono::year_month_day(std::chrono::sys_days(d) - std::chrono::days((weekday.c_encoding() + 6) % 7));
        }
        return d.year() / d.month() / std::chrono::day{1};
    };
    // The whole period, so a single day of plays does not fill the chart; all time starts at the first play.
    const auto shown = period();
    const auto first = align(shown.fromMs ? dateOfMs(*shown.fromMs) : parseDate(stats_.buckets.front().start));
    const auto last = align(shown.toMs ? dateOfMs(*shown.toMs - 1) : today());
    std::vector<std::pair<std::chrono::year_month_day, const PlayBucket*>> bars;
    size_t at = 0;
    for (auto d = first; std::chrono::sys_days(d) <= std::chrono::sys_days(last) && bars.size() < 2000; d = next(d)) {
        while (at < stats_.buckets.size() &&
               std::chrono::sys_days(parseDate(stats_.buckets[at].start)) < std::chrono::sys_days(d))
            ++at;
        const PlayBucket* bucket = nullptr;
        if (at < stats_.buckets.size() && parseDate(stats_.buckets[at].start) == d) bucket = &stats_.buckets[at++];
        bars.emplace_back(d, bucket);
    }
    int64_t most = 1;
    for (const auto& b : stats_.buckets) most = std::max(most, b.listenedMs);
    const double width = Chart().ActualWidth(), height = Chart().ActualHeight();
    if (width <= 0 || height <= 0) return;
    const double slot = width / double(bars.size());
    const auto brush = Application::Current().Resources().Lookup(box_value(L"AccentFillColorDefaultBrush")).as<Media::Brush>();
    for (size_t i = 0; i < bars.size(); ++i) {
        const auto* bucket = bars[i].second;
        Shapes::Rectangle bar;
        bar.Width(std::clamp(slot * 0.7, 1.0, 48.0));
        bar.Height(bucket ? std::max(2.0, height * double(bucket->listenedMs) / double(most)) : 0.0);
        bar.Fill(brush);
        bar.RadiusX(2);
        bar.RadiusY(2);
        Canvas::SetLeft(bar, slot * double(i) + (slot - bar.Width()) / 2);
        Canvas::SetTop(bar, height - bar.Height());
        ToolTipService::SetToolTip(bar, box_value(hstring(dateText(bars[i].first) + L" · " +
                                                          std::wstring(listeningTime(bucket ? bucket->listenedMs : 0)) + L" · " +
                                                          std::wstring(plays(bucket ? bucket->plays : 0)))));
        Chart().Children().Append(bar);
    }
    ChartFirst().Text(dateText(bars.front().first));
    ChartLast().Text(dateText(bars.back().first));
}

void StatisticsView::OnPeriodChanged(Windows::Foundation::IInspectable const&, SelectionChangedEventArgs const&) {
    if (!populating_) load();
}

void StatisticsView::OnRangeChanged(CalendarDatePicker const&, CalendarDatePickerDateChangedEventArgs const&) {
    if (!populating_) load();
}

void StatisticsView::OnRankChanged(Windows::Foundation::IInspectable const&, SelectionChangedEventArgs const&) {
    if (!populating_) showRanking();
}

void StatisticsView::OnChartSizeChanged(Windows::Foundation::IInspectable const&, SizeChangedEventArgs const&) {
    drawChart();
}

// Tracks play again; albums and artists open their pages.
void StatisticsView::OnRowDoubleTapped(Windows::Foundation::IInspectable const& sender,
                                       Input::DoubleTappedRoutedEventArgs const& e) {
    const auto element = e.OriginalSource().try_as<FrameworkElement>();
    const auto value = element ? element.DataContext().try_as<CamelPlayer::BrowseItem>() : nullptr;
    if (!value) return;
    const auto& row = get_self<BrowseItem>(value)->row();
    const bool ranking = sender == RankList();
    if (ranking && RankCombo().SelectedIndex() == int32_t(Ranking::Albums)) {
        if (handlers_.openAlbum && row.id) handlers_.openAlbum(row.id, row.title);
    } else if (ranking && RankCombo().SelectedIndex() == int32_t(Ranking::Artists)) {
        if (handlers_.openArtist) handlers_.openArtist(row.title);
    } else if (!row.value.empty() && !row.unavailable && handlers_.submit) {
        Command command;
        command.kind = CommandKind::QueueReplace;
        command.sources.emplace_back(row.value);
        handlers_.submit(std::move(command));
    }
}

void StatisticsView::OnRowContextRequested(UIElement const& sender, Input::ContextRequestedEventArgs const& e) {
    const auto element = sender.as<FrameworkElement>();
    menuItem_ = element.DataContext();
    const auto value = menuItem_.try_as<CamelPlayer::BrowseItem>();
    if (!value || !get_self<BrowseItem>(value)->row().file) return; // only tracks have statistics to reset
    // Which list the row belongs to: its container is in exactly one of them.
    menuList_ = RankList().ContainerFromItem(menuItem_) ? RankList() : HistoryList();
    if (!rowMenu_) {
        rowMenu_ = MenuFlyout();
        MenuFlyoutItem reset;
        reset.Text(tr(L"Stats_ResetSelected"));
        FontIcon icon;
        icon.Glyph(L"\xE74D");
        reset.Icon(icon);
        reset.Click([this](auto&&, auto&&) { resetSelected(menuList_, menuItem_); });
        rowMenu_.Items().Append(reset);
    }
    Primitives::FlyoutShowOptions options;
    if (Windows::Foundation::Point point; e.TryGetPosition(element, point)) options.Position(point);
    rowMenu_.ShowAt(element, options);
    e.Handled(true);
}

// The selected tracks, or the right-clicked one when it is not selected; deleting their plays is not undoable.
winrt::fire_and_forget StatisticsView::resetSelected(ListView list, Windows::Foundation::IInspectable clicked) {
    auto strong = get_strong();
    std::vector<LibraryTrackKey> tracks;
    bool clickedSelected = false;
    for (const auto& selected : list.SelectedItems()) {
        const auto& row = get_self<BrowseItem>(selected.as<CamelPlayer::BrowseItem>())->row();
        if (row.file && std::ranges::find(tracks, LibraryTrackKey{row.file}) == tracks.end()) tracks.push_back({row.file});
        clickedSelected = clickedSelected || selected == clicked;
    }
    if (!clickedSelected) tracks = {{get_self<BrowseItem>(clicked.as<CamelPlayer::BrowseItem>())->row().file}};
    ContentDialog dialog;
    dialog.XamlRoot(XamlRoot());
    dialog.Title(box_value(tr(L"Stats_ResetSelectedTitle")));
    dialog.Content(box_value(ui::format(L"Stats_ResetSelectedText", {std::to_wstring(tracks.size())})));
    dialog.PrimaryButtonText(tr(L"Stats_ResetPrimary"));
    dialog.CloseButtonText(tr(L"Playlist_Cancel"));
    dialog.DefaultButton(ContentDialogButton::Close);
    if (co_await dialog.ShowAsync() != ContentDialogResult::Primary || !handlers_.submit) co_return;
    Command command;
    command.kind = CommandKind::ResetPlayStatistics;
    command.tracks = std::move(tracks);
    handlers_.submit(std::move(command));
    reloadWhenIdle();
}

winrt::fire_and_forget StatisticsView::OnResetAllClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    auto strong = get_strong();
    ContentDialog dialog;
    dialog.XamlRoot(XamlRoot());
    dialog.Title(box_value(tr(L"Stats_ResetAllTitle")));
    dialog.Content(box_value(tr(L"Stats_ResetAllText")));
    dialog.PrimaryButtonText(tr(L"Stats_ResetPrimary"));
    dialog.CloseButtonText(tr(L"Playlist_Cancel"));
    dialog.DefaultButton(ContentDialogButton::Close);
    if (co_await dialog.ShowAsync() != ContentDialogResult::Primary || !handlers_.submit) co_return;
    Command command;
    command.kind = CommandKind::ResetAllPlayStatistics;
    handlers_.submit(std::move(command));
    reloadWhenIdle();
}

// Reloads once the Server applied the submitted commands.
void StatisticsView::reloadWhenIdle() {
    background([this, server = server_] {
        server->waitIdle();
        return std::function<void()>([this] { load(); });
    });
}

winrt::fire_and_forget StatisticsView::background(std::function<std::function<void()>()> work) {
    auto weak = get_weak();
    auto queue = DispatcherQueue();
    const auto generation = generation_;
    co_await winrt::resume_background();
    std::function<void()> apply;
    try {
        apply = work();
    } catch (const std::exception& e) {
        camel::log::warn("statistics view: %s", e.what());
        co_return;
    }
    queue.TryEnqueue([weak, generation, apply] {
        auto self = weak.get();
        if (self && self->generation_ == generation && apply) apply();
    });
}

} // namespace winrt::CamelPlayer::implementation
