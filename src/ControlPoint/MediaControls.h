// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// System Media Transport Controls (docs/Plan.md §8.4): media keys, the Windows volume flyout and the lock screen show
// the current track and send their commands back through the window, which submits them to the Server.
#pragma once

#include <functional>
#include <memory>
#include <string>

#include <winrt/Windows.Media.h>

#include "Server/Api/ServerApi.h"

namespace ui {

class MediaControls {
public:
    struct Handlers {
        std::function<void(camel::server::CommandKind)> command; // Play, Pause, Stop, Next, Previous
        std::function<void(double seconds)> seek;
    };

    // Called on the UI thread; button presses arrive on the thread of `queue`.
    MediaControls(HWND window, winrt::Microsoft::UI::Dispatching::DispatcherQueue queue, const Handlers& handlers,
                  std::shared_ptr<camel::server::IServer> server);
    ~MediaControls();

    // The track as the playback bar shows it; positions are in seconds.
    void update(const camel::server::PlaybackSnapshot& playback, const std::string& title, const std::string& artist,
                const std::string& album, double position, double length);

private:
    winrt::fire_and_forget showCover(std::string path);

    winrt::Windows::Media::SystemMediaTransportControls controls_{nullptr};
    winrt::event_token buttonToken_{}, seekToken_{};
    std::shared_ptr<camel::server::IServer> server_;
    std::string shownPath_, shownTitle_, shownArtist_, shownAlbum_;
    camel::server::PlaybackState shownState_ = camel::server::PlaybackState::Stopped;
    double shownLength_ = -1, shownPosition_ = -1;
    camel::server::CancellationToken coverToken_;
};

} // namespace ui
