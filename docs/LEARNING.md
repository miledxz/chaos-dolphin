# What this project teaches

A guide to chaos-dolphin (`chaosproxy`): what each part does, the systems
concepts behind it, the bugs that came up, and what is worth carrying into
the rest of your career.

The project is about 2,800 lines of C++20, plus tests and a deployment harness.
It is small enough to read in an afternoon. It also covers most of the ideas
that come up in real networked systems: event loops, thread ownership,
timers, backpressure, flow control, lock-free configuration, graceful shutdown,
and testing across environments.

---

## Contents

1. [The project in one picture](#1-the-project-in-one-picture)
2. [The life of a byte](#2-the-life-of-a-byte)
3. [File-by-file tour](#3-file-by-file-tour)
4. [Architecture concepts](#4-architecture-concepts)
5. [TCP facts this project forces you to learn](#5-tcp-facts-this-project-forces-you-to-learn)
6. [The bugs, and what each one teaches](#6-the-bugs-and-what-each-one-teaches)
7. [How it was tested, and why it took four layers](#7-how-it-was-tested-and-why-it-took-four-layers)
8. [Career takeaways](#8-career-takeaways)
9. [Questions to test yourself](#9-questions-to-test-yourself)
10. [Where to go next](#10-where-to-go-next)

---

## 1. The project in one picture

A **proxy** is a program that sits between a client and a server. The client
connects to the proxy as if the proxy were the server. The proxy opens its own
connection to the real server and copies bytes in both directions.

A **chaos proxy** copies the bytes *badly on purpose*. It delays them, slows
them down, holds them back, swallows them, or resets the connection. You use it
to find out how an app behaves on a bad network *before* production finds out
for you.

```
                        ┌──────────────────── chaosproxy process ─────────────────────┐
                        │                                                             │
  client app ──connect──┼─> Acceptor thread ──hands fd──> Worker pool (N threads)     │
      ▲                 │                                  │  each runs an event loop │
      │   bytes (both   │                                  │  and owns its sessions   │
      └────directions)──┼──────────────────────────────────┤                          │
                        │                                  ├──> upstream server ◄─────┼── real app
                        │                                  │                          │
                        │     Timer thread  ◄──schedule────┤                          │
                        │       (min-heap)  ───wake-up────>┘                          │
                        │                                                             │
  admin (ctl/nc) ───────┼─> Control thread ──publishes──> RuleStore (versioned        │
                        │                                 immutable snapshots)        │
                        └─────────────────────────────────────────────────────────────┘
```

Four kinds of threads, each with **one job**:

| Thread | Job | Touches sockets? | Touches sessions? |
|---|---|---|---|
| Acceptor | `accept()` new clients, hand them to a worker | only the listening socket | no |
| Worker (N) | read, apply faults, write, for the sessions it owns | yes, its own | yes, its own only |
| Timer | sleep until the next deadline, then wake the right worker | no | no |
| Control | parse admin commands, publish new rules | only the control port | no (it asks workers via messages) |

The most important design rule in the whole project:

> **Every session belongs to exactly one worker thread, and only that thread
> ever touches it.** Every other thread talks to a worker by *posting a message*.

That rule is why there is no mutex inside the proxying code, and no data race
for ThreadSanitizer to find.

---

## 2. The life of a byte

Following one connection from start to finish is the best way to understand
the code. The file references are links you can click.

### 2.1 Accept

The client connects to port 6379. The **acceptor** thread
([acceptor.cpp](../src/core/acceptor.cpp)) waits in `poll()` on the listening
socket, with a 200 ms timeout so it notices shutdown. When the socket is ready,
it calls `accept()` in a loop until `EAGAIN`, because there may be several new
clients queued.

It sets three options on each new client socket:

- `O_NONBLOCK`: no call on this socket may ever block a worker.
- `TCP_NODELAY`: turns off Nagle's algorithm. Otherwise the kernel may hold
  small writes back for up to ~40 ms, which would show up as fake latency.
- `SO_NOSIGPIPE` (macOS): writing to a closed socket returns an error instead
  of killing the process with SIGPIPE.

Then it calls `pool.assign(fd, peer)`.

### 2.2 Hand-off to a worker

`WorkerPool::assign` ([worker.cpp](../src/core/worker.cpp)) gives the
connection a global id, picks a worker round-robin (`id % N`), and **posts a
task** to that worker: "adopt this fd".

Posting means: lock the worker's inbox mutex, push a `std::function`, unlock,
and wake the worker's poller if the inbox was empty. The acceptor never touches
the worker's data structures directly. One detail: `std::function` must be
copyable, and `Fd` is move-only, so the fd travels inside a `shared_ptr`.

### 2.3 Connect upstream, without blocking

On the worker thread, `Worker::adopt` creates a `Session` and calls
`Session::start()` ([session.cpp](../src/core/session.cpp)). That:

1. creates a non-blocking socket and calls `connect()`, which returns
   immediately with `EINPROGRESS`;
2. registers the upstream socket for **write** readiness, because a pending
   connect becomes writable when it finishes, successfully or not;
3. schedules a timer at `now + connect_timeout` (5 s by default).

While the connect is pending, the client socket is not even registered. Its
bytes simply wait in the kernel.

When the upstream socket becomes writable, `finish_connect()` reads `SO_ERROR`
to learn whether the connect succeeded. If it did, the session becomes `Open`
and both sockets are registered for reading. If the 5 s timer fires first, the
session closes the client. (Section 6.3 explains why that timer exists: a bug
found in Kubernetes.)

### 2.4 Read, then decide the fault

The client sends `PING`. The worker's event loop reports the client socket as
readable, and `Session::read_from(Dir::Up)` runs:

1. **Refresh the rules if they changed.** This costs one atomic load
   (`store.version()`) per chunk. The rule store's lock is only taken when the
   version number moved.
2. **`recv()` into a per-thread 64 KiB scratch buffer**, then copy exactly the
   `n` bytes received into a new `Chunk`. (Section 6.2 explains why it copies
   instead of keeping the buffer.)
3. **Run the fault pipeline** ([pipeline.cpp](../src/chaos/pipeline.cpp)).
   Each stage looks at the chunk and either moves its `release_at` time later,
   consumes it, or asks for a reset:
   ```
   reset? → blackhole? → throttle (token bucket) → latency ± jitter → stall ("drop")
   ```
4. **Keep the FIFO order:**
   `release_at = max(release_at, previous chunk's release_at)`. Jitter may
   give this chunk an earlier time than the one in front of it, but bytes must
   still leave in the order they arrived.
5. **Queue the chunk.** If too many bytes are now queued, set
   `paused = true`, which stops reading from this socket (backpressure).
6. **Try to flush.**

### 2.5 Flush, or wait for the timer

`Session::flush(dir)` sends chunks from the front of the queue while
`front.release_at <= now`:

- If `send()` takes only part of the chunk, the session remembers
  `head_offset` and registers for **write** readiness. That means the
  receiver's kernel buffer is full.
- If the front chunk is not due yet, it calls `arm_timer(front.release_at)`.

The timer thread ([timer_service.cpp](../src/core/timer_service.cpp)) holds a
min-heap of `{due, worker, session_id, dir}` entries. **It holds no data and
never touches a socket.** When an entry is due, it posts "wake session X,
direction D" to the owning worker. The worker looks the session up by id. If
the session is gone, the wake-up is ignored. Otherwise it calls `flush()`
again.

### 2.6 Closing: half-close, then full close

When the client finishes sending, it sends a FIN, and `recv()` returns 0. The
session marks `src_eof`. **It does not close anything yet**, because data may
still be queued in that direction. Once the queue empties, it calls
`shutdown(upstream, SHUT_WR)`, passing the FIN along.

The server still sends its reply, then its own FIN, and the same happens in the
other direction. When both directions have passed their FIN on, the session
closes. Doing this right lets protocols that rely on half-close (HTTP/1.0,
many RPC frameworks, `nc`) work correctly through the proxy.

To *inject* a reset, the proxy sets `SO_LINGER {on, 0 seconds}` before
`close()`. That makes the kernel send a RST instead of a FIN, so the peer sees
`ECONNRESET`.

---

## 3. File-by-file tour

Organised by layer, from the bottom up. Each layer only depends on the layers
below it.

```
src/
├── util/      clock, logging                    (no dependencies)
├── net/       sockets, RAII fd, poller          (depends on util)
├── chaos/     rules, parsing, faults, buckets   (pure logic, no sockets)
├── core/      sessions, workers, timer, proxy   (the engine)
├── control/   command parser + control server   (the admin interface)
└── main.cpp   CLI, signals, `ctl` subcommand
```

### `util/` (the foundation)

| File | What it does | Worth noticing |
|---|---|---|
| [clock.hpp](../src/util/clock.hpp) | Type aliases: `Clock` = `steady_clock`, `Duration`, `Rng` = `mt19937_64`, `Dir` {Up, Down} | **Always use `steady_clock` for timeouts.** `system_clock` can jump when NTP adjusts the wall clock. |
| [log.hpp](../src/util/log.hpp) / [log.cpp](../src/util/log.cpp) | Thread-safe logging with levels, using `std::format` | `std::format_string<Args...>` checks the format string at **compile time**. The level check happens *before* formatting, so disabled debug logs cost almost nothing. |

### `net/` (talking to the operating system)

| File | What it does | Worth noticing |
|---|---|---|
| [fd.hpp](../src/net/fd.hpp) | `Fd`: owns a file descriptor and closes it in its destructor. Move-only. | **RAII**: the resource's lifetime is the object's lifetime. Fds can't leak on error paths, because there is no "remember to close" step. Deleting the copy constructor makes "two owners" a compile error. |
| [socket.hpp](../src/net/socket.hpp) / [socket.cpp](../src/net/socket.cpp) | Address parsing and DNS (`getaddrinfo`), listen, non-blocking connect, socket options | `sockaddr_storage` holds IPv4 or IPv6 without the caller caring which. `pending_error()` reads `SO_ERROR`, the only way to learn whether a non-blocking connect worked. |
| [poller.hpp](../src/net/poller.hpp) | The `Poller` interface: `add`, `modify`, `remove`, `wait`, `wakeup` | One interface, two backends, chosen at compile time. The rest of the code never knows which OS it runs on. |
| [poller_kqueue.cpp](../src/net/poller_kqueue.cpp) | macOS/BSD backend | kqueue has separate *filters* for read and write. `EVFILT_USER` provides the cross-thread wake-up. |
| [poller_epoll.cpp](../src/net/poller_epoll.cpp) | Linux backend | epoll uses one bitmask per fd. An `eventfd` provides the wake-up. `EPOLLERR`/`EPOLLHUP` are reported as readable and writable, so the next `recv`/`send` surfaces the real error. |

### `chaos/` (the fault logic, with no I/O)

This layer is **pure logic**. Nothing in it touches a socket or reads the clock
by itself; time is always passed in. That makes it easy to test
deterministically.

| File | What it does | Worth noticing |
|---|---|---|
| [rules.hpp](../src/chaos/rules.hpp) / [rules.cpp](../src/chaos/rules.cpp) | The data model: `Faults` (resolved values), `FaultPatch` (optional overrides), `Match` (filters), `Rule`, `Rules`. Also the `set` command parser and the unit parsers (`200ms`, `64kbps`, `5%`). | **Layered configuration**: rules are partial patches applied in order, and later ones win field by field. `std::optional` models "not set" versus "set to zero". Unnamed rules are keyed by their filters, so repeated `set` commands merge into one rule. Units are explicit (`kbps` = bits, `KB/s` = bytes) because ambiguity there causes real bugs. |
| [rule_store.hpp](../src/chaos/rule_store.hpp) | Publishes immutable `Rules` snapshots with a version number | Readers check an atomic version number on the fast path and lock only when it changed. Writers copy, edit, then publish (copy-on-write). See section 4.5. |
| [token_bucket.hpp](../src/chaos/token_bucket.hpp) | Rate limiter that can go into **debt** | `reserve(bytes, now)` always succeeds and returns *when* those bytes may go. Back-to-back reservations are spaced at exactly the configured rate. |
| [pipeline.hpp](../src/chaos/pipeline.hpp) / [pipeline.cpp](../src/chaos/pipeline.cpp) | The `Stage` interface and five stages, plus `Pipeline::standard()` | **Stateless shared stages, with per-connection state passed in** through `StageContext` (rules, RNG, bucket, now). One pipeline object serves every connection. Adding a fault means adding a class. |

### `core/` (the engine)

| File | What it does | Worth noticing |
|---|---|---|
| [context.hpp](../src/core/context.hpp) | `ProxyContext`: the shared, thread-safe services (upstream address, rules, timer, stats, seed, connect timeout) | Dependency injection without a framework: one struct passed by reference instead of global variables. Tests can build their own. |
| [stats.hpp](../src/core/stats.hpp) / [stats.cpp](../src/core/stats.cpp) | Process-wide counters | `memory_order_relaxed`: counters only need atomicity, not ordering against other memory, so this is the cheapest correct choice. |
| [session.hpp](../src/core/session.hpp) / [session.cpp](../src/core/session.cpp) | **The heart.** One proxied connection: two `Pipe`s (up and down), connect, read, pipeline, FIFO, flush, timers, interest updates, close | Each `Pipe` is a small state machine: `src_eof`, `dst_shut`, `paused`, `want_write`, `timer_armed`. `update_interest()` *derives* which events to watch from that state, and only calls the OS when the answer changed. |
| [worker.hpp](../src/core/worker.hpp) / [worker.cpp](../src/core/worker.cpp) | `Worker`: one event-loop thread, its poller, its sessions, and its inbox. `WorkerPool`: N workers, round-robin assignment. | The loop is: wait → dispatch events by token → run posted tasks. A **token** encodes `(session_id << 1) | side`, so an event maps to a session without a pointer that could dangle. Sessions closed during a batch are erased right away; later events for them find nothing and are skipped. |
| [timer_service.hpp](../src/core/timer_service.hpp) / [timer_service.cpp](../src/core/timer_service.cpp) | The timer thread: min-heap plus `condition_variable::wait_until` | It only notifies the thread when a new entry is **earlier than the current head**, since that's the only case where the sleep time changes. Due entries are grouped per worker, so each worker gets one wake-up per batch. Section 6.1 covers the bug found here. |
| [acceptor.hpp](../src/core/acceptor.hpp) / [acceptor.cpp](../src/core/acceptor.cpp) | The accept loop | It deliberately does **not** connect upstream. A slow upstream must never stall new accepts. On `EMFILE` (out of file descriptors) it backs off briefly instead of spinning. |
| [proxy.hpp](../src/core/proxy.hpp) / [proxy.cpp](../src/core/proxy.cpp) | `Proxy`: builds and owns everything, with `start()` and `stop()` in a carefully chosen order | The **order of shutdown** matters: acceptor (no new work) → control (no new commands) → timer (no new wake-ups) → workers (close sessions). Tests and `main` use the same class, so the tests exercise the real wiring. |

### `control/` (the admin interface)

| File | What it does | Worth noticing |
|---|---|---|
| [commands.hpp](../src/control/commands.hpp) / [commands.cpp](../src/control/commands.cpp) | `CommandProcessor::execute(line)` returns reply text. Commands: `set`, `rules`, `del`, `clear`, `conns`, `kill`, `stats`, `help`. | **Asking another thread a question:** `conns` posts a task to every worker; each task fills a `std::promise`, and the control thread waits on the futures with a timeout. That is request/response over message passing. The control thread never reads session state directly. |
| [control_server.hpp](../src/control/control_server.hpp) / [control_server.cpp](../src/control/control_server.cpp) | Line-protocol TCP server built on `poll()`. Also removes rules whose ttl has expired. | It splits input into lines across `recv()` calls, because TCP has no message boundaries (section 5). On EOF it still answers a final line that had no newline. Housekeeping runs on the poll timeout tick. |

### `main.cpp` (the process)

[main.cpp](../src/main.cpp) parses the CLI (`--opt value` and `--opt=value`),
loads a rules file, and runs the `chaosproxy ctl ...` client mode.

The detail most worth studying is **signal handling with the self-pipe trick**.
A signal handler may only call async-signal-safe functions, so it does one
thing: `write()` one byte to a pipe. The main thread `poll()`s that pipe, with
a timeout that doubles as the stats-printing interval. When the byte arrives,
it shuts down normally, outside signal context.

### `tests/`

| File | What it covers |
|---|---|
| [test.hpp](../tests/test.hpp) / [test_main.cpp](../tests/test_main.cpp) | A ~100-line test framework (`TEST_CASE`, `CHECK`, `CHECK_EQ`), so the project builds with no downloads |
| [test_rules.cpp](../tests/test_rules.cpp) | Unit parsing, rule layering and matching, ttl, RuleStore versioning |
| [test_pipeline.cpp](../tests/test_pipeline.cpp) | Stages with a **fake clock and a fixed seed**: jitter bounds, drop rate, token bucket accuracy, determinism |
| [test_timer.cpp](../tests/test_timer.cpp) | Order of firing, re-sleeping when an earlier entry arrives, prompt shutdown |
| [test_proxy.cpp](../tests/test_proxy.cpp) | Full in-process system: echo server ← `Proxy` ← client sockets. Integrity, concurrency, every fault, live changes, control protocol, connect timeout. |

### `deploy/`

| File | What it does |
|---|---|
| [Dockerfile](../Dockerfile) | Two-stage build. The **build stage runs the test suite**, so a broken image cannot be built. The runtime stage is about 29 MB and runs as a non-root user. |
| [compose.yaml](../deploy/compose.yaml) | Dragonfly (a Redis-compatible server) ← chaosproxy ← tester |
| [k8s/stack.yaml](../deploy/k8s/stack.yaml) | Deployments and Services, with a hardened security context. The probes target the **control** port, because probing the data port would open a real upstream connection every few seconds. |
| [k8s/tester-job.yaml](../deploy/k8s/tester-job.yaml) | Runs the end-to-end suite as a Kubernetes Job |
| [k8s/netpol-isolate-dragonfly.yaml](../deploy/k8s/netpol-isolate-dragonfly.yaml) | A NetworkPolicy that silently drops SYNs to the upstream, to test the connect timeout |
| [test/chaos_check.py](../deploy/test/chaos_check.py) | Standard-library-only end-to-end checks: a minimal Redis-protocol client, control-port driver, latency percentiles, and integrity checks |

---

## 4. Architecture concepts

These are the ideas worth remembering, each tied to where it lives in the code.

### 4.1 Event loop / reactor pattern

**What:** one thread watches many sockets with a readiness API (epoll, kqueue)
and reacts to whichever is ready. It never blocks on a single socket.

**Why:** a thread per connection is simple, but costs about 8 MB of stack per
thread and a lot of context switching at 10k connections. A small number of
event loops scales to very large numbers of connections. nginx, Redis, Node.js,
Netty, Envoy and Dragonfly all use this design.

**Where:** `Worker::run()` and `Poller`.

**Details to know:**
- **Level-triggered** (used here): the OS keeps reporting a socket while it is
  still ready. That's forgiving, because if you read only part of the data you
  get told again. **Edge-triggered** reports only *changes*, which is faster
  but means you must drain the socket until `EAGAIN` every time or you'll hang.
- **Write interest is only turned on when the socket buffer is full.** A
  socket is almost always writable, so watching for writability all the time
  makes the loop spin. This is a classic beginner bug.

### 4.2 Thread ownership: "share nothing, pass messages"

**What:** instead of protecting shared data with locks, give each piece of data
exactly **one owning thread**. Other threads send it messages.

**Why:** there is no lock contention, no deadlock, and no data race inside a
session. The few places that need synchronisation (inboxes, the timer heap, the
rule store) are small and easy to reason about.

**Where:**
- `Worker::post()`: a mutex-protected vector plus a wake-up. It only wakes
  when the inbox goes from empty to non-empty; if it was already non-empty, a
  wake-up is already pending.
- The timer posts work to workers instead of touching sessions.
- The control thread gets connection snapshots through promises and futures.

**Where else you'll see this:** Seastar/ScyllaDB, Dragonfly ("shared-nothing,
thread per core"), Go's "share memory by communicating", the actor model
(Erlang, Akka).

### 4.3 Identifiers instead of pointers across threads

The timer stores `session_id`, not `Session*`. By the time a timer fires, the
session may be gone. A pointer would then be a use-after-free. An id lookup
either finds the session or finds nothing, and both are safe.

The same idea applies to the poller tokens. **When something can outlive the
thing it refers to, refer to it by id.** You will meet this again in caches,
handles in game engines, file handles, and database row ids.

### 4.4 Timers: a min-heap and a sleeping thread

The timer thread sleeps until the earliest deadline (`wait_until(top.due)`).
When a new entry arrives, it only notifies the thread if that entry is earlier
than the current earliest one.

**Cost of the "one wake-up per direction" rule:** the session only arms a
timer for the *front* of its FIFO queue, and only if nothing earlier is already
armed. With thousands of chunks queued, the heap still holds about two entries
per connection.

**Alternatives you should know:** a timer *wheel* (O(1) insert, used in the
Linux kernel, Kafka and Netty), per-thread timers inside the poller
(`EVFILT_TIMER`, `timerfd`), or hierarchical wheels. The heap is the simplest
correct choice for this scale.

### 4.5 Immutable snapshots plus a version number (RCU-style configuration)

**Problem:** the control thread changes rules while workers read them on every
chunk. Locking on every read would be slow.

**Solution:**
- Rules are immutable after publication.
- A writer copies the current rules, edits the copy, then swaps a
  `shared_ptr` under a mutex and bumps an atomic `version`.
- A reader compares `version` with the version it cached (one atomic load) and
  only takes the lock to grab the new snapshot when the number moved.
- Old snapshots stay valid for anyone still holding them; the `shared_ptr`
  frees them when the last reader lets go.

This is the same idea as Linux's RCU (read-copy-update), and as how Envoy and
nginx hot-reload configuration. **Readers never wait for writers.**

A related lesson: `std::atomic<std::shared_ptr>` exists in C++20, but Apple's
libc++ doesn't implement it, and libstdc++'s version uses a lock internally
anyway. Check what the standard library actually does rather than what the
standard says it could do.

### 4.6 Backpressure and flow control

**Problem:** if the proxy delays data by 1 second but keeps reading at full
speed, a fast sender fills the proxy's memory without limit.

**Solution:** when queued bytes reach the *high watermark*, stop reading from
the source. The kernel's receive buffer fills up, TCP's receive window drops to
zero, and the **real sender slows down**. Reading resumes below the *low
watermark* (a quarter of the high one).

Having two thresholds instead of one is called **hysteresis**. It stops the
proxy from flipping between paused and reading on every chunk.

This is the most transferable idea in the project. The same principle appears
in Reactive Streams, gRPC flow control, Kafka consumer lag, and HTTP/2 windows.
It all reduces to one rule: **a fast producer and a slow consumer need a bounded
buffer between them, plus a way to tell the producer to slow down.** Unbounded
queues are how systems run out of memory.

### 4.7 Ordering invariants

Jitter assigns random delays, so chunk 2 might be due before chunk 1. The fix
is one line: `release_at = max(release_at, last_release)`. Release times then
never go down along the queue, so FIFO order holds.

A simple invariant like this, enforced at one point, beats complex logic
scattered everywhere. When you design a queue, **state its invariant and
enforce it at the point where items are added**.

### 4.8 Token bucket rate limiting

Tokens refill at `rate` per second, up to a `burst` cap. Sending `n` bytes
costs `n` tokens. This version allows *debt*: it always grants the request and
returns the time when it becomes legal. That turns "may I send?" into "when may
I send?", which fits a queue with release times exactly.

Token buckets are everywhere: API rate limits, network traffic shaping, AWS
burst credits, Stripe and GitHub API limits.

### 4.9 Pipeline of stateless stages

Each fault is a small class with one method. The pipeline runs them in order
until one doesn't say "forward". The stages hold no state; per-connection state
(RNG, token bucket, current rules) is passed in a context object.

**Benefits:** one shared pipeline object instead of one per connection; each
stage is unit-testable on its own; a new fault means one new class. This is the
*chain of responsibility* pattern, the same shape as HTTP middleware (Express,
ASP.NET, Envoy filters).

### 4.10 Platform abstraction at the right boundary

The OS difference (kqueue versus epoll) is hidden behind a ~5-method
interface. Exactly one file per platform is compiled, with no `#ifdef` spread
through the logic. The interface is shaped by **what the caller needs**
("tell me when this fd is readable or writable, and let other threads wake
me"), not by either OS API.

### 4.11 Graceful shutdown

1. A signal arrives; the self-pipe turns it into an ordinary event.
2. Shutdown runs in dependency order: stop *intake* first (acceptor,
   control), then *internal sources of work* (timer), then the *workers*,
   which close every session.
3. A Kubernetes rolling restart showed the result: SIGTERM, then
   `shutting down`, open connections closed, final stats logged, exit 0 in
   1.8 s of a 10 s grace period.

A rule of thumb for any service: **stop accepting, finish or cancel in-flight
work, release resources, exit 0, all within the orchestrator's grace period.**

### 4.12 Deterministic randomness

Each session's RNG is seeded with `seed ^ (id × constant)`. With `--seed`, the
same sequence of chunks gets the same fault decisions. The tests use fixed
seeds, so they are deterministic. **Randomness in tests must be seeded**, or
test failures can't be reproduced.

---

## 5. TCP facts this project forces you to learn

1. **TCP is a byte stream, not a message stream.** One `send("PING")` can
   arrive as `"PI"` + `"NG"`, or merged with the next message. The control
   server buffers until it sees `\n`, and the Redis-protocol parser buffers
   until it has a full reply. Every TCP protocol needs its own framing
   (delimiters, length prefixes).

2. **Packet loss and reordering are invisible to applications.** The kernel
   retransmits and reorders before your `recv()` sees anything. A lost packet
   shows up only as **a stall**, about one retransmission timeout (≥ 200 ms on
   Linux). That is why "drop" in this proxy means "stall", and why reordering
   needs a framed or UDP mode. Literally dropping bytes from a TCP stream
   would simulate something real networks never do: corruption.

3. **A non-blocking `connect()` reports its result through writability plus
   `SO_ERROR`.** It does not return the error directly.

4. **FIN versus RST.** `close()` normally sends FIN, a polite "I'm done
   sending". `SO_LINGER{1,0}` + `close()` sends RST, an abrupt abort that the
   peer sees as `ECONNRESET`. Closing a socket that still has *unread* data in
   its receive buffer also sends RST. That's why the outage test saw
   `ConnectionResetError`, not a clean EOF.

5. **Half-close exists.** `shutdown(SHUT_WR)` means "I'm done sending, but
   still listening". A proxy must pass this through rather than closing
   everything at the first FIN.

6. **SIGPIPE kills processes by default.** Writing to a socket the peer has
   closed raises a signal whose default action is to terminate the process.
   Every network server must handle it (`SO_NOSIGPIPE`, `MSG_NOSIGNAL`, or
   ignoring the signal).

7. **Nagle's algorithm adds latency to small writes.** Set `TCP_NODELAY` on
   anything interactive.

8. **The listen backlog limit drops SYNs instead of refusing them.** When the
   accept queue is full, Linux and macOS silently ignore new SYNs, and the
   client keeps retrying for about 2 minutes. `listen(fd, 0)` means
   "*default size*" on macOS but "*tiny queue*" on Linux. Platform
   differences like this hide in the details.

9. **Kubernetes Services are NAT, and NAT has memory.** kube-proxy rewrites
   the destination of packets to a ClusterIP, and conntrack remembers that
   rewrite for the whole flow. Section 6.3 shows how that caused a hang.

---

## 6. The bugs, and what each one teaches

The real bugs are the most valuable part of this project. Each one came from a
different testing layer.

### 6.1 Use-after-free in the timer (found by AddressSanitizer)

```cpp
cv_.wait_until(lock, heap_.top().due);   // passes a REFERENCE into the vector
```

`wait_until` **releases the lock** while it sleeps. A worker then calls
`schedule()`, the heap's vector grows and moves to new memory, and the old
memory is freed. When `wait_until` wakes, it re-reads `due` through the stale
reference into freed memory.

**Fix:** copy the deadline first: `const auto due = heap_.top().due;`

**Lessons:**
- **Any reference into a container is invalidated by anything that can change
  the container**, including code that runs while *you* have released a lock.
- ThreadSanitizer did not flag this one (the threads were synchronised). AddressSanitizer
  did. **Run more than one sanitizer**; they catch different classes of bug.
- The code looked fine and passed all tests. Only instrumentation found it.

### 6.2 About 200 MiB of idle memory (found in Docker)

```cpp
std::vector<char> buf(65536);
n = recv(fd, buf.data(), buf.size(), 0);
buf.resize(n);          // size = 14 bytes, CAPACITY still 64 KiB
```

`resize()` to a smaller size never releases capacity. Every queued "PING"
chunk held 64 KiB. Thousands of delayed small chunks added up to hundreds of
megabytes. On top of that, the backpressure watermark counted `data.size()`,
so it *thought* only a few kilobytes were queued: the safety mechanism measured
the wrong thing. After the burst, glibc kept the freed memory in its
per-thread memory pools instead of returning it to the OS.

**Fix:** read into one reusable buffer per thread, then copy out exactly `n`
bytes. Idle memory after the suite went from 201 MiB to 2.1 MiB.

**Lessons:**
- **Size and capacity are different things.** Know what your containers
  actually allocate.
- **Make sure a safety mechanism measures the real resource.** A limit on the
  wrong quantity gives false confidence.
- To tell a **leak** from **retention**, run the workload several times. A
  leak keeps growing; retention levels off. This one levelled off at 201 MiB.
- Unit tests and sanitizers didn't catch it, because nothing was *incorrect*.
  It only showed up by **watching the process like an operator would**
  (`docker stats`).

### 6.3 Clients hung with no connect timeout (found in Kubernetes)

Dragonfly was scaled to 0. Right after that, a client connected through the
proxy and hung for 15 s, its full timeout. In Docker, the same scenario failed
in 1 ms.

The cause was a chain:
1. The pod was deleted, but kube-proxy had not yet removed it from the
   Service's routing rules.
2. The proxy's SYN was rewritten to the dead pod's IP.
3. conntrack remembered that rewrite, so **every retransmitted SYN went to the
   same dead IP**.
4. The proxy had no connect timeout, and the kernel keeps retrying a SYN for
   about 2 minutes.

**Fix:** `--connect-timeout` (default 5 s), driven by the existing timer
thread. It was proven with a NetworkPolicy that drops SYNs deterministically:
a client going straight to Dragonfly hangs, and a client through the proxy is
closed at 5.006 s. A regression test reproduces it locally with a full listen
queue.

**Lessons:**
- **Every network operation needs a timeout.** Connect, read, write, DNS. A
  missing timeout eventually turns into a hang.
- **"Connection refused" and "packet dropped" are completely different
  failures.** One fails in a millisecond, the other in minutes. Test both.
- **Environments differ in their failure modes.** Docker's bridge network
  refuses connections to a stopped container quickly; Kubernetes NAT can
  blackhole them. Same code, same test, different outcome.
- **Look at what "PASS" actually measured.** The check passed because it
  accepted *any* error, including a timeout. Reading the detail
  ("TimeoutError after 15022 ms") exposed the bug. A test that can't fail
  isn't testing anything, so the check was tightened.

### 6.4 Stats updated after the close (a race found by a flaky test)

`close()` closed the sockets, then decremented `conns_active`. A test saw EOF
and read the counter in between. **Fix:** update the counter first.

**Lesson:** make observable side effects happen in the order observers can see
them. If clients can see the close, the stats should already reflect it.

### 6.5 Bugs in the test tooling (equally educational)

- The Python stats parser expected `<number> <word>` when the output said
  `<word> <number>` → **check the output format against real output.**
- A socket's port was read after `close()` → **cache values while the
  resource is alive.**
- `docker compose run` started the stopped dependency, so the "upstream down"
  test ran with the upstream up → **know what your tools do implicitly**
  (`--no-deps`).
- zsh doesn't word-split `$VAR` like bash → **shell portability matters in
  scripts.**

The broader point: when a test fails, **suspect the test as much as the
code**, then prove which one is wrong.

---

## 7. How it was tested, and why it took four layers

| Layer | Tool | What it found that the others didn't |
|---|---|---|
| Unit tests with a fake clock and fixed seed | `test_rules`, `test_pipeline`, `test_timer` | Logic errors in parsing, layering, rates. Fast and deterministic. |
| In-process integration | `test_proxy.cpp` (real sockets, real threads) | Wiring, half-close, the control protocol, the `conns` stale-faults bug |
| Sanitizers | AddressSanitizer + UBSan, ThreadSanitizer | The timer use-after-free (6.1) |
| Containers and orchestration | Docker Compose, kind, a real Redis-protocol server | The memory retention (6.2), the connect hang (6.3), and probe/NAT/port behaviour |

Each layer found bugs the previous ones could not. In particular, **the most
serious production-style bugs only appeared in a production-like
environment.** That is the strongest argument for realistic end-to-end
environments in CI.

Other good testing habits used here:
- **The image build runs the tests**, so broken code can't produce an
  artifact.
- **Measure, don't assert vaguely.** The checks print p50/p99, effective
  rates and timings, so a PASS still tells you something (and a suspicious
  PASS, like 6.3, can be noticed).
- **Compare against the expected value.** "Drop 30%" should stall about 51%
  of round trips, because either direction can stall (1 − 0.7²). The first
  version of the check claimed 30–50%, which was wrong; working out the
  expected value catches mistakes in your own understanding.
- **Test the failure paths on purpose**: a dead upstream, a firewalled
  upstream, resets, kills, timeouts.

---

## 8. Career takeaways

### Skills this project demonstrates

You can put these in a résumé or discuss them in an interview:

- **Systems programming in modern C++:** RAII, move semantics, `std::optional`,
  `std::format`, concepts of ownership, `thread_local`, atomics with explicit
  memory orders.
- **Network programming:** non-blocking sockets, epoll and kqueue, TCP
  lifecycle (half-close, RST, SO_ERROR, Nagle, SIGPIPE), cross-platform
  abstraction.
- **Concurrency design:** thread ownership, message passing, condition
  variables, a timer service, a copy-on-write config store, and choosing *not*
  to use locks.
- **Reliability engineering:** backpressure, timeouts, graceful shutdown,
  fault injection.
- **Testing and debugging:** layered testing, sanitizers, deterministic tests,
  container and Kubernetes end-to-end tests, root-causing via conntrack and
  NAT.
- **Operational awareness:** memory observed with `docker stats` and cgroups,
  probes chosen so they don't create side effects, hardened pod security
  context, small images.

### A one-minute interview story

> "I built a TCP chaos proxy in C++20 that injects latency, throttling,
> stalls and resets per connection, with rules you can change live. Each
> connection is owned by one event-loop thread, so the data path has no locks.
> A timer thread holds only wake-ups, and config is published as versioned
> immutable snapshots. Testing in Kubernetes found a hang: a SYN routed by
> conntrack to a just-deleted pod was silently dropped, and the proxy had no
> connect timeout. I added one and proved it with a NetworkPolicy that drops
> SYNs. Docker testing also showed 200 MB idle memory, caused by vectors
> keeping their capacity after a resize; fixing it brought that down to 2 MB."

That story shows design, debugging depth, and knowing how things behave in
production. Interviewers ask about all three.

### Habits worth keeping

1. **Define the problem by what users observe.** "Drop" became "stall"
   because that's what an application sees. Build to the observable behaviour,
   not the textbook term.
2. **Give every piece of state a single owner.** It removes whole classes of
   bugs.
3. **Bound every queue and put a timeout on every wait.** Nearly every outage
   story involves one that was missing.
4. **Test in an environment shaped like production.** The two most serious
   bugs were invisible on a laptop.
5. **Read the numbers behind a PASS.** A passing test with a suspicious
   timing is a bug report.
6. **Prefer a small invariant enforced in one place** (`max(release_at,
   last_release)`) over defensive checks everywhere.
7. **Know your platform's actual behaviour** (missing `jthread`,
   `atomic<shared_ptr>`, `listen(0)`), not just the standard or the man page
   of a different OS.

### Topics this prepares you for

| If you go into... | This project gave you a head start on |
|---|---|
| Backend / distributed systems | timeouts, retries, backpressure, failure modes, chaos testing |
| SRE / platform / Kubernetes | probes, graceful shutdown, NAT and conntrack, network policies, observability |
| Databases / storage (e.g. Dragonfly, Redis, ScyllaDB) | shared-nothing threads, event loops, protocol parsing, memory accounting |
| Networking / proxies (Envoy, HAProxy, service meshes) | this *is* a small L4 proxy |
| Performance engineering | allocation behaviour, syscall counts, wake-up coalescing |

---

## 9. Questions to test yourself

<details>
<summary>1. Why doesn't the acceptor connect to the upstream itself?</summary>

A blocking connect to a slow or dead upstream would stall every new client
behind it, and on Linux a dropped SYN can block for about 2 minutes. The worker
does a non-blocking connect, so only that one session waits.
</details>

<details>
<summary>2. Why does the timer store a session id instead of a pointer?</summary>

The session can be closed and destroyed before the timer fires. An id lookup
safely finds nothing; a pointer would be a use-after-free.
</details>

<details>
<summary>3. What would happen without backpressure if you set `latency 5s` and upload 10 GB?</summary>

The proxy would keep reading at full speed and hold up to 5 seconds of
traffic in memory: potentially gigabytes, ending in an out-of-memory kill. With
backpressure, reading pauses at the watermark and TCP flow control slows the
sender.
</details>

<details>
<summary>4. Why is write interest normally off?</summary>

Sockets are almost always writable. With level-triggered polling, constant
write interest would make the event loop wake up continuously and burn CPU.
It's enabled only when a send returned `EAGAIN` or wrote only part of the data.
</details>

<details>
<summary>5. With `drop 30%`, why do about 51% of ping round trips stall?</summary>

A round trip has two chunks, the request and the response, each stalled
independently with probability 0.3. P(at least one stalls) = 1 − 0.7² = 0.51.
</details>

<details>
<summary>6. Why can't a TCP proxy realistically "reorder packets"?</summary>

The receiving kernel reorders TCP segments before the application sees them,
so applications never observe reordering. Reordering the byte stream inside
the proxy would corrupt it instead. Realistic reordering needs message
boundaries (a framed mode) or UDP.
</details>

<details>
<summary>7. Why is `wait_until(lock, heap_.top().due)` dangerous?</summary>

`wait_until` releases the lock while sleeping, so another thread can push to
the heap and reallocate it. The reference to `top().due` then points into freed
memory, and `wait_until` reads it again on wake-up.
</details>

<details>
<summary>8. Why do the Kubernetes probes target port 9100 instead of 6379?</summary>

A TCP probe on 6379 would be a real proxied connection, so it would connect
upstream, count as a session, and possibly have faults applied to it. Probing
the control port checks that the process is alive with no side effects.
</details>

<details>
<summary>9. How do you tell a memory leak from allocator retention?</summary>

Repeat the same workload. A leak grows with every run; retention reaches a
plateau and stays there. Here memory levelled off at 201 MiB across three
runs, so it was retention, made much worse by vectors keeping their 64 KiB
capacity.
</details>

<details>
<summary>10. Why are the rules immutable once published?</summary>

So readers never need a lock while they use them. A worker can keep using an
old snapshot safely while a new one is published; reference counting frees the
old one when the last reader drops it.
</details>

---

## 10. Where to go next

**Features that would teach something new:**
- **Framed mode with real reordering:** parse length-prefixed messages and
  swap them within a window. You'd learn stream framing and partial-message
  buffering.
- **UDP mode:** real drop, duplication and reordering of datagrams. You'd
  learn connectionless "sessions" and NAT-style session tracking.
- **Re-resolve the upstream DNS** with a TTL, without blocking workers. You'd
  learn async DNS (c-ares) or a dedicated resolver thread.
- **Prometheus `/metrics`:** histograms of injected delay. You'd learn
  observability conventions.
- **io_uring backend:** completion-based I/O instead of readiness-based.
  This is the modern Linux direction.
- **Benchmarks:** compare throughput and p99 overhead against a direct
  connection with `iperf3` or `memtier_benchmark`. You'd learn performance
  methodology.
- **Scripted scenarios:** "at t=10s add 500 ms for 30 s, then reset 1% of
  connections". You'd learn time-based state machines.

**Reading:**
- W. Richard Stevens, *UNIX Network Programming, Vol. 1*: the classic on
  sockets.
- Dan Kegel, *The C10K problem*: why event loops exist.
- `man 7 tcp`, `man 7 epoll`, `man 2 kqueue`, `man 2 connect` (see the
  `EINPROGRESS` section).
- Shopify's **Toxiproxy**: a production chaos proxy, useful for comparing
  design choices.
- *Designing Data-Intensive Applications* (Kleppmann), chapter 8 "The Trouble
  with Distributed Systems": the theory behind why tools like this matter.
- The Envoy architecture overview: what a production L4/L7 proxy adds on top
  of these ideas.
