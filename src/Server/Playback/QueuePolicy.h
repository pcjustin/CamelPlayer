// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <random>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace camel::server {

enum class RepeatMode : uint8_t { Off, One, All };

struct QueuePosition {
    uint64_t entryId = 0;
    size_t index = 0;
    uint64_t revision = 0;
};

class QueuePolicy {
public:
    explicit QueuePolicy(uint32_t seed = std::random_device{}()) : random_(seed) {}

    uint64_t replace(std::vector<uint64_t> entryIds) {
        validateUnique(entryIds);
        entries_ = std::move(entryIds);
        failed_.clear();
        unavailable_.clear();
        history_.clear();
        current_.reset();
        order_ = entries_;
        if (shuffle_) shuffleOrder();
        ++revision_;
        return revision_;
    }

    uint64_t reorder(const std::vector<uint64_t>& entryIds) {
        if (entryIds.size() != entries_.size()) throw std::invalid_argument("reorder must contain every queue entry");
        validateUnique(entryIds);
        for (const auto id : entries_) {
            if (std::find(entryIds.begin(), entryIds.end(), id) == entryIds.end())
                throw std::invalid_argument("reorder contains an unknown queue entry");
        }
        entries_ = entryIds;
        order_ = entries_;
        if (shuffle_) shuffleOrder();
        ++revision_;
        return revision_;
    }

    void setShuffle(bool enabled) {
        if (shuffle_ == enabled) return;
        shuffle_ = enabled;
        order_ = entries_;
        if (shuffle_) shuffleOrder();
        ++revision_;
    }

    void setRepeat(RepeatMode mode) { repeat_ = mode; }

    std::optional<QueuePosition> current() const {
        if (!current_) return std::nullopt;
        return position(*current_);
    }

    QueuePosition select(uint64_t entryId, bool alreadyAudible = false) {
        if (std::find(entries_.begin(), entries_.end(), entryId) == entries_.end())
            throw std::invalid_argument("unknown queue entry");
        if (unavailable_.contains(entryId) && !alreadyAudible) throw std::invalid_argument("queue entry is unavailable");
        if (current_ && *current_ != entryId) remember(*current_);
        current_ = entryId;
        failed_.erase(entryId);
        return position(entryId);
    }

    std::optional<QueuePosition> next(bool naturalCompletion) {
        if (naturalCompletion && repeat_ == RepeatMode::One && current_ && eligible(*current_))
            return position(*current_);
        return advance();
    }

    std::optional<QueuePosition> previous(uint64_t positionFrames, uint64_t sampleRate) {
        if (!current_) return next(false);
        if (sampleRate && positionFrames > sampleRate * 3 && eligible(*current_)) return position(*current_);
        while (!history_.empty()) {
            const auto id = history_.back();
            history_.pop_back();
            if (contains(id) && eligible(id)) {
                current_ = id;
                return position(id);
            }
        }
        return eligible(*current_) ? std::optional(position(*current_)) : std::nullopt;
    }

    // The entry next(true) would choose, without changing any state; this is the preload target.
    std::optional<QueuePosition> upcoming() const {
        QueuePolicy copy = *this;
        return copy.next(true);
    }

    void markFailed(uint64_t entryId) { failed_.insert(entryId); }
    bool setUnavailable(uint64_t entryId, bool unavailable) {
        const bool changed = unavailable ? unavailable_.insert(entryId).second : unavailable_.erase(entryId) != 0;
        if (changed) ++revision_;
        return changed;
    }
    bool unavailable(uint64_t entryId) const { return unavailable_.contains(entryId); }
    bool shuffle() const { return shuffle_; }
    RepeatMode repeat() const { return repeat_; }
    uint64_t revision() const { return revision_; }
    bool isCurrent(const QueuePosition& prepared) const {
        return prepared.revision == revision_ && current_ == prepared.entryId && contains(prepared.entryId);
    }

private:
    static void validateUnique(const std::vector<uint64_t>& ids) {
        std::unordered_set<uint64_t> unique(ids.begin(), ids.end());
        if (unique.size() != ids.size() || unique.contains(0))
            throw std::invalid_argument("queue entry IDs must be unique and nonzero");
    }

    bool contains(uint64_t id) const { return std::find(entries_.begin(), entries_.end(), id) != entries_.end(); }
    bool eligible(uint64_t id) const { return !failed_.contains(id) && !unavailable_.contains(id); }

    QueuePosition position(uint64_t id) const {
        const auto it = std::find(entries_.begin(), entries_.end(), id);
        return {id, size_t(std::distance(entries_.begin(), it)), revision_};
    }

    void shuffleOrder() {
        if (current_ && order_.size() > 1) {
            const auto it = std::find(order_.begin(), order_.end(), *current_);
            if (it != order_.end()) order_.erase(it);
            std::shuffle(order_.begin(), order_.end(), random_);
            if (!order_.empty() && order_.front() == *current_) std::swap(order_.front(), order_.back());
            order_.insert(order_.begin(), *current_);
        } else {
            std::shuffle(order_.begin(), order_.end(), random_);
        }
    }

    // A new shuffle round visits every entry; only its first entry must differ from the one just played.
    void shuffleNewRound() {
        order_ = entries_;
        std::shuffle(order_.begin(), order_.end(), random_);
        if (current_ && order_.size() > 1 && order_.front() == *current_) std::swap(order_.front(), order_.back());
    }

    size_t eligibleCount() const {
        return size_t(std::count_if(entries_.begin(), entries_.end(), [&](uint64_t id) { return eligible(id); }));
    }

    void remember(uint64_t id) {
        history_.push_back(id);
        if (history_.size() > kMaxHistory) history_.erase(history_.begin());
    }

    std::optional<QueuePosition> advance() {
        if (entries_.empty()) return std::nullopt;
        const bool avoidRepeat = eligibleCount() > 1;
        auto it = current_ ? std::find(order_.begin(), order_.end(), *current_) : order_.end();
        size_t index = it == order_.end() ? 0 : size_t(std::distance(order_.begin(), it)) + 1;
        bool wrapped = false;
        for (;;) {
            if (index == order_.size()) {
                if (wrapped || repeat_ != RepeatMode::All) return std::nullopt;
                wrapped = true;
                if (shuffle_) shuffleNewRound();
                index = 0;
            }
            const auto id = order_[index++];
            if (avoidRepeat && id == current_.value_or(0)) continue;
            if (!eligible(id)) continue;
            if (current_) remember(*current_);
            current_ = id;
            return position(id);
        }
    }

    static constexpr size_t kMaxHistory = 1024;

    std::vector<uint64_t> entries_, order_, history_;
    std::unordered_set<uint64_t> failed_;
    std::unordered_set<uint64_t> unavailable_;
    std::optional<uint64_t> current_;
    RepeatMode repeat_ = RepeatMode::Off;
    bool shuffle_ = false;
    uint64_t revision_ = 0;
    std::mt19937 random_;
};

} // namespace camel::server
