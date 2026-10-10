#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace chaos {

// Process-wide counters, updated with relaxed atomics from worker threads.
struct Stats {
    std::atomic<std::uint64_t> conns_active{0};
    std::atomic<std::uint64_t> conns_total{0};
    std::atomic<std::uint64_t> connect_failures{0};
    std::atomic<std::uint64_t> bytes_up{0};
    std::atomic<std::uint64_t> bytes_down{0};
    std::atomic<std::uint64_t> chunks{0};
    std::atomic<std::uint64_t> chunks_delayed{0};
    std::atomic<std::uint64_t> chunks_stalled{0};
    std::atomic<std::uint64_t> chunks_blackholed{0};
    std::atomic<std::uint64_t> resets_injected{0};

    static void add(std::atomic<std::uint64_t>& counter, std::uint64_t n = 1) {
        counter.fetch_add(n, std::memory_order_relaxed);
    }
    static std::uint64_t get(const std::atomic<std::uint64_t>& counter) {
        return counter.load(std::memory_order_relaxed);
    }

    std::string line() const;  // one-line summary for the live stats output
};

}  // namespace chaos
