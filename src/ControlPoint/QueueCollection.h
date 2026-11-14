// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The queue panel's ItemsSource. A queue can hold a whole 100k-track library, so items, and the text of their rows, are
// made only for the rows the list realizes (converting every entry took 100-280 ms on the UI thread at each change),
// and a new snapshot with the same entries refreshes those rows in place instead of resetting the view. Drag
// reordering moves rows through RemoveAt and InsertAt.
#pragma once

#include <algorithm>
#include <filesystem>
#include <vector>

#include "Common/util/Text.h"
#include "ItemsVector.h"
#include "QueueItem.h"
#include "Server/Api/ServerApi.h"
#include "UiText.h"

namespace ui {

class QueueCollection : public ItemsVector<QueueCollection> {
public:
    using Item = winrt::CamelPlayer::QueueItem;

    // The list shows at most this many rows, around the current entry: about 65k rows (millions of pixels) drew rows
    // past the list's bottom edge at 125 % scaling, and higher scaling lowers that limit.
    static constexpr size_t kShown = 25000;

    void update(camel::server::QueueEntries entries, uint64_t current) {
        std::vector<Slot> rows;
        rows.reserve(entries.size());
        for (const auto& entry : entries) rows.push_back({entry.id, &entry});
        size_t first = 0;
        if (rows.size() > kShown) {
            const auto at = std::find_if(rows.begin(), rows.end(), [&](const Slot& row) { return row.id == current; });
            const size_t index = at == rows.end() ? 0 : size_t(at - rows.begin());
            first = std::min(index > kShown / 2 ? index - kShown / 2 : 0, rows.size() - kShown);
        }
        const bool same = first == first_ && std::equal(rows.begin(), rows.end(), rows_.begin(), rows_.end(),
                                                        [](const Slot& a, const Slot& b) { return a.id == b.id; });
        rows_ = std::move(rows);
        entries_ = std::move(entries); // what the slots point into
        first_ = first;
        current_ = current;
        if (same) {
            for (auto& [index, item] : items_) fill(item, index);
            return;
        }
        items_.clear();
        notify(CollectionChange::Reset, 0u);
    }

    void setCurrent(uint64_t current) {
        current_ = current;
        for (auto& [index, item] : items_) fill(item, index);
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
            fill(item, index);
        }
        trim(index);
        return item;
    }

    // With more entries than listed rows, the row after the last listed one moves into the list (or out of it), so the
    // list keeps its size.
    void RemoveAt(uint32_t index) {
        if (index >= Size()) throw winrt::hresult_out_of_bounds();
        const auto before = Size();
        rows_.erase(rows_.begin() + ptrdiff_t(first_ + index));
        removed(index, before);
    }

    void InsertAt(uint32_t index, IInspectable const& value) {
        if (index > Size()) throw winrt::hresult_out_of_bounds();
        const auto before = Size();
        rows_.insert(rows_.begin() + ptrdiff_t(first_ + index), Slot{self(value)->row().id, self(value)->entry()});
        inserted(index, value, before);
    }

private:
    static winrt::CamelPlayer::implementation::QueueItem* self(IInspectable const& item) {
        return winrt::get_self<winrt::CamelPlayer::implementation::QueueItem>(item.as<Item>());
    }

    // A listed row: its entry's id and the entry in the snapshot entries_ holds.
    struct Slot {
        uint64_t id = 0;
        const camel::server::QueueEntry* entry = nullptr;
    };

    const Slot& row(uint32_t index) const { return rows_[first_ + index]; }

    static QueueRow rowOf(const camel::server::QueueEntry& entry) {
        const auto& path = entry.source.pathUtf8();
        return {entry.id,
                entry.title.empty() ? winrt::hstring(std::filesystem::path(camel::toWide(path)).stem().wstring())
                                    : utf8(entry.title),
                utf8(entry.artist),
                utf8(path),
                entry.automatic,
                entry.file,
                entry.favorite};
    }

    void fill(IInspectable const& item, uint32_t index) const {
        const auto& slot = row(index);
        self(item)->fill(rowOf(*slot.entry), slot.id == current_, slot.entry);
    }

    std::vector<Slot> rows_;
    camel::server::QueueEntries entries_;
    size_t first_ = 0; // the first listed row
    uint64_t current_ = 0;
};

} // namespace ui
