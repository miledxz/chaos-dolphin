#include <condition_variable>
#include <mutex>

#include "core/timer_service.hpp"
#include "test.hpp"

using namespace chaos;
using namespace std::chrono_literals;

TEST_CASE("timer fires entries in deadline order, grouped per worker") {
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> fired;
    std::vector<Clock::time_point> fired_at;

    TimerService timer([&](std::uint32_t worker, std::vector<TimerEntry> due) {
        std::lock_guard lock(m);
        for (auto& e : due) {
            fired.emplace_back(worker, e.session);
            fired_at.push_back(Clock::now());
        }
        cv.notify_all();
    });
    timer.start();

    auto t0 = Clock::now();
    timer.schedule({t0 + 120ms, 1, 3, Dir::Up});
    timer.schedule({t0 + 80ms, 0, 2, Dir::Down});
    // Earlier than the current head: the timer thread must re-sleep for it.
    timer.schedule({t0 + 20ms, 0, 1, Dir::Up});

    std::unique_lock lock(m);
    CHECK(cv.wait_for(lock, 2s, [&] { return fired.size() == 3; }));
    CHECK_EQ(fired[0].second, 1u);
    CHECK_EQ(fired[1].second, 2u);
    CHECK_EQ(fired[2].second, 3u);
    CHECK_EQ(fired[2].first, 1u);
    CHECK(fired_at[0] >= t0 + 20ms);
    CHECK(fired_at[2] >= t0 + 120ms);
    CHECK(fired_at[0] < t0 + 80ms);  // didn't wait for the later entries
    lock.unlock();
    timer.stop();
    CHECK_EQ(timer.pending(), 0u);
}

TEST_CASE("timer stop with pending entries returns promptly") {
    TimerService timer([](std::uint32_t, std::vector<TimerEntry>) {});
    timer.start();
    timer.schedule({Clock::now() + 1h, 0, 1, Dir::Up});
    auto t0 = Clock::now();
    timer.stop();
    CHECK(Clock::now() - t0 < 500ms);
}
