// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <cstdint>
#include <unordered_map>

namespace ui {

// The reads of a folder tree's folders, made off the UI thread: start() before a read, and current() as its result
// comes, to tell whether it may list its rows. Only a folder's last read does, as one opened, closed and opened again
// while the library is busy has two on their way.
class TreeReads {
public:
    uint64_t start(int64_t folder) { return ++reads_[folder]; }
    bool current(int64_t folder, uint64_t read) const {
        const auto found = reads_.find(folder);
        return found != reads_.end() && found->second == read;
    }

private:
    std::unordered_map<int64_t, uint64_t> reads_;
};

} // namespace ui
