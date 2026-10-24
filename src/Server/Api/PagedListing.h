// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <algorithm>
#include <cstdint>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace camel::server {

// Client-side cache of one paged Server listing for virtualized views (docs/Plan.md §8.2). Views read any index;
// missing pages are requested once and fetched off the UI thread. All pages come from one library revision: a page from
// a newer revision replaces the cache, so a scan never mixes two orders. Not thread-safe; use it on the UI thread.
template <class T>
class PagedListing {
public:
    explicit PagedListing(int64_t pageSize = 100, size_t maxPages = 64) : pageSize_(pageSize), maxPages_(maxPages) {}

    int64_t pageSize() const { return pageSize_; }
    int64_t size() const { return total_; }
    std::optional<uint64_t> revision() const { return revision_; }

    // The cached item, or nothing with its page queued for fetching.
    const T* at(int64_t index) {
        if (index < 0 || (revision_ && index >= total_)) return nullptr;
        const int64_t page = index / pageSize_;
        if (const auto found = pages_.find(page); found != pages_.end()) {
            touch(page);
            const auto offset = size_t(index - page * pageSize_);
            return offset < found->second.size() ? &found->second[offset] : nullptr;
        }
        if (!inFlight_.contains(page)) wanted_.insert(page);
        return nullptr;
    }

    // Pages to fetch now; they count as in flight until delivered or failed.
    std::vector<int64_t> takeWanted() {
        std::vector<int64_t> out(wanted_.begin(), wanted_.end());
        inFlight_.insert(wanted_.begin(), wanted_.end());
        wanted_.clear();
        return out;
    }

    // Returns true when the page started a new revision: the view must then reread its size and every item.
    bool deliver(int64_t page, uint64_t revision, int64_t total, std::vector<T> items) {
        inFlight_.erase(page);
        const bool reset = !revision_ || *revision_ != revision;
        if (reset) {
            if (revision_ && revision < *revision_) return false; // an older answer that raced a refresh
            pages_.clear();
            order_.clear();
            revision_ = revision;
            total_ = total;
        }
        pages_[page] = std::move(items);
        touch(page);
        while (pages_.size() > maxPages_) {
            pages_.erase(order_.back());
            order_.pop_back();
        }
        return reset;
    }

    // A failed fetch may be retried by the next read of its page.
    void failed(int64_t page) { inFlight_.erase(page); }

private:
    void touch(int64_t page) {
        order_.remove(page);
        order_.push_front(page);
    }

    int64_t pageSize_;
    size_t maxPages_;
    int64_t total_ = 0;
    std::optional<uint64_t> revision_;
    std::map<int64_t, std::vector<T>> pages_;
    std::list<int64_t> order_; // most recently read first
    std::set<int64_t> wanted_, inFlight_;
};

} // namespace camel::server
