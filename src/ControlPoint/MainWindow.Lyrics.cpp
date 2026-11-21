// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pch.h"

#include "MainWindow.xaml.h"
#include "MainWindowParts.h"

#include <utility>

#include "UiText.h"
#include "Server/Library/Lyrics.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace camel;
using namespace camel::audio;
using ui::tr;
using namespace camel::server;

namespace winrt::CamelPlayer::implementation {

using namespace mainwindow;

void MainWindow::OnCoverClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    openLyrics(!lyricsOpen_);
}

constexpr auto kLyricsFollowAgain = std::chrono::seconds(3);
constexpr double kLyricsDimmed = 0.45;

void MainWindow::openLyrics(bool open) {
    lyricsOpen_ = open;
    LyricsView().Visibility(open ? Visibility::Visible : Visibility::Collapsed);
    const hstring tip = tr(open ? L"Lyrics_Hide" : L"Lyrics_Show");
    ToolTipService::SetToolTip(CoverButton(), box_value(tip));
    Automation::AutomationProperties::SetName(CoverButton(), tip);
    showTitleBack();
    if (!open) return;
    lyricsCentered_ = false;
    lyricsScrolledAt_ = {};
    const auto path = server_ ? server_->snapshot().playback.pathUtf8 : std::string();
    if (path != lyricsPath_) loadLyrics(path);
    showLyrics();
}

// Asked when a track starts, open or not, so that opening finds them; the Server reads them off this thread.
winrt::fire_and_forget MainWindow::loadLyrics(std::string path) {
    auto strong = get_strong();
    lyricsPath_ = path;
    lyrics_ = {};
    if (lyricsOpen_) showLyrics();
    if (path.empty() || !server_) co_return;
    auto server = server_;
    winrt::apartment_context foreground;
    co_await winrt::resume_background();
    auto read = server->lyrics(path);
    co_await foreground;
    if (lyricsPath_ != path) co_return;
    lyrics_ = std::move(read);
    if (lyricsOpen_) showLyrics();
}

void MainWindow::showLyrics() {
    lyricsLit_.reset();
    lyricsCentered_ = false;
    auto lines = LyricsLines().Children();
    lines.Clear();
    const bool none = lyrics_.lines.empty();
    LyricsNone().Visibility(none ? Visibility::Visible : Visibility::Collapsed);
    LyricsScroller().Visibility(none ? Visibility::Collapsed : Visibility::Visible);
    if (none) {
        LyricsNoneTitle().Text(TitleText().Text());
        hstring artist;
        if (server_) {
            const auto snapshot = server_->snapshot();
            const auto current = std::ranges::find(snapshot.queue.entries, snapshot.queueEntryId, &QueueEntry::id);
            if (current != snapshot.queue.entries.end()) artist = ui::utf8(current->artist);
        }
        LyricsNoneArtist().Text(artist);
        return;
    }
    for (const auto& line : lyrics_.lines) {
        TextBlock text;
        text.Text(ui::utf8(line.text));
        text.FontSize(lyrics_.synced ? 30 : 24);
        text.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
        text.Foreground(Media::SolidColorBrush(Windows::UI::Colors::White()));
        text.TextAlignment(TextAlignment::Center);
        text.HorizontalAlignment(HorizontalAlignment::Center);
        text.TextWrapping(TextWrapping::Wrap);
        text.Opacity(lyrics_.synced ? kLyricsDimmed : 0.9);
        if (line.ms) {
            text.Tapped([this, ms = *line.ms](auto&&, auto&&) {
                const auto s = server_ ? server_->snapshot().playback : PlaybackSnapshot{};
                if (!s.source) return;
                Command seek;
                seek.kind = CommandKind::Seek;
                seek.frame = secondsToFrames(ms / 1000.0, s.source->sampleRate);
                submitCommand(std::move(seek));
                lyricsScrolledAt_ = {};
                lyricsCentered_ = false;
            });
        }
        lines.Append(text);
    }
    LyricsScroller().ChangeView(nullptr, 0.0, nullptr, true);
    // Paused, no snapshot follows to light the line, so the position is read once the lines are laid out.
    DispatcherQueue().TryEnqueue(Microsoft::UI::Dispatching::DispatcherQueuePriority::Low, [weak = get_weak()] {
        const auto self = weak.get();
        if (!self || !self->server_) return;
        const auto s = self->server_->snapshot().playback;
        if (s.source && s.source->sampleRate) self->followLyrics(framesToSeconds(s.position, s.source->sampleRate));
    });
}

// The lines of the stamp being sung lit and brought to the middle, unless the user scrolled in the last seconds.
void MainWindow::followLyrics(double seconds) {
    if (!lyricsOpen_ || !lyrics_.synced || lyrics_.lines.empty()) return;
    const auto at = camel::server::lyricLineAt(lyrics_, uint32_t(std::max(0.0, seconds) * 1000));
    auto lines = LyricsLines().Children();
    if (at != lyricsLit_) {
        lyricsLit_ = at;
        lyricsCentered_ = false;
        for (uint32_t i = 0; i < lines.Size(); ++i) {
            const bool lit = at && lyrics_.lines[i].ms == lyrics_.lines[*at].ms;
            lines.GetAt(i).as<TextBlock>().Opacity(lit ? 1.0 : kLyricsDimmed);
        }
    }
    if (!at || lyricsCentered_ || std::chrono::steady_clock::now() - lyricsScrolledAt_ < kLyricsFollowAgain) return;
    size_t last = *at;
    while (last + 1 < lyrics_.lines.size() && lyrics_.lines[last + 1].ms == lyrics_.lines[*at].ms) ++last;
    const auto first = lines.GetAt(uint32_t(*at)).as<FrameworkElement>();
    const auto end = lines.GetAt(uint32_t(last)).as<FrameworkElement>();
    const auto top = first.TransformToVisual(LyricsLines()).TransformPoint({0, 0}).Y;
    const auto bottom = end.TransformToVisual(LyricsLines()).TransformPoint({0, 0}).Y + float(end.ActualHeight());
    const double middle = (top + bottom) / 2.0 - LyricsScroller().ViewportHeight() / 2.0;
    LyricsScroller().ChangeView(nullptr, std::max(0.0, middle), nullptr, false);
    lyricsCentered_ = true;
}

void MainWindow::OnLyricsScrollerSizeChanged(Windows::Foundation::IInspectable const&, SizeChangedEventArgs const& e) {
    // Room above the first line and below the last, so that either can stand in the middle.
    const double half = e.NewSize().Height / 2;
    LyricsLines().Padding(ThicknessHelper::FromLengths(32, half, 32, half));
    lyricsCentered_ = false;
}

} // namespace winrt::CamelPlayer::implementation
