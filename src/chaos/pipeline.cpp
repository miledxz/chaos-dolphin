#include "chaos/pipeline.hpp"

#include <algorithm>
#include <random>

namespace chaos {

namespace {
bool chance(Rng& rng, double p) {
    if (p <= 0) return false;
    if (p >= 1) return true;
    return std::uniform_real_distribution<double>(0.0, 1.0)(rng) < p;
}
}  // namespace

Verdict ResetStage::apply(Chunk&, StageContext& ctx) const {
    return chance(ctx.rng, ctx.faults.reset) ? Verdict::Reset : Verdict::Forward;
}

Verdict BlackholeStage::apply(Chunk&, StageContext& ctx) const {
    return ctx.faults.blackhole ? Verdict::Consume : Verdict::Forward;
}

Verdict ThrottleStage::apply(Chunk& chunk, StageContext& ctx) const {
    ctx.bucket.set_rate(ctx.faults.throttle_Bps, ctx.now);
    if (ctx.faults.throttle_Bps == 0) return Verdict::Forward;
    chunk.release_at = std::max(chunk.release_at, ctx.bucket.reserve(chunk.data.size(), ctx.now));
    return Verdict::Forward;
}

Verdict LatencyStage::apply(Chunk& chunk, StageContext& ctx) const {
    const auto& f = ctx.faults;
    if (f.latency.count() == 0 && f.jitter.count() == 0) return Verdict::Forward;
    auto delay = f.latency;
    if (f.jitter.count() > 0) {
        std::uniform_int_distribution<Duration::rep> dist(-f.jitter.count(), f.jitter.count());
        delay += Duration(dist(ctx.rng));
    }
    if (delay.count() > 0) chunk.release_at += delay;
    return Verdict::Forward;
}

Verdict StallStage::apply(Chunk& chunk, StageContext& ctx) const {
    if (chance(ctx.rng, ctx.faults.drop)) {
        chunk.release_at += ctx.faults.stall;
        ctx.stalled = true;
    }
    return Verdict::Forward;
}

Verdict Pipeline::run(Chunk& chunk, StageContext& ctx) const {
    for (const auto& stage : stages_) {
        Verdict v = stage->apply(chunk, ctx);
        if (v != Verdict::Forward) return v;
    }
    return Verdict::Forward;
}

const Pipeline& Pipeline::standard() {
    static const Pipeline pipeline = [] {
        Pipeline p;
        p.add(std::make_unique<ResetStage>());
        p.add(std::make_unique<BlackholeStage>());
        p.add(std::make_unique<ThrottleStage>());
        p.add(std::make_unique<LatencyStage>());
        p.add(std::make_unique<StallStage>());
        return p;
    }();
    return pipeline;
}

}  // namespace chaos
