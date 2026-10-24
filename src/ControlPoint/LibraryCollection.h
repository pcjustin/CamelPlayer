// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// A virtualized, read-only ItemsSource over one paged Server listing (docs/Plan.md §8.2). List controls read only the
// indices they realize; each missing page is fetched once on a background thread and applied on the UI thread, so the
// UI thread never touches the database.
#pragma once

#include <functional>
#include <unordered_map>

#include "BrowseItem.h"
#include "Server/Api/PagedListing.h"

namespace ui {

struct BrowsePage {
    uint64_t revision = 0;
    int64_t total = 0;
    std::vector<BrowseRow> rows;
};

// Runs off the UI thread; a pinned revision makes it throw once the library changed.
using BrowseFetch = std::function<BrowsePage(int64_t offset, int64_t limit, std::optional<uint64_t> revision)>;

class LibraryCollection
    : public winrt::implements<LibraryCollection, winrt::Windows::Foundation::Collections::IObservableVector<winrt::Windows::Foundation::IInspectable>,
                               winrt::Windows::Foundation::Collections::IVector<winrt::Windows::Foundation::IInspectable>,
                               winrt::Windows::Foundation::Collections::IVectorView<winrt::Windows::Foundation::IInspectable>,
                               winrt::Windows::Foundation::Collections::IIterable<winrt::Windows::Foundation::IInspectable>> {
public:
    using IInspectable = winrt::Windows::Foundation::IInspectable;
    using Item = winrt::CamelPlayer::BrowseItem;

    LibraryCollection(winrt::Microsoft::UI::Dispatching::DispatcherQueue queue, BrowseFetch fetch)
        : queue_(std::move(queue)), fetch_(std::move(fetch)) {}

    // Loads the first page; until it arrives the collection is empty. `reset` hears the size of each library state.
    void start(std::function<void(int64_t)> reset = {}) {
        reset_ = std::move(reset);
        load(0, std::nullopt);
    }

    // Rereads the first page without a pinned revision; a newer library state resets the view.
    void refresh() {
        if (!refreshing_) load(0, std::nullopt, true);
    }

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

    uint32_t GetMany(uint32_t start, winrt::array_view<IInspectable> out) {
        uint32_t n = 0;
        for (; n < out.size() && start + n < Size(); ++n) out[n] = GetAt(start + n);
        return n;
    }

    bool IndexOf(IInspectable const& value, uint32_t& index) const {
        for (const auto& [i, item] : items_)
            if (item == value) {
                index = uint32_t(i);
                return true;
            }
        return false;
    }

    winrt::Windows::Foundation::Collections::IVectorView<IInspectable> GetView() { return *this; }
    winrt::Windows::Foundation::Collections::IIterator<IInspectable> First() { return winrt::make<Iterator>(*this); }

    void SetAt(uint32_t, IInspectable const&) { throw winrt::hresult_illegal_method_call(); }
    void InsertAt(uint32_t, IInspectable const&) { throw winrt::hresult_illegal_method_call(); }
    void RemoveAt(uint32_t) { throw winrt::hresult_illegal_method_call(); }
    void Append(IInspectable const&) { throw winrt::hresult_illegal_method_call(); }
    void RemoveAtEnd() { throw winrt::hresult_illegal_method_call(); }
    void Clear() { throw winrt::hresult_illegal_method_call(); }
    void ReplaceAll(winrt::array_view<IInspectable const>) { throw winrt::hresult_illegal_method_call(); }

    winrt::event_token VectorChanged(winrt::Windows::Foundation::Collections::VectorChangedEventHandler<IInspectable> const& handler) {
        return changed_.add(handler);
    }
    void VectorChanged(winrt::event_token const& token) noexcept { changed_.remove(token); }

    // The row behind a realized item, for click handling.
    static const BrowseRow& rowOf(IInspectable const& item) {
        return winrt::get_self<winrt::CamelPlayer::implementation::BrowseItem>(item.as<Item>())->row();
    }

private:
    struct ResetArgs : winrt::implements<ResetArgs, winrt::Windows::Foundation::Collections::IVectorChangedEventArgs> {
        winrt::Windows::Foundation::Collections::CollectionChange CollectionChange() const {
            return winrt::Windows::Foundation::Collections::CollectionChange::Reset;
        }
        uint32_t Index() const { return 0; }
    };

    struct Iterator : winrt::implements<Iterator, winrt::Windows::Foundation::Collections::IIterator<IInspectable>> {
        explicit Iterator(LibraryCollection& owner) : owner_(owner.get_strong()) {}
        IInspectable Current() const { return owner_->GetAt(index_); }
        bool HasCurrent() const { return index_ < owner_->Size(); }
        bool MoveNext() { return ++index_ < owner_->Size(); }
        uint32_t GetMany(winrt::array_view<IInspectable> out) {
            const auto n = owner_->GetMany(index_, out);
            index_ += n;
            return n;
        }
        winrt::com_ptr<LibraryCollection> owner_;
        uint32_t index_ = 0;
    };

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
            return;
        }
        const auto revision = listing_.revision();
        if (refresh && revision && *revision == result.revision) return;
        if (listing_.deliver(page, result.revision, result.total, std::move(result.rows))) {
            items_.clear();
            changed_(*this, winrt::make<ResetArgs>());
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

    // Keeps the items around the reading position so scrolling a 100k-row view does not keep 100k objects alive.
    void trim(uint32_t index) {
        if (items_.size() <= 4000) return;
        std::erase_if(items_, [index](const auto& pair) { return pair.first + 2000 < index || pair.first > index + 2000; });
    }

    winrt::Microsoft::UI::Dispatching::DispatcherQueue queue_;
    BrowseFetch fetch_;
    std::function<void(int64_t)> reset_;
    camel::server::PagedListing<BrowseRow> listing_{100, 64};
    std::unordered_map<uint32_t, IInspectable> items_;
    winrt::event<winrt::Windows::Foundation::Collections::VectorChangedEventHandler<IInspectable>> changed_;
    bool scheduled_ = false, refreshing_ = false;
};

} // namespace ui
