#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

#include "chaos/rules.hpp"

namespace chaos {

// Publishes immutable Rules snapshots.
//
// Readers check version() (one relaxed atomic load) per chunk and only take the
// mutex to grab a new snapshot when the version changed, so the hot path is
// lock-free in practice. (libc++ has no std::atomic<std::shared_ptr>, and where
// it exists it is not lock-free either.)
class RuleStore {
public:
    RuleStore() : current_(std::make_shared<const Rules>()) {}

    std::uint64_t version() const { return version_.load(std::memory_order_acquire); }

    std::shared_ptr<const Rules> snapshot() const {
        std::lock_guard lock(mutex_);
        return current_;
    }

    // Copies the current rules, lets `edit` change the copy, then publishes it.
    // If `edit` returns false (or throws) nothing is published.
    template <class F>
    bool update(F&& edit) {
        std::lock_guard lock(mutex_);
        auto next = std::make_shared<Rules>(*current_);
        if (!edit(*next)) return false;
        next->version = current_->version + 1;
        current_ = std::move(next);
        version_.store(current_->version, std::memory_order_release);
        return true;
    }

private:
    mutable std::mutex mutex_;
    std::shared_ptr<const Rules> current_;
    std::atomic<std::uint64_t> version_{0};
};

}  // namespace chaos
