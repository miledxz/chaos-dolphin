#include "chaos/pipeline.hpp"
#include "chaos/token_bucket.hpp"
#include "test.hpp"

using namespace chaos;
using namespace std::chrono_literals;

namespace {
long long ms(Duration d) { return std::chrono::duration_cast<Millis>(d).count(); }

struct Harness {
    Faults faults;
    Rng rng{42};
    TokenBucket bucket;
    Clock::time_point now = Clock::time_point{} + 1000s;  // fake clock

    std::pair<Verdict, Chunk> run(std::size_t bytes) {
        Chunk c{std::vector<char>(bytes), now};
        StageContext ctx{faults, rng, bucket, now};
        Verdict v = Pipeline::standard().run(c, ctx);
        return {v, std::move(c)};
    }
};
}  // namespace

TEST_CASE("no faults forwards immediately") {
    Harness h;
    auto [v, c] = h.run(100);
    CHECK(v == Verdict::Forward);
    CHECK(c.release_at == h.now);
}

TEST_CASE("latency and jitter stay within bounds") {
    Harness h;
    h.faults.latency = 200ms;
    h.faults.jitter = 50ms;
    bool saw_low = false, saw_high = false;
    for (int i = 0; i < 2000; ++i) {
        auto [v, c] = h.run(10);
        auto d = ms(c.release_at - h.now);
        CHECK(d >= 150 && d <= 250);
        saw_low |= d < 175;
        saw_high |= d > 225;
    }
    CHECK(saw_low && saw_high);
}

TEST_CASE("jitter larger than latency never goes negative") {
    Harness h;
    h.faults.latency = 10ms;
    h.faults.jitter = 100ms;
    for (int i = 0; i < 500; ++i) CHECK(h.run(10).second.release_at >= h.now);
}

TEST_CASE("drop stalls roughly the configured fraction") {
    Harness h;
    h.faults.drop = 0.25;
    h.faults.stall = 300ms;
    int stalled = 0;
    for (int i = 0; i < 4000; ++i) {
        auto [v, c] = h.run(10);
        CHECK(v == Verdict::Forward);
        if (c.release_at > h.now) {
            CHECK_EQ(ms(c.release_at - h.now), 300);
            ++stalled;
        }
    }
    CHECK(stalled > 850 && stalled < 1150);
}

TEST_CASE("blackhole consumes and reset wins over blackhole") {
    Harness h;
    h.faults.blackhole = true;
    CHECK(h.run(10).first == Verdict::Consume);
    h.faults.reset = 1.0;
    CHECK(h.run(10).first == Verdict::Reset);
}

TEST_CASE("same seed gives the same decisions") {
    Harness a, b;
    a.faults.drop = b.faults.drop = 0.5;
    a.faults.jitter = b.faults.jitter = 40ms;
    for (int i = 0; i < 200; ++i) CHECK(a.run(10).second.release_at == b.run(10).second.release_at);
}

TEST_CASE("token bucket spaces chunks at the configured rate") {
    TokenBucket bucket;
    auto t0 = Clock::time_point{} + 50s;
    bucket.set_rate(10'000, t0);  // 10 KB/s, burst = 1024 bytes
    Clock::time_point last{};
    // 100 KB reserved all at once must take ~10s minus the initial burst.
    for (int i = 0; i < 100; ++i) last = bucket.reserve(1000, t0);
    auto total = ms(last - t0);
    CHECK(total >= 9800 && total <= 10000);
}

TEST_CASE("token bucket refills while idle, capped at burst") {
    TokenBucket bucket;
    auto t0 = Clock::time_point{} + 50s;
    bucket.set_rate(100'000, t0);  // burst = 5000 bytes
    CHECK(bucket.reserve(5000, t0) == t0);
    CHECK(bucket.reserve(1000, t0) > t0);
    auto later = t0 + 10s;  // long idle: refills to burst only
    CHECK(bucket.reserve(5000, later) == later);
    CHECK(bucket.reserve(100, later) > later);
}

TEST_CASE("throttle stage uses the bucket") {
    Harness h;
    h.faults.throttle_Bps = 1000;
    h.run(1024);  // burst
    auto [v, c] = h.run(500);
    CHECK_EQ(ms(c.release_at - h.now), 500);
}
