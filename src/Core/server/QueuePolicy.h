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
        history_.clear();
        current_.reset();
        order_ = entries_;
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

    std::optional<QueuePosition> select(uint64_t entryId) {
        if (std::find(entries_.begin(), entries_.end(), entryId) == entries_.end())
            throw std::invalid_argument("unknown queue entry");
        if (current_ && *current_ != entryId) history_.push_back(*current_);
        current_ = entryId;
        failed_.erase(entryId);
        return position(entryId);
    }

    std::optional<QueuePosition> next(bool naturalCompletion) {
        if (naturalCompletion && repeat_ == RepeatMode::One && current_ && !failed_.contains(*current_))
            return position(*current_);
        return advance(false);
    }

    std::optional<QueuePosition> previous(uint64_t positionFrames, uint64_t sampleRate) {
        if (!current_) return next(false);
        if (sampleRate && positionFrames > sampleRate * 3) return position(*current_);
        while (!history_.empty()) {
            const auto id = history_.back();
            history_.pop_back();
            if (contains(id) && !failed_.contains(id)) {
                current_ = id;
                return position(id);
            }
        }
        return position(*current_);
    }

    void markFailed(uint64_t entryId) { failed_.insert(entryId); }
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

    std::optional<QueuePosition> advance(bool newRound) {
        if (entries_.empty()) return std::nullopt;
        if (newRound && shuffle_) shuffleOrder();
        auto it = current_ ? std::find(order_.begin(), order_.end(), *current_) : order_.end();
        size_t index = it == order_.end() ? 0 : size_t(std::distance(order_.begin(), it)) + 1;
        for (size_t visited = 0; visited < order_.size(); ++visited) {
            if (index == order_.size()) {
                if (repeat_ != RepeatMode::All) return std::nullopt;
                if (shuffle_) shuffleOrder();
                index = 0;
            }
            const auto id = order_[index++];
            if (id == current_.value_or(0) && entries_.size() > 1) continue;
            if (failed_.contains(id)) continue;
            if (current_) history_.push_back(*current_);
            current_ = id;
            return position(id);
        }
        return std::nullopt;
    }

    std::vector<uint64_t> entries_, order_, history_;
    std::unordered_set<uint64_t> failed_;
    std::optional<uint64_t> current_;
    RepeatMode repeat_ = RepeatMode::Off;
    bool shuffle_ = false;
    uint64_t revision_ = 0;
    std::mt19937 random_;
};

} // namespace camel::server
