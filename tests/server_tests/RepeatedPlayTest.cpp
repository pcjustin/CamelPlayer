// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <gtest/gtest.h>

#include <chrono>

#include "ControlPoint/RepeatedPlay.h"

using namespace std::chrono_literals;
using camel::server::CommandKind;

// The second play of a double click is dropped; a play sent again after another command, as Stop, is the user's.
TEST(RepeatedPlay, APlayAfterAnotherCommandIsNotTakenForADoubleClick) {
    ui::RepeatedPlay plays;
    const ui::RepeatedPlay::Request play{CommandKind::QueueReplace, 0, 12, "C:/Music/a.flac"};
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(plays.repeated(play, start, 500ms));
    EXPECT_TRUE(plays.repeated(play, start + 150ms, 500ms)); // the second click
    plays.other();                                           // Stop
    EXPECT_FALSE(plays.repeated(play, start + 300ms, 500ms));
    EXPECT_FALSE(plays.repeated(play, start + 1000ms, 500ms));
}
