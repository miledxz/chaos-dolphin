#include "core/session.hpp"

#include <sys/socket.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "core/worker.hpp"
#include "util/log.hpp"

namespace chaos {

namespace {

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;  // macOS: SO_NOSIGPIPE is set on the socket instead
#endif

constexpr std::size_t kMaxRead = 64 * 1024;
constexpr std::size_t kMaxQueued = 4 * 1024 * 1024;

// When throttled, read small chunks (~50ms worth) so output is smooth rather
// than one big burst followed by a long wait.
std::size_t read_cap(const Faults& f) {
    if (f.throttle_Bps == 0) return kMaxRead;
    return std::clamp<std::size_t>(f.throttle_Bps / 20, 256, kMaxRead);
}

// Backpressure: stop reading once this much is waiting in the proxy. With a
// throttle it is ~0.5s of traffic, so the proxy doesn't hide a huge buffer.
std::size_t high_watermark(const Faults& f) {
    if (f.throttle_Bps == 0) return kMaxQueued;
    return std::clamp<std::size_t>(f.throttle_Bps / 2, 16 * 1024, kMaxQueued);
}

bool would_block(int err) { return err == EAGAIN || err == EWOULDBLOCK; }

}  // namespace

Session::Session(Worker& worker, std::uint64_t id, net::Fd client, const net::Address& peer)
    : worker_(worker),
      info_{id, peer.ip(), peer.port()},
      peer_(peer.to_string()),
      client_(std::move(client)),
      started_(Clock::now()),
      rng_(worker.ctx().seed ^ (id * 0x9E3779B97F4A7C15ull)) {
    Stats::add(worker_.ctx().stats.conns_total);
    Stats::add(worker_.ctx().stats.conns_active);
}

Session::~Session() {
    if (!closed()) close(false, "shutdown");
}

void Session::start() {
    try {
        upstream_ = net::connect_nonblocking(worker_.ctx().upstream);
        worker_.poller().add(upstream_.get(), token(info_.id, kUpstreamSide), {.read = false, .write = true});
        upstream_interest_ = {.read = false, .write = true};
        // A SYN that is silently dropped (e.g. a Service still routing to a deleted
        // pod) would otherwise leave the client hanging for the kernel's ~2 minutes.
        connect_deadline_ = Clock::now() + worker_.ctx().connect_timeout;
        worker_.ctx().timer.schedule({connect_deadline_, worker_.index(), info_.id, Dir::Up});
    } catch (const std::exception& e) {
        Stats::add(worker_.ctx().stats.connect_failures);
        close(false, e.what());
    }
}

void Session::finish_connect() {
    if (int err = net::pending_error(upstream_.get()); err != 0) {
        Stats::add(worker_.ctx().stats.connect_failures);
        close(false, std::string("upstream connect failed: ") + std::strerror(err));
        return;
    }
    state_ = State::Open;
    refresh_faults();
    worker_.poller().add(client_.get(), token(info_.id, kClientSide), {});
    client_interest_ = {};
    log::info("conn {} open  {} -> {} (worker {})", info_.id, peer_,
              worker_.ctx().upstream.to_string(), worker_.index());
    update_interest();
}

void Session::on_event(int side, bool readable, bool writable) {
    if (closed()) return;
    if (state_ == State::Connecting) {
        if (side == kUpstreamSide && (writable || readable)) finish_connect();
        return;
    }
    if (readable) {
        read_from(side == kClientSide ? Dir::Up : Dir::Down);
        if (closed()) return;
    }
    if (writable) {
        // A writable client drains the down pipe; a writable upstream drains the up pipe.
        flush(side == kClientSide ? Dir::Down : Dir::Up);
        if (closed()) return;
    }
    update_interest();
}

void Session::on_timer(Dir dir, Clock::time_point due) {
    if (closed()) return;
    if (state_ == State::Connecting) {
        if (Clock::now() >= connect_deadline_) {
            Stats::add(worker_.ctx().stats.connect_failures);
            close(false, "upstream connect timed out after " + format_duration(worker_.ctx().connect_timeout));
        }
        return;
    }
    Pipe& p = pipe(dir);
    if (p.timer_armed && due >= p.timer_due) p.timer_armed = false;
    flush(dir);
    if (!closed()) update_interest();
}

void Session::refresh_faults() {
    auto& store = worker_.ctx().rules;
    if (store.version() == rules_version_) return;
    auto rules = store.snapshot();
    auto now = Clock::now();
    up_.faults = rules->resolve(info_, Dir::Up, now);
    down_.faults = rules->resolve(info_, Dir::Down, now);
    rules_version_ = rules->version;
}

void Session::read_from(Dir dir) {
    Pipe& p = pipe(dir);
    if (p.src_eof || p.paused) return;
    refresh_faults();

    // Read into a reusable per-thread buffer and copy out exactly what arrived:
    // a queued chunk must not pin a 64KB allocation for a few bytes of payload
    // (and queued_bytes, which drives backpressure, must reflect real memory).
    thread_local std::vector<char> scratch(kMaxRead);
    ssize_t n = ::recv(src_fd(dir), scratch.data(), read_cap(p.faults), 0);
    if (n < 0) {
        if (would_block(errno) || errno == EINTR) return;
        // The peer reset (or similar): pass the reset on to the other side.
        close(true, std::string(dir == Dir::Up ? "client" : "upstream") + " error: " + std::strerror(errno));
        return;
    }
    if (n == 0) {
        p.src_eof = true;
        flush(dir);  // sends FIN once everything queued has gone out
        return;
    }
    auto& stats = worker_.ctx().stats;
    auto now = Clock::now();
    Chunk chunk{std::vector<char>(scratch.data(), scratch.data() + n), now};
    StageContext ctx{p.faults, rng_, p.bucket, now};
    Stats::add(stats.chunks);

    switch (Pipeline::standard().run(chunk, ctx)) {
        case Verdict::Reset:
            Stats::add(stats.resets_injected);
            close(true, std::string("injected reset (") + to_string(dir) + ")");
            return;
        case Verdict::Consume:
            Stats::add(stats.chunks_blackholed);
            return;
        case Verdict::Forward:
            break;
    }

    // Keep the FIFO ordered: jitter may give this chunk an earlier release time
    // than the one in front of it, but bytes must leave in order.
    chunk.release_at = std::max(chunk.release_at, p.last_release);
    p.last_release = chunk.release_at;
    if (chunk.release_at > now) Stats::add(stats.chunks_delayed);
    if (ctx.stalled) Stats::add(stats.chunks_stalled);

    p.queued_bytes += chunk.data.size();
    p.queue.push_back(std::move(chunk));
    if (p.queued_bytes >= high_watermark(p.faults)) p.paused = true;

    flush(dir);
}

void Session::flush(Dir dir) {
    Pipe& p = pipe(dir);
    if (p.dst_shut) return;
    auto now = Clock::now();
    bool blocked = false;

    while (!p.queue.empty()) {
        Chunk& c = p.queue.front();
        if (c.release_at > now) break;
        const char* data = c.data.data() + p.head_offset;
        std::size_t left = c.data.size() - p.head_offset;
        ssize_t n = ::send(dst_fd(dir), data, left, kSendFlags);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (would_block(errno)) {
                blocked = true;
                break;
            }
            close(true, std::string(dir == Dir::Up ? "upstream" : "client") + " write failed: " + std::strerror(errno));
            return;
        }
        auto sent = static_cast<std::size_t>(n);
        p.bytes += sent;
        Stats::add(dir == Dir::Up ? worker_.ctx().stats.bytes_up : worker_.ctx().stats.bytes_down, sent);
        if (sent < left) {
            p.head_offset += sent;
            blocked = true;
            break;
        }
        p.queued_bytes -= c.data.size();
        p.head_offset = 0;
        p.queue.pop_front();
    }

