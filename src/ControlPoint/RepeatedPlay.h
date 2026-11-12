// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "Server/Api/ServerApi.h"

namespace ui {

// Lists play on a single click, so a double click sends the same play twice; the second would only restart the track
// it has just started, cutting its crossfade.
class RepeatedPlay {
public:
    struct Request {
        camel::server::CommandKind kind{};
        uint64_t index = 0;
        size_t count = 0;
        std::string path;
        bool operator==(const Request&) const = default;
    };

    // Whether `play`, sent at `now`, is the second of a double click within `window`.
    bool repeated(Request play, std::chrono::steady_clock::time_point now, std::chrono::milliseconds window) {
        const bool same = play == last_ && now - lastAt_ < window;
        last_ = std::move(play);
        lastAt_ = now;
        return same;
    }
    // Any other command was sent: a play after it, as after Stop, is the user's own.
    void other() { last_.reset(); }

private:
    std::optional<Request> last_;
    std::chrono::steady_clock::time_point lastAt_;
};

} // namespace ui
