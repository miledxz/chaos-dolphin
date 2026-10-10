#pragma once

#include <cstdint>
#include <deque>
#include <string>

#include "chaos/pipeline.hpp"
#include "chaos/rules.hpp"
#include "chaos/token_bucket.hpp"
#include "net/fd.hpp"
#include "net/poller.hpp"
#include "net/socket.hpp"
#include "util/clock.hpp"

namespace chaos {

class Worker;

struct ConnSnapshot {
    std::uint64_t id = 0;
    std::uint32_t worker = 0;
    std::string peer;
    std::string state;
    Duration age{};
    std::uint64_t bytes_up = 0, bytes_down = 0;
    std::size_t queued_up = 0, queued_down = 0;
    std::string faults_up, faults_down;
};

// One direction of a session: bytes read from `src`, delayed in a FIFO, written to `dst`.
struct Pipe {
    std::deque<Chunk> queue;          // release_at is non-decreasing front to back
    std::size_t queued_bytes = 0;
    std::size_t head_offset = 0;      // bytes of queue.front() already written
    Clock::time_point last_release{};

    bool src_eof = false;     // source sent FIN
    bool dst_shut = false;    // we sent FIN to the destination
    bool paused = false;      // reading stopped by backpressure
    bool want_write = false;  // destination's socket buffer is full

    bool timer_armed = false;
    Clock::time_point timer_due{};

    Faults faults;
    TokenBucket bucket;
    std::uint64_t bytes = 0;  // forwarded
};

// A proxied connection. Owned by exactly one Worker and only ever touched on
// that worker's thread, so it needs no locking.
class Session {
public:
    Session(Worker& worker, std::uint64_t id, net::Fd client, const net::Address& peer);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Starts the non-blocking connect to the upstream server.
    void start();

    void on_event(int side, bool readable, bool writable);
    void on_timer(Dir dir, Clock::time_point due);
    void kill(bool rst, const char* reason);

    bool closed() const { return state_ == State::Closed; }
    std::uint64_t id() const { return info_.id; }
    ConnSnapshot snapshot() const;

    static constexpr int kClientSide = 0;
    static constexpr int kUpstreamSide = 1;
    static std::uint64_t token(std::uint64_t id, int side) { return (id << 1) | static_cast<std::uint64_t>(side); }

private:
    enum class State { Connecting, Open, Closed };

    Pipe& pipe(Dir d) { return d == Dir::Up ? up_ : down_; }
    int src_fd(Dir d) const { return d == Dir::Up ? client_.get() : upstream_.get(); }
    int dst_fd(Dir d) const { return d == Dir::Up ? upstream_.get() : client_.get(); }

    void finish_connect();
    void read_from(Dir dir);
    void flush(Dir dir);
    void arm_timer(Dir dir, Clock::time_point due);
    void refresh_faults();
    void update_interest();
    void maybe_finish();
    void close(bool rst, const std::string& reason);

    Worker& worker_;
    ConnInfo info_;
    std::string peer_;
    net::Fd client_;
    net::Fd upstream_;
    State state_ = State::Connecting;
    Clock::time_point started_;

    Pipe up_, down_;
    Rng rng_;
    std::uint64_t rules_version_ = ~std::uint64_t{0};

    net::Interest client_interest_{}, upstream_interest_{};
};

}  // namespace chaos
