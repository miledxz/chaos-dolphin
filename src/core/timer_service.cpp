#include "core/timer_service.hpp"

#include <algorithm>

namespace chaos {

TimerService::TimerService(Dispatch dispatch) : dispatch_(std::move(dispatch)) {}

TimerService::~TimerService() { stop(); }

void TimerService::start() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = false;
    }
    thread_ = std::thread([this] { run(); });
}

void TimerService::stop() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_one();
    if (thread_.joinable()) thread_.join();
}

void TimerService::schedule(const TimerEntry& entry) {
    bool new_earliest;
    {
        std::lock_guard lock(mutex_);
        new_earliest = heap_.empty() || entry.due < heap_.top().due;
        heap_.push(entry);
    }
    // Only an entry earlier than the current head changes how long the timer
    // thread should sleep.
    if (new_earliest) cv_.notify_one();
}

std::size_t TimerService::pending() const {
    std::lock_guard lock(mutex_);
    return heap_.size();
}

void TimerService::run() {
    std::vector<std::vector<TimerEntry>> by_worker;
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        if (heap_.empty()) {
            cv_.wait(lock);
            continue;
        }
        auto now = Clock::now();
        if (heap_.top().due > now) {
            // Copy: wait_until reads its deadline after re-locking, by which time a
            // schedule() may have reallocated the heap under a reference to top().
            const Clock::time_point due = heap_.top().due;
            cv_.wait_until(lock, due);
            continue;
        }

        // Pop everything due and group it, so each worker gets one wakeup.
        while (!heap_.empty() && heap_.top().due <= now) {
            const TimerEntry& e = heap_.top();
            if (by_worker.size() <= e.worker) by_worker.resize(e.worker + 1);
            by_worker[e.worker].push_back(e);
            heap_.pop();
        }
        lock.unlock();
        for (std::uint32_t w = 0; w < by_worker.size(); ++w) {
            if (by_worker[w].empty()) continue;
            dispatch_(w, std::move(by_worker[w]));
            by_worker[w].clear();
        }
        lock.lock();
    }
}

}  // namespace chaos