    p.want_write = blocked;
    if (!blocked && !p.queue.empty()) arm_timer(dir, p.queue.front().release_at);
    if (p.paused && p.queued_bytes <= high_watermark(p.faults) / 4) p.paused = false;

    if (p.queue.empty() && p.src_eof && !p.dst_shut) {
        ::shutdown(dst_fd(dir), SHUT_WR);  // half-close: pass the FIN along
        p.dst_shut = true;
        maybe_finish();
    }
}

void Session::arm_timer(Dir dir, Clock::time_point due) {
    Pipe& p = pipe(dir);
    // One pending wakeup per direction is enough: the FIFO head is the earliest chunk.
    if (p.timer_armed && p.timer_due <= due) return;
    p.timer_armed = true;
    p.timer_due = due;
    worker_.ctx().timer.schedule({due, worker_.index(), info_.id, dir});
}

void Session::update_interest() {
    if (state_ != State::Open) return;
    net::Interest client{.read = !up_.src_eof && !up_.paused, .write = down_.want_write};
    net::Interest upstream{.read = !down_.src_eof && !down_.paused, .write = up_.want_write};
    if (client != client_interest_) {
        worker_.poller().modify(client_.get(), token(info_.id, kClientSide), client);
        client_interest_ = client;
    }
    if (upstream != upstream_interest_) {
        worker_.poller().modify(upstream_.get(), token(info_.id, kUpstreamSide), upstream);
        upstream_interest_ = upstream;
    }
}

void Session::maybe_finish() {
    if (up_.dst_shut && down_.dst_shut) close(false, "closed");
}

void Session::kill(bool rst, const char* reason) { close(rst, reason); }

void Session::close(bool rst, const std::string& reason) {
    if (closed()) return;
    state_ = State::Closed;
    // Update stats before the peers can observe the close.
    Stats::add(worker_.ctx().stats.conns_active, static_cast<std::uint64_t>(-1));
    for (net::Fd* fd : {&client_, &upstream_}) {
        if (!fd->valid()) continue;
        worker_.poller().remove(fd->get());
        if (rst) net::set_linger_reset(fd->get());
        fd->reset();
    }
    log::info("conn {} {} {} (up {} down {})", info_.id, rst ? "reset" : "close", reason,
              format_bytes(up_.bytes), format_bytes(down_.bytes));
}

ConnSnapshot Session::snapshot() const {
    ConnSnapshot s;
    s.id = info_.id;
    s.worker = worker_.index();
    s.peer = peer_;
    s.state = state_ == State::Connecting ? "connecting" : state_ == State::Open ? "open" : "closed";
    s.age = Clock::now() - started_;
    s.bytes_up = up_.bytes;
    s.bytes_down = down_.bytes;
    s.queued_up = up_.queued_bytes;
    s.queued_down = down_.queued_bytes;
    // Resolve against the current rules: the cached faults only refresh when the next chunk arrives.
    auto rules = worker_.ctx().rules.snapshot();
    auto now = Clock::now();
    s.faults_up = rules->resolve(info_, Dir::Up, now).describe();
    s.faults_down = rules->resolve(info_, Dir::Down, now).describe();
    return s;
}

}  // namespace chaos
