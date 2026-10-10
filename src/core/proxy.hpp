#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "chaos/rule_store.hpp"
#include "core/stats.hpp"
#include "core/timer_service.hpp"

namespace chaos {

class Acceptor;
class ControlServer;
class CommandProcessor;
class WorkerPool;
struct ProxyContext;

struct Config {
    std::string listen = "127.0.0.1:8000";
    std::string upstream;
    std::string control = "127.0.0.1:9100";
    unsigned workers = 0;                 // 0 = hardware concurrency
    std::optional<std::uint64_t> seed;    // random if unset
    std::vector<std::string> rule_commands;  // e.g. "set latency 100ms", applied at startup
};

// Owns all the threads: acceptor, worker pool, timer and control.
class Proxy {
public:
    explicit Proxy(const Config& config);
    ~Proxy();
    Proxy(const Proxy&) = delete;
    Proxy& operator=(const Proxy&) = delete;

    void start();
    void stop();

    std::uint16_t listen_port() const;
    std::uint16_t control_port() const;
    std::uint64_t seed() const { return seed_; }
    unsigned workers() const { return workers_; }

    // Runs a control command in-process (same as sending it to the control port).
    std::string execute(std::string_view command);

    RuleStore& rules() { return rules_; }
    const Stats& stats() const { return stats_; }

private:
    std::uint64_t seed_;
    unsigned workers_;
    RuleStore rules_;
    Stats stats_;
    TimerService timer_;
    std::unique_ptr<ProxyContext> ctx_;
    std::unique_ptr<WorkerPool> pool_;
    std::unique_ptr<CommandProcessor> commands_;
    std::unique_ptr<Acceptor> acceptor_;
    std::unique_ptr<ControlServer> control_;
    bool running_ = false;
};

}  // namespace chaos
