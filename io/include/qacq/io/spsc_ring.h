#pragma once

// Lock-free ring buffer for exactly one producer thread (the acquisition) and one consumer thread (the
// interface or the recorder). Neither side ever waits for the other: push() writes what fits and says
// how much, pop() reads what is there. Elements must be trivially copyable (samples, small structs).
//
// The producer owns m_head, the consumer owns m_tail; each publishes its index with a release store and
// reads the other's with an acquire load, so the data written before a publication is visible after it.
// Each side caches the other's index and reloads it only when the cached value says "full" or "empty".

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <type_traits>

namespace qacq::io
{
template <typename T>
class SpscRing
{
    static_assert(std::is_trivially_copyable_v<T>, "SpscRing holds trivially copyable elements");

public:
    // Capacity rounded up to a power of two (at least 2).
    explicit SpscRing(std::size_t capacity)
    {
        std::size_t c = 2;
        while (c < capacity) c <<= 1;
        m_capacity = c;
        m_mask = c - 1;
        m_data = std::make_unique<T[]>(c);
    }

    SpscRing(const SpscRing &) = delete;
    SpscRing &operator=(const SpscRing &) = delete;

    std::size_t capacity() const noexcept { return m_capacity; }

    // Producer side. Writes as many elements as fit, returns that count.
    std::size_t push(std::span<const T> items) noexcept
    {
        const std::size_t head = m_head.load(std::memory_order_relaxed);
        std::size_t free = m_capacity - (head - m_tailCache);
        if (free < items.size())
        {
            m_tailCache = m_tail.load(std::memory_order_acquire);
            free = m_capacity - (head - m_tailCache);
        }
        const std::size_t count = std::min(free, items.size());
        copyIn(head, items.first(count));
        m_head.store(head + count, std::memory_order_release);
        return count;
    }

    // Producer side. Writes everything or nothing (keeps frames of several channels whole).
    bool pushAll(std::span<const T> items) noexcept
    {
        const std::size_t head = m_head.load(std::memory_order_relaxed);
        if (m_capacity - (head - m_tailCache) < items.size())
        {
            m_tailCache = m_tail.load(std::memory_order_acquire);
            if (m_capacity - (head - m_tailCache) < items.size()) return false;
        }
        copyIn(head, items);
        m_head.store(head + items.size(), std::memory_order_release);
        return true;
    }

    // Consumer side. Reads up to out.size() elements, returns the count read.
    std::size_t pop(std::span<T> out) noexcept
    {
        const std::size_t tail = m_tail.load(std::memory_order_relaxed);
        std::size_t available = m_headCache - tail;
        if (available < out.size())
        {
            m_headCache = m_head.load(std::memory_order_acquire);
            available = m_headCache - tail;
        }
        const std::size_t count = std::min(available, out.size());
        copyOut(tail, out.first(count));
        m_tail.store(tail + count, std::memory_order_release);
        return count;
    }

    // Either side: a snapshot, exact only on the consumer side (the producer may add meanwhile).
    std::size_t size() const noexcept
    {
        return m_head.load(std::memory_order_acquire) - m_tail.load(std::memory_order_acquire);
    }
    bool empty() const noexcept { return size() == 0; }

private:
    void copyIn(std::size_t head, std::span<const T> items) noexcept
    {
        const std::size_t start = head & m_mask;
        const std::size_t first = std::min(items.size(), m_capacity - start);
        std::copy_n(items.data(), first, m_data.get() + start);
        std::copy_n(items.data() + first, items.size() - first, m_data.get());
    }

    void copyOut(std::size_t tail, std::span<T> out) noexcept
    {
        const std::size_t start = tail & m_mask;
        const std::size_t first = std::min(out.size(), m_capacity - start);
        std::copy_n(m_data.get() + start, first, out.data());
        std::copy_n(m_data.get(), out.size() - first, out.data() + first);
    }

    std::unique_ptr<T[]> m_data;
    std::size_t m_capacity = 0;
    std::size_t m_mask = 0;

    // Free-running indices, masked on access: unsigned differences stay right when they wrap.
    alignas(64) std::atomic<std::size_t> m_head{0}; // written by the producer
    std::size_t m_tailCache = 0;                   // producer's copy of m_tail
    alignas(64) std::atomic<std::size_t> m_tail{0}; // written by the consumer
    std::size_t m_headCache = 0;                   // consumer's copy of m_head
};
} // namespace qacq::io
