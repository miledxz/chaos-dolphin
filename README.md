# chaosproxy

A TCP proxy that sits between a client and a server and injects latency, jitter,
packet-loss stalls, throttling, blackholes and connection resets, per connection
and per direction. You can change the rules live while traffic is flowing. Use it
to see how an app behaves on a bad network.

```
client ──> chaosproxy :8000 ──> upstream server
               ▲
               └── control :9100  (chaosproxy ctl ... / nc)
```

## Build

You need a C++20 compiler and CMake 3.20 or newer. It has been tested with Apple
clang 15 (macOS, kqueue) and GCC 13 (Linux, epoll). It has no other dependencies.

```sh
cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure      # or ./build/chaos_tests [filter]
```

To build with sanitizers, use `-DCHAOS_SANITIZE=address` (ASan + UBSan) or
`-DCHAOS_SANITIZE=thread`. To treat warnings as errors, add `-DCHAOS_WERROR=ON`.

## Run

```sh
# Proxy localhost:15432 -> Postgres, with 100ms ± 20ms latency from the start
./build/chaosproxy --upstream 127.0.0.1:5432 --listen :15432 --set "latency 100ms jitter 20ms" --stats 5

# Change the rules while traffic is flowing
./build/chaosproxy ctl set drop 2% stall 400ms
./build/chaosproxy ctl set throttle 64kbps dir=down match cport=51234
./build/chaosproxy ctl set name=outage blackhole on ttl=10s
./build/chaosproxy ctl conns
./build/chaosproxy ctl kill 3 rst
./build/chaosproxy ctl clear
```

`nc 127.0.0.1 9100` also gives you an interactive control session.

## Faults

TCP is a byte stream, and the kernel repairs packet loss and reordering before the
app sees anything. So each fault is defined by what an app would actually observe:

| Setting | Effect |
|---|---|
| `latency 200ms` | each chunk is held for this long |
| `jitter 50ms` | adds a uniform ±50ms; byte order is still preserved |
| `drop 5%` + `stall 300ms` | "lost" chunks stall for a retransmit-like timeout, and later chunks wait behind them (head-of-line blocking) |
| `throttle 64kbps` | token bucket. `kbps`/`mbit` = bits per second; `KB/s`, `KBps` or a bare number = bytes per second; `off` disables it |
| `blackhole on` | bytes are read and discarded, so the client sees silence and should time out |
| `reset 0.1%` | chance per chunk of killing the connection with a TCP RST |

**Filters:** `conn=ID`, `ip=ADDR`, `cport=PORT` (the client's source port), and
`dir=up|down|both`.

**Options:** `name=NAME` and `ttl=30s` (the rule deletes itself when the ttl runs out).

**How rules combine:** they layer field by field, and later rules win. An unnamed
`set` is keyed by its filters, so `set latency 200ms` followed by `set jitter 50ms`
builds a single rule.

## Design

- **Acceptor thread:** accepts clients and hands them round-robin to a worker. It
  never connects to the upstream itself.
- **Worker pool:** N event loops (kqueue or epoll), each owning its own sessions.
  Session state is only touched on its owner's thread, so it needs no locks. The
  worker starts the non-blocking upstream connect.
- **Per-direction pipeline:** reset → blackhole → throttle → latency/jitter → stall.
  The stages are stateless and shared. They can only move `release_at` later,
  consume a chunk, or request a reset.
- **FIFO:** each direction's queue is kept ordered by clamping `release_at` to be
  no earlier than the chunk before it.
- **Backpressure:** when too many bytes are queued in a direction, the worker stops
  reading from its source. The kernel buffers fill up and the sender slows down.
- **Timer thread:** a min-heap of `{due, worker, session, dir}` entries. It holds
  no data and never touches sockets. When an entry is due, it posts a wakeup to the
  owning worker. Each direction has at most one armed entry: the deadline of the
  head of its FIFO.
- **Rule store:** an immutable `Rules` snapshot plus an atomic version number.
  Workers check the version once per chunk and only take a lock when it has changed.
  libc++ has no `std::atomic<std::shared_ptr>`.
- **Control thread:** a line-protocol TCP server. It also expires rules whose ttl
  has run out.
- **Shutdown:** SIGINT/SIGTERM stops the acceptor, then the control thread, then the
  timer, then the workers. Stopping the workers closes every session.

Apple's libc++ lacks `std::jthread` and floating-point `from_chars`. This code uses
`std::thread` with stop flags, and `strtod`, instead.

## Not done yet

- **Reordering:** this needs a framed mode (length-prefixed messages) or a UDP mode,
  because reordering a raw TCP byte stream would only corrupt it.
- **`--seed`:** it makes the per-chunk fault decisions repeatable, but chunk
  boundaries depend on timing, so whole runs are not bit-for-bit reproducible.
