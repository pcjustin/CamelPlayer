// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Which of the Server's events the window takes. The Server reports the position with every engine poll, 20 times a
// second, and the window's 250 ms refresh shows it, so an event that moves only the position is not sent to the UI
// thread at all, minimized or not.
#pragma once

#include <mutex>
#include <optional>

#include "Server/Api/ServerApi.h"

namespace ui {

inline bool movesOnlyThePosition(const camel::server::Snapshot& snapshot, const camel::server::Snapshot& shown) {
    const auto &now = snapshot.playback, &before = shown.playback;
    return snapshot.queueEntryId == shown.queueEntryId && snapshot.queue.revision == shown.queue.revision &&
           snapshot.queue.marks == shown.queue.marks && snapshot.queue.upcomingEntryId == shown.queue.upcomingEntryId &&
           snapshot.devices.revision == shown.devices.revision && snapshot.commandError == shown.commandError &&
           now.revision == before.revision && now.state == before.state && now.pathUtf8 == before.pathUtf8 &&
           now.total == before.total && now.underruns == before.underruns;
}

class SnapshotForward {
public:
    // True when the event is to be sent on; it is then what the next event is compared with.
    bool forward(const camel::server::Event& event) {
        std::scoped_lock lock(mutex_);
        if (event.kind != camel::server::EventKind::CommandCompleted && sent_ && movesOnlyThePosition(event.snapshot, *sent_))
            return false;
        sent_ = event.snapshot;
        return true;
    }

private:
    std::mutex mutex_;
    std::optional<camel::server::Snapshot> sent_;
};

} // namespace ui
