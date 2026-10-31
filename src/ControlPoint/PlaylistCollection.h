// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The playlist page's ItemsSource. Every entry is held as a row and items are made only for the rows the list realizes;
// drag reordering moves rows through RemoveAt and InsertAt.
#pragma once

#include <algorithm>
#include <ranges>
#include <set>
#include <unordered_map>
#include <vector>

#include "BrowseItem.h"

namespace ui {

class PlaylistCollection
    : public winrt::implements<
          PlaylistCollection,
          winrt::Windows::Foundation::Collections::IObservableVector<winrt::Windows::Foundation::IInspectable>,
          winrt::Windows::Foundation::Collections::IVector<winrt::Windows::Foundation::IInspectable>,
          winrt::Windows::Foundation::Collections::IVectorView<winrt::Windows::Foundation::IInspectable>,
          winrt::Windows::Foundation::Collections::IIterable<winrt::Windows::Foundation::IInspectable>> {
public:
    using IInspectable = winrt::Windows::Foundation::IInspectable;
    using Item = winrt::CamelPlayer::BrowseItem;
    using CollectionChange = winrt::Windows::Foundation::Collections::CollectionChange;

    // The same limit as the queue panel (QueueCollection::kShown): longer playlists list their first entries.
    static constexpr size_t kShown = 25000;

    void reset(std::vector<BrowseRow> rows) {
        rows_ = std::move(rows);
        items_.clear();
        changed_(*this, winrt::make<ChangeArgs>(CollectionChange::Reset, 0u));
    }

    void remove(const std::set<int64_t>& ids) {
        std::vector<uint32_t> indices;
        for (uint32_t i = 0; i < Size(); ++i)
            if (ids.contains(rows_[i].id)) indices.push_back(i);
        if (rows_.size() > kShown || indices.size() > 200) {
            std::erase_if(rows_, [&](const BrowseRow& row) { return ids.contains(row.id); });
            reset(std::move(rows_));
            return;
        }
        for (const uint32_t index : std::views::reverse(indices)) {
            rows_.erase(rows_.begin() + index);
            shift(index, -1);
            changed_(*this, winrt::make<ChangeArgs>(CollectionChange::ItemRemoved, index));
        }
    }

    // A favorite mark set on these files, in every entry that plays one of them.
    void mark(const std::vector<int64_t>& files, bool favorite) {
        for (size_t i = 0; i < rows_.size(); ++i) {
            auto& row = rows_[i];
            if (!row.file || std::ranges::find(files, row.file) == files.end()) continue;
            row.favorite = favorite;
            if (const auto found = items_.find(uint32_t(i)); found != items_.end()) self(found->second)->setFavorite(favorite);
        }
    }

    const std::vector<BrowseRow>& rows() const { return rows_; }
    static const BrowseRow& rowOf(IInspectable const& item) { return self(item)->row(); }

    uint32_t Size() const { return uint32_t(std::min(rows_.size(), kShown)); }

    IInspectable GetAt(uint32_t index) {
        if (index >= Size()) throw winrt::hresult_out_of_bounds();
        auto& item = items_[index];
        if (!item) {
            item = winrt::make<winrt::CamelPlayer::implementation::BrowseItem>();
            self(item)->fill(rows_[index]);
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

    // The rows made so far; a row made later reads its state then.
    template <class F> void forEachItem(F&& f) const {
        for (const auto& [index, item] : items_)
            if (item) f(item);
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

    void RemoveAt(uint32_t index) {
        if (index >= Size()) throw winrt::hresult_out_of_bounds();
        const auto before = Size();
        rows_.erase(rows_.begin() + index);
        shift(index, -1);
        changed_(*this, winrt::make<ChangeArgs>(CollectionChange::ItemRemoved, index));
        if (Size() == before) changed_(*this, winrt::make<ChangeArgs>(CollectionChange::ItemInserted, before - 1));
    }

    void InsertAt(uint32_t index, IInspectable const& value) {
        if (index > Size()) throw winrt::hresult_out_of_bounds();
        const auto before = Size();
        rows_.insert(rows_.begin() + index, self(value)->row());
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
        explicit Iterator(PlaylistCollection& owner) : owner_(owner.get_strong()) {}
        IInspectable Current() const { return owner_->GetAt(index_); }
        bool HasCurrent() const { return index_ < owner_->Size(); }
        bool MoveNext() { return ++index_ < owner_->Size(); }
        uint32_t GetMany(winrt::array_view<IInspectable> out) {
            const auto n = owner_->GetMany(index_, out);
            index_ += n;
            return n;
        }
        winrt::com_ptr<PlaylistCollection> owner_;
        uint32_t index_ = 0;
    };

    static winrt::CamelPlayer::implementation::BrowseItem* self(IInspectable const& item) {
        return winrt::get_self<winrt::CamelPlayer::implementation::BrowseItem>(item.as<Item>());
    }

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

    std::vector<BrowseRow> rows_;
    std::unordered_map<uint32_t, IInspectable> items_;
    winrt::event<winrt::Windows::Foundation::Collections::VectorChangedEventHandler<IInspectable>> changed_;
};

} // namespace ui
