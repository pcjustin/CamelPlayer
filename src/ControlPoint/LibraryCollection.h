// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// A virtualized, read-only ItemsSource over one paged Server listing (docs/Plan.md §8.2). List controls read only the
// indices they realize; each missing page is fetched once on a background thread and applied on the UI thread, so the
// UI thread never touches the database.
#pragma once

#include <functional>

#include "BrowseItem.h"
#include "ItemsVector.h"
#include "Server/Api/PagedListing.h"

namespace ui {

struct BrowsePage {
    uint64_t revision = 0;
    int64_t total = 0;
    std::vector<BrowseRow> rows;
};

// Runs off the UI thread; a pinned revision makes it throw once the library changed.
using BrowseFetch = std::function<BrowsePage(int64_t offset, int64_t limit, std::optional<uint64_t> revision)>;

class LibraryCollection : public ItemsVector<LibraryCollection> {
public:
    using Item = winrt::CamelPlayer::BrowseItem;

    LibraryCollection(winrt::Microsoft::UI::Dispatching::DispatcherQueue queue, BrowseFetch fetch)
        : queue_(std::move(queue)), fetch_(std::move(fetch)) {}

    // Loads the first page; until it arrives the collection is empty. `reset` hears the size of each library state.
    void start(std::function<void(int64_t)> reset = {}) {
        reset_ = std::move(reset);
        load(0, std::nullopt);
    }

    // Rereads the first page without a pinned revision; a newer library state updates the view where it is read.
    void refresh() {
        if (!refreshing_) load(0, std::nullopt, true);
    }

    // The library state the rows shown come from; none before the first page.
    std::optional<uint64_t> revision() const { return listing_.revision(); }

    uint32_t Size() const { return uint32_t(std::min<int64_t>(listing_.size(), UINT32_MAX)); }

    IInspectable GetAt(uint32_t index) {
        if (index >= Size()) throw winrt::hresult_out_of_bounds();
        auto& item = items_[index];
        if (!item) item = winrt::make<winrt::CamelPlayer::implementation::BrowseItem>();
        if (const auto* row = listing_.at(index)) {
            auto* self = winrt::get_self<winrt::CamelPlayer::implementation::BrowseItem>(item.as<Item>());
            if (!self->loaded()) self->fill(*row);
        }
        trim(index);
        schedule();
        return item;
    }

    void InsertAt(uint32_t, IInspectable const&) { throw winrt::hresult_illegal_method_call(); }
    void RemoveAt(uint32_t) { throw winrt::hresult_illegal_method_call(); }

    // The row behind a realized item, for click handling.
    static const BrowseRow& rowOf(IInspectable const& item) {
        return winrt::get_self<winrt::CamelPlayer::implementation::BrowseItem>(item.as<Item>())->row();
    }

private:
    // Batches the pages wanted by one layout pass into fetches after it.
    void schedule() {
        if (scheduled_) return;
        scheduled_ = true;
        queue_.TryEnqueue(winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority::Low, [weak = get_weak()] {
            auto self = weak.get();
            if (!self) return;
            self->scheduled_ = false;
            for (const auto page : self->listing_.takeWanted()) self->load(page, self->listing_.revision());
        });
    }

    winrt::fire_and_forget load(int64_t page, std::optional<uint64_t> revision, bool refresh = false) {
        auto weak = get_weak();
        auto fetch = fetch_;
        auto queue = queue_;
        const int64_t size = listing_.pageSize();
        refreshing_ = refreshing_ || refresh;
        co_await winrt::resume_background();
        auto result = std::make_shared<BrowsePage>();
        bool failed = false;
        try {
            *result = fetch(page * size, size, revision);
        } catch (...) {
            failed = true; // a changed revision, cancelled query or offline library: the next refresh retries
        }
        queue.TryEnqueue([weak, page, result, failed, refresh] {
            if (auto self = weak.get()) self->delivered(page, *result, failed, refresh);
        });
    }

    void delivered(int64_t page, BrowsePage& result, bool failed, bool refresh) {
        if (refresh) refreshing_ = false;
        if (failed) {
            listing_.failed(page);
            retryLater();
            return;
        }
        const auto revision = listing_.revision();
        if (refresh && revision && *revision == result.revision) return;
        const uint32_t before = Size();
        if (listing_.deliver(page, result.revision, result.total, std::move(result.rows))) {
            if (refresh && !items_.empty())
                revise(before);
            else {
                items_.clear();
                notify(CollectionChange::Reset, 0u);
            }
            if (reset_) reset_(listing_.size());
            return;
        }
        const int64_t first = page * listing_.pageSize();
        for (int64_t i = first; i < first + listing_.pageSize(); ++i) {
            const auto found = items_.find(uint32_t(i));
            if (found == items_.end()) continue;
            if (const auto* row = listing_.at(i))
                winrt::get_self<winrt::CamelPlayer::implementation::BrowseItem>(found->second.as<Item>())->fill(*row);
        }
    }

    // A failed page is asked for again after a pause, not at once against what made it fail.
    void retryLater() {
        if (!retry_) {
            retry_ = queue_.CreateTimer();
            retry_.Interval(std::chrono::seconds(1));
            retry_.IsRepeating(false);
            retry_.Tick([weak = get_weak()](auto&&, auto&&) {
                if (auto self = weak.get()) self->schedule();
            });
        }
        retry_.Start();
    }

    // A scan or edit produced a newer library state while the view is read. Like Roon, the view stays where it is:
    // realized items show the new rows at their positions (pages not yet cached arrive as usual), and only the end of
    // the list grows or shrinks. A reset would scroll back to the top on every library change.
    void revise(uint32_t before) {
        const uint32_t after = Size();
        std::erase_if(items_, [after](const auto& pair) { return pair.first >= after; });
        for (auto& [index, item] : items_) {
            auto* self = winrt::get_self<winrt::CamelPlayer::implementation::BrowseItem>(item.as<Item>());
            if (const auto* row = listing_.at(index)) self->fill(*row);
        }
        schedule();
        for (uint32_t i = before; i < after; ++i) notify(CollectionChange::ItemInserted, i);
        for (uint32_t i = before; i > after; --i) notify(CollectionChange::ItemRemoved, i - 1);
    }

    winrt::Microsoft::UI::Dispatching::DispatcherQueue queue_;
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer retry_{nullptr};
    BrowseFetch fetch_;
    std::function<void(int64_t)> reset_;
    camel::server::PagedListing<BrowseRow> listing_{100, 64};
    bool scheduled_ = false, refreshing_ = false;
};

} // namespace ui
