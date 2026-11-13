// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include "ControlPoint/SnapshotForward.h"

using namespace camel::server;

namespace {
Event event(EventKind kind, uint64_t position, PlaybackState state = PlaybackState::Playing) {
    Event e;
    e.kind = kind;
    e.snapshot.playback.position = position;
    e.snapshot.playback.state = state;
    return e;
}
} // namespace

TEST(SnapshotForward, AnEventThatMovesOnlyThePositionIsNotSentOn) {
    ui::SnapshotForward forward;
    EXPECT_TRUE(forward.forward(event(EventKind::StateChanged, 0)));
    EXPECT_FALSE(forward.forward(event(EventKind::StateChanged, 4410)));
    EXPECT_FALSE(forward.forward(event(EventKind::StateChanged, 8820)));
    EXPECT_TRUE(forward.forward(event(EventKind::StateChanged, 8820, PlaybackState::Paused)));
    EXPECT_FALSE(forward.forward(event(EventKind::StateChanged, 8820, PlaybackState::Paused)));
    EXPECT_TRUE(forward.forward(event(EventKind::CommandCompleted, 8820, PlaybackState::Paused)));
}
