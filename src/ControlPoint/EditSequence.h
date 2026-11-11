// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <utility>

namespace ui {

// Edits of a list the Server keeps by revision, which refuses an edit made from a revision it has gone past. `send`
// starts an edit off the UI thread; its answer comes back through answered(). Edits go one at a time, each with the
// revision the one before left, so edits made in a row are all applied, in order. `Change` has an optional revision.
template <class Change> class EditSequence {
public:
    explicit EditSequence(std::function<void(Change)> send) : send_(std::move(send)) {}

    // The list as read from the Server; a read made while edits are under way is older than their answers.
    void loaded(int64_t revision) {
        if (!sending_ && waiting_.empty()) revision_ = revision;
    }
    void edit(Change change) {
        waiting_.push_back(std::move(change));
        next();
    }
    // The revision the edit left the list at, or nothing when it was refused or failed: then the list is read again
    // and the edits waiting, made on what it showed, are dropped.
    void answered(std::optional<int64_t> revision) {
        sending_ = false;
        if (revision) {
            revision_ = *revision;
        } else {
            waiting_.clear();
        }
        next();
    }
    // Another list is shown: what is under way belongs to the one before.
    void clear() {
        sending_ = false;
        waiting_.clear();
    }

private:
    void next() {
        if (sending_ || waiting_.empty()) return;
        sending_ = true;
        auto change = std::move(waiting_.front());
        waiting_.pop_front();
        change.revision = revision_;
        send_(std::move(change));
    }

    std::function<void(Change)> send_;
    int64_t revision_ = 0;
    bool sending_ = false;
    std::deque<Change> waiting_;
};

} // namespace ui
