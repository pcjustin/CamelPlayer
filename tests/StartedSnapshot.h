// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include <mutex>
#include <optional>

#include "Engine/PlaybackEngine.h"

namespace testutil {

// Records the first snapshot the engine publishes with an output plan. A short track can end, which clears the plan,
// before the test thread reads a snapshot after waitIdle().
class StartedSnapshot {
public:
    explicit StartedSnapshot(camel::audio::PlaybackEngine& engine) : engine_(engine) {
        engine_.setListener([this] {
            auto s = engine_.snapshot();
            std::lock_guard lock(mutex_);
            if (s.plan && !snapshot_) snapshot_ = std::move(s);
        });
    }
    ~StartedSnapshot() { engine_.setListener({}); }
    StartedSnapshot(const StartedSnapshot&) = delete;
    StartedSnapshot& operator=(const StartedSnapshot&) = delete;

    std::optional<camel::audio::EngineSnapshot> get() const {
        std::lock_guard lock(mutex_);
        return snapshot_;
    }
    void reset() {
        std::lock_guard lock(mutex_);
        snapshot_.reset();
    }

private:
    camel::audio::PlaybackEngine& engine_;
    mutable std::mutex mutex_;
    std::optional<camel::audio::EngineSnapshot> snapshot_;
};

} // namespace testutil
