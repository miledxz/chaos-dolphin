#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "util/clock.hpp"

namespace chaos {

// "Wake session X's direction D at time T." The timer never holds data or
// touches sockets: chunks stay in the session's FIFO, and the owning worker
// flushes them when woken.
struct TimerEntry {
    Clock::time_point due;
    std::uint32_t worker = 0;
    std::uint64_t session = 0;
    Dir dir = Dir::Up;
};

class TimerService {
public:
    // Called on the timer thread with all entries for one worker that are due.
    using Dispatch = std::function<void(std::uint32_t worker, std::vector<TimerEntry> due)>;

    explicit TimerService(Dispatch dispatch);
    ~TimerService();

    void start();
    void stop();

    // Thread-safe.
    void schedule(const TimerEntry& entry);
    std::size_t pending() const;

private:
    struct Later {
        bool operator()(const TimerEntry& a, const TimerEntry& b) const { return a.due > b.due; }
    };

    void run();

    Dispatch dispatch_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::priority_queue<TimerEntry, std::vector<TimerEntry>, Later> heap_;
    bool stopping_ = false;
    std::thread thread_;
};

}  // namespace chaos
