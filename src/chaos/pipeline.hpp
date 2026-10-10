#pragma once

#include <memory>
#include <vector>

#include "chaos/rules.hpp"
#include "chaos/token_bucket.hpp"
#include "util/clock.hpp"

namespace chaos {

struct Chunk {
    std::vector<char> data;
    Clock::time_point release_at{};
};

enum class Verdict { Forward, Consume, Reset };

// Per-direction state a stage may read or update while processing one chunk.
struct StageContext {
    const Faults& faults;
    Rng& rng;
    TokenBucket& bucket;
    Clock::time_point now;
    bool stalled = false;  // set by StallStage, for stats
};

// One composable fault. Stages are stateless and shared by all sessions; any
// per-connection state lives in the StageContext. A stage only moves
// release_at later, consumes the chunk, or asks for a reset.
class Stage {
public:
    virtual ~Stage() = default;
    virtual Verdict apply(Chunk& chunk, StageContext& ctx) const = 0;
};

class ResetStage final : public Stage {
public:
    Verdict apply(Chunk& chunk, StageContext& ctx) const override;
};

class BlackholeStage final : public Stage {
public:
    Verdict apply(Chunk& chunk, StageContext& ctx) const override;
};

// Spaces chunks out to the configured byte rate.
class ThrottleStage final : public Stage {
public:
    Verdict apply(Chunk& chunk, StageContext& ctx) const override;
};

// Fixed latency plus uniform jitter.
class LatencyStage final : public Stage {
public:
    Verdict apply(Chunk& chunk, StageContext& ctx) const override;
};

// Simulated packet loss: the chunk is held for a retransmit-like timeout.
// Because each direction is a FIFO, later chunks wait behind it too, which is
// the head-of-line blocking a real lost segment causes.
class StallStage final : public Stage {
public:
    Verdict apply(Chunk& chunk, StageContext& ctx) const override;
};

class Pipeline {
public:
    void add(std::unique_ptr<Stage> stage) { stages_.push_back(std::move(stage)); }

    // Runs stages in order until one returns something other than Forward.
    Verdict run(Chunk& chunk, StageContext& ctx) const;

    // reset -> blackhole -> throttle -> latency/jitter -> stall
    static const Pipeline& standard();

private:
    std::vector<std::unique_ptr<Stage>> stages_;
};

}  // namespace chaos
