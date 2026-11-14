// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// What the list pages' ItemsSources share: an observable vector whose items are made only for the rows the list
// realizes, kept around the reading position. `Derived` gives Size() and GetAt(), and RemoveAt and InsertAt where its
// rows can be dragged; the rest of the vector refuses changes from the list.
#pragma once

#include <unordered_map>

namespace ui {

template <class Derived>
class ItemsVector
    : public winrt::implements<
          Derived, winrt::Windows::Foundation::Collections::IObservableVector<winrt::Windows::Foundation::IInspectable>,
          winrt::Windows::Foundation::Collections::IVector<winrt::Windows::Foundation::IInspectable>,
          winrt::Windows::Foundation::Collections::IVectorView<winrt::Windows::Foundation::IInspectable>,
          winrt::Windows::Foundation::Collections::IIterable<winrt::Windows::Foundation::IInspectable>> {
public:
    using IInspectable = winrt::Windows::Foundation::IInspectable;
    using CollectionChange = winrt::Windows::Foundation::Collections::CollectionChange;

    uint32_t GetMany(uint32_t start, winrt::array_view<IInspectable> out) {
        uint32_t n = 0;
        for (; n < out.size() && start + n < derived().Size(); ++n) out[n] = derived().GetAt(start + n);
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

    winrt::Windows::Foundation::Collections::IVectorView<IInspectable> GetView() { return derived(); }
    winrt::Windows::Foundation::Collections::IIterator<IInspectable> First() { return winrt::make<Iterator>(derived()); }

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

protected:
    void notify(CollectionChange change, uint32_t index) { changed_(derived(), winrt::make<ChangeArgs>(change, index)); }

    // Keeps the items around the reading position so scrolling a 100k-row view does not keep 100k objects alive.
    void trim(uint32_t index) {
        if (items_.size() <= 4000) return;
        std::erase_if(items_, [index](const auto& pair) { return pair.first + 2000 < index || pair.first > index + 2000; });
    }

    // After a row at `index` left the rows, of `before` listed: with more rows than listed, the row after the last
    // listed one moves into the list, so the list keeps its size.
    void removed(uint32_t index, uint32_t before) {
        shift(index, -1);
        notify(CollectionChange::ItemRemoved, index);
        if (derived().Size() == before) notify(CollectionChange::ItemInserted, before - 1);
    }

    // After `value`'s row joined the rows at `index`, of `before` listed: the last listed row then leaves the list.
    void inserted(uint32_t index, IInspectable const& value, uint32_t before) {
        shift(index, 1);
        items_[index] = value;
        notify(CollectionChange::ItemInserted, index);
        if (derived().Size() == before) {
            items_.erase(before);
            notify(CollectionChange::ItemRemoved, before);
        }
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

    std::unordered_map<uint32_t, IInspectable> items_;

private:
    struct ChangeArgs : winrt::implements<ChangeArgs, winrt::Windows::Foundation::Collections::IVectorChangedEventArgs> {
        ChangeArgs(CollectionChange change, uint32_t index) : change_(change), index_(index) {}
        CollectionChange CollectionChange() const { return change_; }
        uint32_t Index() const { return index_; }
        winrt::Windows::Foundation::Collections::CollectionChange change_;
        uint32_t index_;
    };

    struct Iterator : winrt::implements<Iterator, winrt::Windows::Foundation::Collections::IIterator<IInspectable>> {
        explicit Iterator(Derived& owner) : owner_(owner.get_strong()) {}
        IInspectable Current() const { return owner_->GetAt(index_); }
        bool HasCurrent() const { return index_ < owner_->Size(); }
        bool MoveNext() { return ++index_ < owner_->Size(); }
        uint32_t GetMany(winrt::array_view<IInspectable> out) {
            const auto n = owner_->GetMany(index_, out);
            index_ += n;
            return n;
        }
        winrt::com_ptr<Derived> owner_;
        uint32_t index_ = 0;
    };

    Derived& derived() { return static_cast<Derived&>(*this); }
    const Derived& derived() const { return static_cast<const Derived&>(*this); }

    winrt::event<winrt::Windows::Foundation::Collections::VectorChangedEventHandler<IInspectable>> changed_;
};

} // namespace ui
