#pragma once

#include <algorithm>
#include <cstdint>

#include "util/clock.hpp"

namespace chaos {

// Token bucket that can go into debt: reserve() always succeeds and returns the
// time at which the reserved bytes are allowed to leave. Back-to-back
// reservations therefore space chunks out at exactly `rate` bytes per second.
class TokenBucket {
public:
    std::uint64_t rate() const { return rate_; }

    // Changing the rate starts a fresh bucket (full burst available).
    void set_rate(std::uint64_t bytes_per_sec, Clock::time_point now) {
        if (bytes_per_sec == rate_) return;
        rate_ = bytes_per_sec;
        burst_ = std::max<double>(1024.0, rate_ * 0.05);  // ~50ms worth of bytes
        tokens_ = burst_;
        last_ = now;
    }

    Clock::time_point reserve(std::size_t bytes, Clock::time_point now) {
        if (rate_ == 0) return now;
        if (now > last_) {
            double elapsed = std::chrono::duration<double>(now - last_).count();
            tokens_ = std::min(burst_, tokens_ + elapsed * rate_);
            last_ = now;
        }
        tokens_ -= static_cast<double>(bytes);
        if (tokens_ >= 0) return now;
        auto wait = std::chrono::duration<double>(-tokens_ / rate_);
        return now + std::chrono::duration_cast<Duration>(wait);
    }

private:
    std::uint64_t rate_ = 0;
    double burst_ = 0;
    double tokens_ = 0;
    Clock::time_point last_{};
};

}  // namespace chaos
