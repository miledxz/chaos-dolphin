#include "core/worker.hpp"

#include <array>

#include "util/log.hpp"

namespace chaos {

Worker::Worker(std::uint32_t index, ProxyContext& ctx) : index_(index), ctx_(ctx) {}

Worker::~Worker() { stop(); }

void Worker::start() {
    stopping_ = false;
    thread_ = std::thread([this] { run(); });
}

void Worker::stop() {
    if (!thread_.joinable()) return;
    stopping_ = true;
    poller_.wakeup();
    thread_.join();
}

void Worker::post(std::function<void()> task) {
    bool was_empty;
    {
        std::lock_guard lock(inbox_mutex_);
        was_empty = inbox_.empty();
        inbox_.push_back(std::move(task));
    }
    // A non-empty inbox means a wakeup is already pending.
    if (was_empty) poller_.wakeup();
}

void Worker::run() {
    std::array<net::PollEvent, 128> events;
    while (!stopping_.load(std::memory_order_relaxed)) {
        int n;
        try {
            n = poller_.wait(events, -1);
        } catch (const std::exception& e) {
            log::error("worker {}: {}", index_, e.what());
            break;
        }
        for (int i = 0; i < n; ++i) {
            const auto& ev = events[i];
            if (ev.token == net::Poller::kWakeToken) continue;
            std::uint64_t id = ev.token >> 1;
            auto it = sessions_.find(id);
            if (it == sessions_.end()) continue;  // closed earlier in this batch
            try {
                it->second->on_event(static_cast<int>(ev.token & 1), ev.readable, ev.writable);
            } catch (const std::exception& e) {
                it->second->kill(true, e.what());
            }
            if (it->second->closed()) sessions_.erase(it);
        }
        run_tasks();
    }
    sessions_.clear();  // closes every connection
    std::lock_guard lock(inbox_mutex_);
    inbox_.clear();
}

void Worker::run_tasks() {
    std::vector<std::function<void()>> tasks;
    {
        std::lock_guard lock(inbox_mutex_);
        tasks.swap(inbox_);
    }
    for (auto& task : tasks) task();
}

void Worker::reap(std::uint64_t id) {
    auto it = sessions_.find(id);
    if (it != sessions_.end() && it->second->closed()) sessions_.erase(it);
}

void Worker::adopt(std::uint64_t id, net::Fd client, const net::Address& peer) {
    auto [it, inserted] = sessions_.emplace(id, std::make_unique<Session>(*this, id, std::move(client), peer));
    it->second->start();
    reap(id);
}

void Worker::on_timers(const std::vector<TimerEntry>& due) {
    for (const auto& e : due) {
        auto it = sessions_.find(e.session);
        if (it == sessions_.end()) continue;  // session closed since the timer was set
        it->second->on_timer(e.dir, e.due);
        reap(e.session);
    }
}

bool Worker::kill(std::uint64_t id, bool rst) {
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return false;
    it->second->kill(rst, "killed by control");
    sessions_.erase(it);
    return true;
}

std::vector<ConnSnapshot> Worker::snapshot() const {
    std::vector<ConnSnapshot> out;
    out.reserve(sessions_.size());
    for (const auto& [id, s] : sessions_) out.push_back(s->snapshot());
    return out;
}

// ---------------------------------------------------------------- WorkerPool

WorkerPool::WorkerPool(std::size_t count, ProxyContext& ctx) {
    for (std::size_t i = 0; i < count; ++i)
        workers_.push_back(std::make_unique<Worker>(static_cast<std::uint32_t>(i), ctx));
}

void WorkerPool::start() {
    for (auto& w : workers_) w->start();
}

void WorkerPool::stop() {
    for (auto& w : workers_) w->stop();
}

void WorkerPool::assign(net::Fd client, const net::Address& peer) {
    std::uint64_t id = next_id_.fetch_add(1, std::memory_order_relaxed);
    Worker& w = *workers_[id % workers_.size()];
    // std::function needs a copyable callable, so the Fd travels in a shared_ptr.
    auto fd = std::make_shared<net::Fd>(std::move(client));
    w.post([&w, id, fd, peer] { w.adopt(id, std::move(*fd), peer); });
}

}  // namespace chaos
