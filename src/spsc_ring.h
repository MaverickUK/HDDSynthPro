// Lock-free single-producer, single-consumer ring for passing items between
// cores. The producer only calls push(), the consumer only pop(). N must be a
// power of two. Indices run freely and wrap through unsigned overflow.
// Pure C++.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

template <typename T, size_t N>
class SpscRing {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "N must be a power of two");

public:
    // Producer side. Returns false (and drops the item) when full.
    // Always inlined: core 1 calls them, and an out-of-line copy would be in
    // flash (scripts/check_core1_ram.py).
    __attribute__((always_inline)) bool push(const T& item) {
        uint32_t head = head_.load(std::memory_order_relaxed);
        if (head - tail_.load(std::memory_order_acquire) >= N) return false;
        items_[head & (N - 1)] = item;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns false when empty.
    __attribute__((always_inline)) bool pop(T& item) {
        uint32_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        item = items_[tail & (N - 1)];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Safe from either side; the answer may be stale by the time it is used.
    size_t size() const {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }

private:
    T items_[N];
    std::atomic<uint32_t> head_{0};
    std::atomic<uint32_t> tail_{0};
};
