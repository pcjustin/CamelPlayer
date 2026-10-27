// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The queue panel's ItemsSource. A queue can hold a whole 100k-track library, so items are made only for the rows the
// list realizes, and a new snapshot with the same entries refreshes those rows in place instead of resetting the view.
// Drag reordering moves rows through RemoveAt and InsertAt.
#pragma once

#include <algorithm>
#include <unordered_map>
#include <vector>

#include "QueueItem.h"

namespace ui {

class QueueCollection
    : public winrt::implements<
          QueueCollection, winrt::Windows::Foundation::Collections::IObservableVector<winrt::Windows::Foundation::IInspectable>,
          winrt::Windows::Foundation::Collections::IVector<winrt::Windows::Foundation::IInspectable>,
          winrt::Windows::Foundation::Collections::IVectorView<winrt::Windows::Foundation::IInspectable>,
          winrt::Windows::Foundation::Collections::IIterable<winrt::Windows::Foundation::IInspectable>> {
public:
    using IInspectable = winrt::Windows::Foundation::IInspectable;
    using Item = winrt::CamelPlayer::QueueItem;

    // The list shows at most this many rows, around the current entry: about 65k rows (millions of pixels) drew rows
    // past the list's bottom edge at 125 % scaling, and higher scaling lowers that limit.
    static constexpr size_t kShown = 25000;

    void update(std::vector<QueueRow> rows, uint64_t current) {
        size_t first = 0;
        if (rows.size() > kShown) {
            const auto at = std::find_if(rows.begin(), rows.end(), [&](const QueueRow& row) { return row.id == current; });
            const size_t index = at == rows.end() ? 0 : size_t(at - rows.begin());
            first = std::min(index > kShown / 2 ? index - kShown / 2 : 0, rows.size() - kShown);
        }
        const bool same = first == first_ && std::equal(rows.begin(), rows.end(), rows_.begin(), rows_.end(),
                                                        [](const QueueRow& a, const QueueRow& b) { return a.id == b.id; });
        rows_ = std::move(rows);
        first_ = first;
        current_ = current;
        if (same) {
            for (auto& [index, item] : items_) self(item)->fill(row(index), row(index).id == current_);
            return;
        }
        items_.clear();
        changed_(*this, winrt::make<ChangeArgs>(winrt::Windows::Foundation::Collections::CollectionChange::Reset, 0u));
    }

    void setCurrent(uint64_t current) {
        current_ = current;
        for (auto& [index, item] : items_) self(item)->fill(row(index), row(index).id == current_);
    }

    size_t total() const { return rows_.size(); }

    // Every entry in its order, including those outside the listed rows.
    std::vector<uint64_t> ids() const {
        std::vector<uint64_t> result;
        result.reserve(rows_.size());
        for (const auto& row : rows_) result.push_back(row.id);
        return result;
    }

    static uint64_t idOf(IInspectable const& item) { return self(item)->row().id; }

    uint32_t Size() const { return uint32_t(std::min(rows_.size() - first_, kShown)); }

    IInspectable GetAt(uint32_t index) {
        if (index >= Size()) throw winrt::hresult_out_of_bounds();
        auto& item = items_[index];
        if (!item) {
            item = winrt::make<winrt::CamelPlayer::implementation::QueueItem>();
            self(item)->fill(row(index), row(index).id == current_);
        }
        if (items_.size() > 4000)
            std::erase_if(items_, [index](const auto& pair) { return pair.first + 2000 < index || pair.first > index + 2000; });
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
                index = i;
                return true;
            }
        return false;
    }

    winrt::Windows::Foundation::Collections::IVectorView<IInspectable> GetView() { return *this; }
    winrt::Windows::Foundation::Collections::IIterator<IInspectable> First() { return winrt::make<Iterator>(*this); }

