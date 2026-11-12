// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <chrono>
#include <functional>
#include <thread>

#include "Server/Artwork/ArtworkProvider.h"

namespace ui {

// The window applies an answer to a consent through the Server's command queue, a moment after the click, or longer
// while the Server is busy: whether `applied` came true before `token` was cancelled, within a minute.
inline bool consentApplied(const std::function<bool()>& applied, const camel::server::CancellationToken& token) {
    const auto giveUp = std::chrono::steady_clock::now() + std::chrono::minutes(1);
    while (!applied()) {
        if (token.cancelled() || std::chrono::steady_clock::now() >= giveUp) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
}

} // namespace ui
