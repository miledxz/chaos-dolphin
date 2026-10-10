#pragma once

#include <string>
#include <string_view>

#include "chaos/rule_store.hpp"
#include "core/stats.hpp"
#include "core/timer_service.hpp"
#include "core/worker.hpp"

namespace chaos {

// Executes one control command line and returns the reply text (always
// newline-terminated). Thread-safe; called from the control thread.
class CommandProcessor {
public:
    CommandProcessor(RuleStore& rules, WorkerPool& pool, Stats& stats, TimerService& timer)
        : rules_(rules), pool_(pool), stats_(stats), timer_(timer) {}

    std::string execute(std::string_view line);

    static std::string help();

private:
    std::string cmd_conns();
    std::string cmd_kill(std::uint64_t id, bool rst);

    RuleStore& rules_;
    WorkerPool& pool_;
    Stats& stats_;
    TimerService& timer_;
};

}  // namespace chaos