    // With more entries than listed rows, the row after the last listed one moves into the list (or out of it), so the
    // list keeps its size.
    void RemoveAt(uint32_t index) {
        using winrt::Windows::Foundation::Collections::CollectionChange;
        if (index >= Size()) throw winrt::hresult_out_of_bounds();
        const auto before = Size();
        rows_.erase(rows_.begin() + ptrdiff_t(first_ + index));
        shift(index, -1);
        changed_(*this, winrt::make<ChangeArgs>(CollectionChange::ItemRemoved, index));
        if (Size() == before) changed_(*this, winrt::make<ChangeArgs>(CollectionChange::ItemInserted, before - 1));
    }

    void InsertAt(uint32_t index, IInspectable const& value) {
        using winrt::Windows::Foundation::Collections::CollectionChange;
        if (index > Size()) throw winrt::hresult_out_of_bounds();
        const auto before = Size();
        rows_.insert(rows_.begin() + ptrdiff_t(first_ + index), self(value)->row());
        shift(index, 1);
        items_[index] = value;
        changed_(*this, winrt::make<ChangeArgs>(CollectionChange::ItemInserted, index));
        if (Size() == before) {
            items_.erase(before);
            changed_(*this, winrt::make<ChangeArgs>(CollectionChange::ItemRemoved, before));
        }
    }

    void SetAt(uint32_t, IInspectable const&) { throw winrt::hresult_illegal_method_call(); }
    void Append(IInspectable const&) { throw winrt::hresult_illegal_method_call(); }
    void RemoveAtEnd() { throw winrt::hresult_illegal_method_call(); }
    void Clear() { throw winrt::hresult_illegal_method_call(); }
    void ReplaceAll(winrt::array_view<IInspectable const>) { throw winrt::hresult_illegal_method_call(); }

    winrt::event_token
    VectorChanged(winrt::Windows::Foundation::Collections::VectorChangedEventHandler<IInspectable> const& handler) {
        return changed_.add(handler);
    }
    void VectorChanged(winrt::event_token const& token) noexcept { changed_.remove(token); }

private:
    struct ChangeArgs : winrt::implements<ChangeArgs, winrt::Windows::Foundation::Collections::IVectorChangedEventArgs> {
        ChangeArgs(winrt::Windows::Foundation::Collections::CollectionChange change, uint32_t index)
            : change_(change), index_(index) {}
        winrt::Windows::Foundation::Collections::CollectionChange CollectionChange() const { return change_; }
        uint32_t Index() const { return index_; }
        winrt::Windows::Foundation::Collections::CollectionChange change_;
        uint32_t index_;
    };

    struct Iterator : winrt::implements<Iterator, winrt::Windows::Foundation::Collections::IIterator<IInspectable>> {
        explicit Iterator(QueueCollection& owner) : owner_(owner.get_strong()) {}
        IInspectable Current() const { return owner_->GetAt(index_); }
        bool HasCurrent() const { return index_ < owner_->Size(); }
        bool MoveNext() { return ++index_ < owner_->Size(); }
        uint32_t GetMany(winrt::array_view<IInspectable> out) {
            const auto n = owner_->GetMany(index_, out);
            index_ += n;
            return n;
        }
        winrt::com_ptr<QueueCollection> owner_;
        uint32_t index_ = 0;
    };

    static winrt::CamelPlayer::implementation::QueueItem* self(IInspectable const& item) {
        return winrt::get_self<winrt::CamelPlayer::implementation::QueueItem>(item.as<Item>());
    }

    const QueueRow& row(uint32_t index) const { return rows_[first_ + index]; }

    // Realized items keep following their rows when rows before them are removed or inserted.
    void shift(uint32_t from, int delta) {
        std::unordered_map<uint32_t, IInspectable> moved;
        for (auto& [index, item] : items_) {
            if (index < from)
                moved.emplace(index, std::move(item));
            else if (delta > 0 || index > from)
                moved.emplace(uint32_t(int64_t(index) + delta), std::move(item));
        }
        items_ = std::move(moved);
    }

    std::vector<QueueRow> rows_;
    size_t first_ = 0; // the first listed row
    uint64_t current_ = 0;
    std::unordered_map<uint32_t, IInspectable> items_;
    winrt::event<winrt::Windows::Foundation::Collections::VectorChangedEventHandler<IInspectable>> changed_;
};

} // namespace ui
