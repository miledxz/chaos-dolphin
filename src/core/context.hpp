#pragma once

#include <cstdint>

#include "chaos/rule_store.hpp"
#include "core/stats.hpp"
#include "core/timer_service.hpp"
#include "net/socket.hpp"

namespace chaos {

// Shared, thread-safe services every worker and session can reach.
struct ProxyContext {
    net::Address upstream;
    RuleStore& rules;
    TimerService& timer;
    Stats& stats;
    std::uint64_t seed = 0;
};

}  // namespace chaos
