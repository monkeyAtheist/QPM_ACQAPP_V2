#pragma once

// A long reading followed from another thread (the import of a text file of hundreds of megabytes): it
// stops early on request, and says how far it is in thousandths. Both pointers may be null.

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace qacq::model
{
struct Progress
{
    const std::atomic<bool> *cancelled = nullptr;
    std::atomic<int> *permille = nullptr;

    bool stopped() const noexcept { return cancelled && cancelled->load(std::memory_order_relaxed); }
    // `done` out of `total` (nothing said when total is 0).
    void report(std::uint64_t done, std::uint64_t total) const noexcept
    {
        if (permille && total > 0)
            permille->store(static_cast<int>(std::min<std::uint64_t>(1000, done * 1000 / total)), std::memory_order_relaxed);
    }
};
} // namespace qacq::model
