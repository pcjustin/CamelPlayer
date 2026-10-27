// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pch.h"

#include "MediaControls.h"

#include <systemmediatransportcontrolsinterop.h>

#include <cmath>

#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>

#include "Common/util/Log.h"
#include "Common/util/Text.h"
#include "UiText.h"

using namespace winrt;
using namespace Windows::Media;
using camel::server::CommandKind;
using camel::server::PlaybackState;

namespace ui {
namespace {

Windows::Foundation::TimeSpan span(double seconds) {
    return std::chrono::duration_cast<Windows::Foundation::TimeSpan>(std::chrono::duration<double>(std::max(0.0, seconds)));
}

} // namespace

MediaControls::MediaControls(HWND window, Microsoft::UI::Dispatching::DispatcherQueue queue, const Handlers& handlers,
                             std::shared_ptr<camel::server::IServer> server)
    : server_(std::move(server)) {
    try {
        const auto interop = get_activation_factory<SystemMediaTransportControls, ISystemMediaTransportControlsInterop>();
        check_hresult(interop->GetForWindow(window, guid_of<SystemMediaTransportControls>(), put_abi(controls_)));
    } catch (const hresult_error& e) {
        camel::log::warn("media controls: 0x%08x", uint32_t(e.code()));
        return;
    }
    controls_.IsEnabled(true);
    controls_.IsPlayEnabled(true);
    controls_.IsPauseEnabled(true);
    controls_.IsStopEnabled(true);
    controls_.IsNextEnabled(true);
    controls_.IsPreviousEnabled(true);
    controls_.PlaybackStatus(MediaPlaybackStatus::Closed);
    buttonToken_ = controls_.ButtonPressed(
        [queue, command = handlers.command](auto&&, SystemMediaTransportControlsButtonPressedEventArgs const& e) {
            CommandKind kind;
            switch (e.Button()) {
            case SystemMediaTransportControlsButton::Play:
                kind = CommandKind::Play;
                break;
            case SystemMediaTransportControlsButton::Pause:
                kind = CommandKind::Pause;
                break;
            case SystemMediaTransportControlsButton::Stop:
                kind = CommandKind::Stop;
                break;
            case SystemMediaTransportControlsButton::Next:
                kind = CommandKind::Next;
                break;
            case SystemMediaTransportControlsButton::Previous:
                kind = CommandKind::Previous;
                break;
            default:
                return;
            }
            queue.TryEnqueue([command, kind] { command(kind); });
        });
    seekToken_ = controls_.PlaybackPositionChangeRequested(
        [queue, seek = handlers.seek](auto&&, PlaybackPositionChangeRequestedEventArgs const& e) {
            const double seconds = std::chrono::duration<double>(e.RequestedPlaybackPosition()).count();
            queue.TryEnqueue([seek, seconds] { seek(seconds); });
        });
}

MediaControls::~MediaControls() {
    if (!controls_) return;
    coverToken_.cancel();
    controls_.ButtonPressed(buttonToken_);
    controls_.PlaybackPositionChangeRequested(seekToken_);
}

void MediaControls::update(const camel::server::PlaybackSnapshot& playback, const std::string& title, const std::string& artist,
                           const std::string& album, double position, double length) {
    if (!controls_) return;
    if (playback.pathUtf8.empty()) {
        if (shownPath_.empty() && shownState_ == PlaybackState::Stopped) return;
        shownPath_.clear();
        shownState_ = PlaybackState::Stopped;
        coverToken_.cancel();
        controls_.DisplayUpdater().ClearAll();
        controls_.DisplayUpdater().Update();
        controls_.PlaybackStatus(MediaPlaybackStatus::Closed);
        return;
    }
    if (playback.pathUtf8 != shownPath_ || title != shownTitle_ || artist != shownArtist_ || album != shownAlbum_) {
        const bool newTrack = playback.pathUtf8 != shownPath_;
        shownPath_ = playback.pathUtf8;
        shownTitle_ = title;
        shownArtist_ = artist;
        shownAlbum_ = album;
        auto updater = controls_.DisplayUpdater();
        updater.Type(MediaPlaybackType::Music);
        updater.MusicProperties().Title(utf8(title));
        updater.MusicProperties().Artist(utf8(artist));
        updater.MusicProperties().AlbumTitle(utf8(album));
        if (newTrack) updater.Thumbnail(nullptr);
        updater.Update();
        if (newTrack) showCover(shownPath_);
    }
    if (playback.state != shownState_) {
        shownState_ = playback.state;
        controls_.PlaybackStatus(playback.state == PlaybackState::Playing  ? MediaPlaybackStatus::Playing
                                 : playback.state == PlaybackState::Paused ? MediaPlaybackStatus::Paused
                                                                           : MediaPlaybackStatus::Stopped);
        shownPosition_ = -1; // the timeline follows every change of state
    }
    // Windows moves the shown position along by itself while playing; corrections once a second are enough.
    if (length != shownLength_ || std::abs(position - shownPosition_) >= 1.0) {
        shownLength_ = length;
        shownPosition_ = position;
        SystemMediaTransportControlsTimelineProperties timeline;
        timeline.StartTime(span(0));
        timeline.MinSeekTime(span(0));
        timeline.EndTime(span(length));
        timeline.MaxSeekTime(span(length));
        timeline.Position(span(std::min(position, length)));
        controls_.UpdateTimelineProperties(timeline);
    }
}

// The cover the playback bar shows: the track's cached thumbnail, read through the Server off the UI thread.
winrt::fire_and_forget MediaControls::showCover(std::string path) {
    coverToken_.cancel();
    coverToken_ = {};
    auto token = coverToken_;
    auto server = server_;
    auto controls = controls_;
    co_await resume_background();
    try {
        const auto ref = server->artwork({camel::server::ArtworkKind::Track, 0, path}, token);
        if (!ref || token.cancelled()) co_return;
        const auto file = co_await Windows::Storage::StorageFile::GetFileFromPathAsync(hstring(ref->path.wstring()));
        if (token.cancelled()) co_return;
        auto updater = controls.DisplayUpdater();
        updater.Thumbnail(Windows::Storage::Streams::RandomAccessStreamReference::CreateFromFile(file));
        updater.Update();
    } catch (const hresult_error& e) {
        camel::log::warn("media controls cover: 0x%08x", uint32_t(e.code()));
    } catch (const std::exception& e) {
        camel::log::warn("media controls cover: %s", e.what());
    }
}

} // namespace ui
