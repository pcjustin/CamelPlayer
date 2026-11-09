// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include <memory>
#include <mutex>
#include <optional>

#include "Engine/PlaybackEngine.h"

namespace testutil {

// Records the first snapshot the engine publishes with an output plan. A short track can end, which clears the plan,
// before the test thread reads a snapshot after waitIdle().
class StartedSnapshot {
public:
    explicit StartedSnapshot(camel::audio::PlaybackEngine& engine) : engine_(engine) {
        // The engine calls a copy of its listener outside its lock, so a call can outlive setListener({}).
        engine_.setListener([&engine, state = state_] {
            auto s = engine.snapshot();
            std::lock_guard lock(state->mutex);
            if (s.plan && !state->snapshot) state->snapshot = std::move(s);
        });
    }
    ~StartedSnapshot() { engine_.setListener({}); }
    StartedSnapshot(const StartedSnapshot&) = delete;
    StartedSnapshot& operator=(const StartedSnapshot&) = delete;

    std::optional<camel::audio::EngineSnapshot> get() const {
        std::lock_guard lock(state_->mutex);
        return state_->snapshot;
    }
    void reset() {
        std::lock_guard lock(state_->mutex);
        state_->snapshot.reset();
    }

private:
    struct State {
        std::mutex mutex;
        std::optional<camel::audio::EngineSnapshot> snapshot;
    };
    camel::audio::PlaybackEngine& engine_;
    std::shared_ptr<State> state_ = std::make_shared<State>();
};

} // namespace testutil
