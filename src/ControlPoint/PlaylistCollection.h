// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The playlist page's ItemsSource. Every entry is held as a row and items are made only for the rows the list realizes;
// drag reordering moves rows through RemoveAt and InsertAt.
#pragma once

#include <algorithm>
#include <ranges>
#include <set>
#include <vector>

#include "BrowseItem.h"
#include "ItemsVector.h"

namespace ui {

class PlaylistCollection : public ItemsVector<PlaylistCollection> {
public:
    using Item = winrt::CamelPlayer::BrowseItem;

    // The same limit as the queue panel (QueueCollection::kShown): longer playlists list their first entries.
    static constexpr size_t kShown = 25000;

    void reset(std::vector<BrowseRow> rows) {
        rows_ = std::move(rows);
        items_.clear();
        notify(CollectionChange::Reset, 0u);
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
            notify(CollectionChange::ItemRemoved, index);
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
        trim(index);
        return item;
    }

    void RemoveAt(uint32_t index) {
        if (index >= Size()) throw winrt::hresult_out_of_bounds();
        const auto before = Size();
        rows_.erase(rows_.begin() + index);
        removed(index, before);
    }

    void InsertAt(uint32_t index, IInspectable const& value) {
        if (index > Size()) throw winrt::hresult_out_of_bounds();
        const auto before = Size();
        rows_.insert(rows_.begin() + index, self(value)->row());
        inserted(index, value, before);
    }

private:
    static winrt::CamelPlayer::implementation::BrowseItem* self(IInspectable const& item) {
        return winrt::get_self<winrt::CamelPlayer::implementation::BrowseItem>(item.as<Item>());
    }

    std::vector<BrowseRow> rows_;
};

} // namespace ui
