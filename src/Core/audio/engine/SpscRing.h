// Single-producer / single-consumer lock-free byte ring (docs/計劃書.md §5.1).
// The decode thread writes packed device bytes; the output's real-time thread reads them.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace camel::audio {

#pragma warning(push)
#pragma warning(disable : 4324)  // padding from alignas: intended, keeps head and tail on separate cache lines
class SpscRing {
public:
    explicit SpscRing(size_t capacity = 0) { reset(capacity); }

    // Not thread-safe: call only while neither side is running.
    void reset(size_t capacity) {
        buf_.assign(capacity, 0);
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }
    void clear() {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

    size_t capacity() const { return buf_.size(); }
    size_t readable() const { return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire); }
    size_t writable() const { return capacity() - readable(); }

    // Producer side.
    size_t write(const void* src, size_t n) {
        const size_t head = head_.load(std::memory_order_relaxed);
        n = std::min(n, capacity() - (head - tail_.load(std::memory_order_acquire)));
        copyIn(head, static_cast<const uint8_t*>(src), n);
        head_.store(head + n, std::memory_order_release);
        return n;
    }

    // Consumer side; safe on a real-time thread (no allocation, no locks).
    size_t read(void* dst, size_t n) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        n = std::min(n, head_.load(std::memory_order_acquire) - tail);
        copyOut(tail, static_cast<uint8_t*>(dst), n);
        tail_.store(tail + n, std::memory_order_release);
        return n;
    }

private:
    void copyIn(size_t pos, const uint8_t* src, size_t n) {
        if (!n) return;
        size_t at = pos % capacity(), first = std::min(n, capacity() - at);
        std::memcpy(buf_.data() + at, src, first);
        std::memcpy(buf_.data(), src + first, n - first);
    }
    void copyOut(size_t pos, uint8_t* dst, size_t n) const {
        if (!n) return;
        size_t at = pos % capacity(), first = std::min(n, capacity() - at);
        std::memcpy(dst, buf_.data() + at, first);
        std::memcpy(dst + first, buf_.data(), n - first);
    }

    std::vector<uint8_t> buf_;
    alignas(64) std::atomic<size_t> head_{0};  // total bytes written
    alignas(64) std::atomic<size_t> tail_{0};  // total bytes read
};
#pragma warning(pop)

}  // namespace camel::audio
