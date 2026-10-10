#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/context.hpp"
#include "core/session.hpp"
#include "core/timer_service.hpp"
#include "net/poller.hpp"

namespace chaos {

// One event-loop thread that owns a set of sessions. Other threads talk to it
// only through post(), which queues a task and wakes the poller.
class Worker {
public:
    Worker(std::uint32_t index, ProxyContext& ctx);
    ~Worker();

    void start();
    void stop();  // closes all sessions and joins

    // Thread-safe. Runs `task` on this worker's thread.
    void post(std::function<void()> task);

    // --- worker thread only ---
    std::uint32_t index() const { return index_; }
    ProxyContext& ctx() { return ctx_; }
    net::Poller& poller() { return poller_; }

    void adopt(std::uint64_t id, net::Fd client, const net::Address& peer);
    void on_timers(const std::vector<TimerEntry>& due);
    bool kill(std::uint64_t id, bool rst);
    std::vector<ConnSnapshot> snapshot() const;

private:
    void run();
    void run_tasks();
    void reap(std::uint64_t id);

    std::uint32_t index_;
    ProxyContext& ctx_;
    net::Poller poller_;
    std::unordered_map<std::uint64_t, std::unique_ptr<Session>> sessions_;

    std::mutex inbox_mutex_;
    std::vector<std::function<void()>> inbox_;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};

// Fixed set of workers; new connections are spread round-robin.
class WorkerPool {
public:
    WorkerPool(std::size_t count, ProxyContext& ctx);

    void start();
    void stop();

    // Thread-safe. Hands an accepted client socket to a worker.
    void assign(net::Fd client, const net::Address& peer);

    std::size_t size() const { return workers_.size(); }
    Worker& at(std::size_t i) { return *workers_[i]; }

private:
    std::vector<std::unique_ptr<Worker>> workers_;
    std::atomic<std::uint64_t> next_id_{1};
};

}  // namespace chaos
