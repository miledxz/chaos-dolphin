#!/usr/bin/env python3
"""End-to-end checks for chaosproxy in front of a Redis-protocol server.

Standard library only. Talks RESP to the server (through the proxy and directly)
and drives the proxy's control port to change faults between checks.

usage: chaos_check.py --proxy HOST:PORT --direct HOST:PORT --control HOST:PORT [--only NAME ...]
"""
import argparse
import re
import socket
import statistics
import sys
import threading
import time

ARGS = None
CHECKS = []  # (name, fn, default)


def check(default=True):
    def wrap(fn):
        CHECKS.append((fn.__name__, fn, default))
        return fn
    return wrap


class CheckFailed(Exception):
    pass


def expect(cond, msg):
    if not cond:
        raise CheckFailed(msg)


def split_addr(addr):
    host, port = addr.rsplit(":", 1)
    return host, int(port)


# --------------------------------------------------------------------------- RESP client

class Resp:
    def __init__(self, addr, timeout=10.0):
        self.sock = socket.create_connection(split_addr(addr), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = bytearray()
        self.local_port = self.sock.getsockname()[1]

    def send(self, *args):
        out = [b"*%d\r\n" % len(args)]
        for a in args:
            a = a if isinstance(a, bytes) else str(a).encode()
            out.append(b"$%d\r\n%s\r\n" % (len(a), a))
        self.sock.sendall(b"".join(out))

    def _fill(self):
        chunk = self.sock.recv(1 << 16)
        if not chunk:
            raise ConnectionError("connection closed by peer")
        self.buf += chunk

    def _line(self):
        while True:
            i = self.buf.find(b"\r\n")
            if i >= 0:
                line = bytes(self.buf[:i])
                del self.buf[: i + 2]
                return line
            self._fill()

    def _exact(self, n):
        while len(self.buf) < n + 2:
            self._fill()
        data = bytes(self.buf[:n])
        del self.buf[: n + 2]
        return data

    def reply(self):
        line = self._line()
        kind, rest = line[:1], line[1:]
        if kind == b"+":
            return rest.decode()
        if kind == b"-":
            raise RuntimeError(rest.decode())
        if kind == b":":
            return int(rest)
        if kind == b"$":
            n = int(rest)
            return None if n < 0 else self._exact(n)
        if kind == b"*":
            return [self.reply() for _ in range(int(rest))]
        raise RuntimeError(f"bad RESP line {line!r}")

    def cmd(self, *args):
        self.send(*args)
        return self.reply()

    def close(self):
        self.sock.close()


def proxy(timeout=10.0):
    return Resp(ARGS.proxy, timeout)


def direct(timeout=10.0):
    return Resp(ARGS.direct, timeout)


# --------------------------------------------------------------------------- helpers

def ctl(line):
    with socket.create_connection(split_addr(ARGS.control), timeout=10) as s:
        s.sendall(line.encode() + b"\n")
        s.shutdown(socket.SHUT_WR)
        out = b""
        while chunk := s.recv(65536):
            out += chunk
    reply = out.decode()
    if reply.startswith("err"):
        raise CheckFailed(f"control '{line}' -> {reply.strip()}")
    return reply


def ping_ms(conn):
    t0 = time.perf_counter()
    expect(conn.cmd("PING") == "PONG", "PING did not return PONG")
    return (time.perf_counter() - t0) * 1000


def pings(conn, n):
    return [ping_ms(conn) for _ in range(n)]


def p(values, q):
    values = sorted(values)
    return values[min(len(values) - 1, int(round(q / 100 * (len(values) - 1))))]


def fmt(values):
    return f"p50={p(values, 50):.1f}ms p99={p(values, 99):.1f}ms min={min(values):.1f}ms max={max(values):.1f}ms"


def stats():
    line = ctl("stats").splitlines()[0]
    out = {k: int(v) for v, k in re.findall(r"(\d+) (active|total)", line)}
    out.update({k: int(v) for k, v in re.findall(r"(delayed|stalled|blackholed|resets) (\d+)", line)})
    return out


# --------------------------------------------------------------------------- checks

@check()
def baseline_overhead():
    """Proxy without faults adds negligible latency compared to talking directly."""
    d, c = direct(), proxy()
    pings(d, 20), pings(c, 20)  # warm up
    dv, cv = pings(d, 200), pings(c, 200)
    d.close(), c.close()
    overhead = p(cv, 50) - p(dv, 50)
    expect(overhead < 2.0, f"proxy adds {overhead:.2f}ms at p50")
    return f"direct {fmt(dv)} | proxy {fmt(cv)} | p50 overhead {overhead:.2f}ms"


@check()
def latency_both_directions():
    """latency 100ms applies to requests and responses: RTT ~200ms."""
    ctl("set latency 100ms")
    c = proxy()
    v = pings(c, 15)
    c.close()
    expect(200 <= p(v, 50) <= 260, f"p50 {p(v, 50):.1f}ms, expected 200-260")
    return fmt(v)


@check()
def latency_one_direction():
    """dir=down only delays responses."""
    ctl("set latency 100ms dir=down")
    c = proxy()
    v = pings(c, 15)
    c.close()
    expect(100 <= p(v, 50) <= 150, f"p50 {p(v, 50):.1f}ms, expected 100-150")
    return fmt(v)


@check()
def jitter_spreads_but_keeps_order():
    """latency 30ms jitter 25ms: RTTs vary, yet 2000 pipelined commands come back intact and in order."""
    ctl("set latency 30ms jitter 25ms")
    c = proxy()
    v = pings(c, 40)
    spread = max(v) - min(v)
    expect(spread >= 20, f"RTT spread only {spread:.1f}ms")
    for i in range(1000):
        c.send("SET", f"jit:{i}", f"value-{i}")
    for i in range(1000):
        c.send("GET", f"jit:{i}")
    sets = [c.reply() for _ in range(1000)]
    gets = [c.reply() for _ in range(1000)]
    c.close()
    expect(all(r == "OK" for r in sets), "a SET failed")
    bad = [i for i, g in enumerate(gets) if g != f"value-{i}".encode()]
    expect(not bad, f"{len(bad)} GETs returned the wrong value (first at {bad[:1]})")
    return f"{fmt(v)} spread={spread:.1f}ms; 1000 SET + 1000 GET pipelined, all in order"


@check()
def drop_stalls_like_retransmits():
    """drop 30% stall 250ms: some RTTs jump by ~250ms, data still exact (pipelined INCR 1..1000)."""
    ctl("set drop 30% stall 250ms")
    c = proxy()
    v = pings(c, 60)
    stalled = [x for x in v if x >= 240]
    fast = [x for x in v if x < 50]
    expect(stalled and fast, f"expected a mix of stalled and fast pings, got {fmt(v)}")
    d = direct()
    d.cmd("DEL", "drop:counter")
    d.close()
    for _ in range(1000):
        c.send("INCR", "drop:counter")
    got = [c.reply() for _ in range(1000)]
    c.close()
    expect(got == list(range(1, 1001)), "INCR replies out of order or missing")
    share = len(stalled) / len(v) * 100
    return f"{len(stalled)}/{len(v)} pings stalled ({share:.0f}%, expected ~51%: 1-0.7^2, either direction can stall), {fmt(v)}; INCR 1..1000 exact"


@check()
def throttle_downstream():
    """throttle 256KB/s dir=down: a 1MB GET takes ~4s instead of milliseconds."""
    value = bytes(range(256)) * 4096  # 1 MiB
    d = direct()
    d.cmd("SET", "big", value)
    t0 = time.perf_counter()
    expect(d.cmd("GET", "big") == value, "direct GET mismatch")
    direct_s = time.perf_counter() - t0
    d.close()

    ctl("set throttle 256KB/s dir=down")
    c = proxy(timeout=30)
    t0 = time.perf_counter()
    got = c.cmd("GET", "big")
    took = time.perf_counter() - t0
    c.close()
    expect(got == value, "throttled GET returned wrong bytes")
    rate = len(value) / took / 1000
    expect(3.4 <= took <= 6.0, f"took {took:.2f}s, expected ~4.1s")
    return f"1MiB GET: direct {direct_s * 1000:.0f}ms, throttled {took:.2f}s ({rate:.0f} KB/s effective, target 256 KB/s)"


@check()
def throttle_upstream():
    """throttle 128KB/s dir=up: a 512KB SET takes ~4s."""
    value = b"u" * (512 * 1024)
    ctl("set throttle 128KB/s dir=up")
    c = proxy(timeout=30)
    t0 = time.perf_counter()
    expect(c.cmd("SET", "bigup", value) == "OK", "SET failed")
    took = time.perf_counter() - t0
    c.close()
    expect(3.4 <= took <= 6.0, f"took {took:.2f}s, expected ~4.1s")
    d = direct()
    expect(d.cmd("STRLEN", "bigup") == len(value), "server stored wrong length")
    d.close()
    return f"512KiB SET took {took:.2f}s ({len(value) / took / 1000:.0f} KB/s effective, target 128 KB/s); server got all bytes"


@check()
def blackhole_triggers_client_timeout():
    """blackhole: the client's 1s socket timeout fires; after clearing, new connections work."""
    c = proxy(timeout=1.0)
    ping_ms(c)
    before = stats().get("blackholed", 0)
    ctl("set blackhole on")
    t0 = time.perf_counter()
    try:
        c.cmd("PING")
        raise CheckFailed("PING got an answer through a blackhole")
    except socket.timeout:
        waited = time.perf_counter() - t0
    c.close()
    ctl("clear")
    c2 = proxy()
    after_ms = ping_ms(c2)
    c2.close()
    expect(stats().get("blackholed", 0) > before, "blackholed counter did not move")
    return f"client timed out after {waited:.2f}s (socket timeout 1.0s); fresh connection after clear: {after_ms:.1f}ms"


@check()
def reset_surfaces_as_connection_error():
    """reset 100%: the client sees ECONNRESET / closed connection."""
    before = stats().get("resets", 0)
    ctl("set reset 100%")
    c = proxy()
    try:
        c.cmd("PING")
        raise CheckFailed("PING succeeded despite reset 100%")
    except (ConnectionError, OSError) as e:
        err = type(e).__name__ + (f" ({e})" if str(e) else "")
    c.close()
    expect(stats().get("resets", 0) == before + 1, "resets counter did not increase by 1")
    return f"client got {err}"


@check()
def per_connection_rule_by_client_port():
    """A rule matching one connection's source port leaves the other connection alone."""
    a, b = proxy(), proxy()
    ping_ms(a), ping_ms(b)
    ctl(f"set latency 150ms match cport={b.local_port}")
    va, vb = pings(a, 10), pings(b, 10)
    a.close(), b.close()
    expect(p(va, 50) < 20, f"untargeted conn slowed: {fmt(va)}")
    expect(p(vb, 50) >= 300, f"targeted conn not slowed: {fmt(vb)}")
    return f"cport={b.local_port}: targeted {fmt(vb)} | other {fmt(va)}"


@check()
def live_change_on_open_connection():
    """Rules change the behaviour of an already-open connection, then revert."""
    c = proxy()
    before = pings(c, 10)
    ctl("set name=live latency 80ms")
    during = pings(c, 10)
    ctl("del live")
    after = pings(c, 10)
    c.close()
    expect(p(before, 50) < 20 and p(during, 50) >= 160 and p(after, 50) < 20,
           f"before {p(before, 50):.1f} during {p(during, 50):.1f} after {p(after, 50):.1f}")
    return f"same connection p50: before {p(before, 50):.1f}ms -> with rule {p(during, 50):.1f}ms -> after del {p(after, 50):.1f}ms"


@check()
def ttl_rule_expires():
    """A rule with ttl=2s disappears on its own."""
    c = proxy()
    ctl("set latency 150ms ttl=2s")
    slow = p(pings(c, 3), 50)
    time.sleep(2.5)
    fast = p(pings(c, 5), 50)
    c.close()
    rules = ctl("rules")
    expect(slow >= 300 and fast < 20, f"slow {slow:.1f}ms fast {fast:.1f}ms")
    expect("(0 rules)" in rules, f"rule still listed: {rules.strip()}")
    return f"p50 {slow:.1f}ms while active -> {fast:.1f}ms after expiry; rule list empty"


@check()
def kill_from_control():
    """`conns` lists the connection with its peer; `kill <id> rst` resets it."""
    c = proxy()
    ping_ms(c)
    listing = ctl("conns")
    conn_id = None
    for m in re.finditer(r"#(\d+)\s+(\S+)", listing):
        if m.group(2).endswith(f":{c.local_port}"):
            conn_id = m.group(1)
    expect(conn_id, f"own connection (port {c.local_port}) not in conns:\n{listing}")
    reply = ctl(f"kill {conn_id} rst").strip()
    try:
        c.cmd("PING")
        raise CheckFailed("connection still alive after kill")
    except (ConnectionError, OSError) as e:
        err = type(e).__name__
    c.close()
    return f"found #{conn_id} by peer port {c.local_port}; '{reply}'; client got {err}"


@check()
def concurrent_clients_under_chaos():
    """40 clients x 250 pipelined SET+GET each, with latency, jitter and drops: no corruption."""
    ctl("set latency 20ms jitter 10ms drop 5% stall 100ms")
    errors, done = [], []
    peak = {"active": 0}

    def client(n):
        try:
            c = proxy(timeout=30)
            for i in range(250):
                c.send("SET", f"cc:{n}:{i}", f"{n}-{i}")
                c.send("GET", f"cc:{n}:{i}")
            for i in range(250):
                expect(c.reply() == "OK", "SET failed")
                got = c.reply()
                expect(got == f"{n}-{i}".encode(), f"client {n} key {i}: got {got!r}")
            done.append(n)
            c.close()
        except Exception as e:  # noqa: BLE001
            errors.append(f"client {n}: {e!r}")

    threads = [threading.Thread(target=client, args=(n,)) for n in range(40)]
    t0 = time.perf_counter()
    for t in threads:
        t.start()
    time.sleep(0.05)
    peak["active"] = stats().get("active", 0)
    for t in threads:
        t.join()
    took = time.perf_counter() - t0
    expect(not errors, "; ".join(errors[:3]))
    expect(len(done) == 40, f"only {len(done)} clients finished")
    return f"40/40 clients, 20,000 commands verified in {took:.2f}s; {peak['active']} connections active mid-run"


@check(default=False)
def upstream_down():
    """With the upstream gone, a new client connection is closed instead of hanging."""
    c = proxy(timeout=15)
    t0 = time.perf_counter()
    try:
        c.cmd("PING")
        raise CheckFailed("PING succeeded with upstream down")
    except socket.timeout:
        raise CheckFailed(f"client hung {time.perf_counter() - t0:.1f}s: the proxy never closed the connection")
    except (ConnectionError, OSError) as e:
        err = type(e).__name__
    took = time.perf_counter() - t0
    c.close()
    expect(took < 10, f"took {took:.1f}s to close")
    return f"client got {err} after {took * 1000:.0f}ms"


@check(default=False)
def upstream_back():
    """After the upstream returns (same service address), new connections work without restarting the proxy."""
    deadline = time.time() + 60
    last = None
    while time.time() < deadline:
        try:
            c = proxy(timeout=3)
            ms = ping_ms(c)
            c.close()
            return f"PING ok ({ms:.1f}ms) through the same proxy process"
        except Exception as e:  # noqa: BLE001
            last = e
            time.sleep(1)
    raise CheckFailed(f"never recovered: {last!r}")


# --------------------------------------------------------------------------- main

def wait_ready():
    deadline = time.time() + 60
    while True:
        try:
            ctl("stats")
            c = proxy(timeout=2)
            ping_ms(c)
            c.close()
            return
        except Exception:  # noqa: BLE001
            if time.time() > deadline:
                raise
            time.sleep(0.5)


def main():
    global ARGS
    ap = argparse.ArgumentParser()
    ap.add_argument("--proxy", required=True)
    ap.add_argument("--direct", required=True)
    ap.add_argument("--control", required=True)
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--no-wait", action="store_true")
    ARGS = ap.parse_args()

    if not ARGS.no_wait:
        wait_ready()
    selected = [(n, f) for n, f, default in CHECKS if (n in ARGS.only if ARGS.only else default)]
    failed = 0
    for name, fn in selected:
        if ARGS.only is None or name not in ("upstream_down", "upstream_back"):
            try:
                ctl("clear")
            except Exception:  # noqa: BLE001
                pass
        t0 = time.perf_counter()
        try:
            detail = fn()
            status = "PASS"
        except Exception as e:  # noqa: BLE001
            detail, status = f"{type(e).__name__}: {e}", "FAIL"
            failed += 1
        print(f"{status}  {name:<36} {time.perf_counter() - t0:6.2f}s  {detail}", flush=True)
    try:
        ctl("clear")
        print("\nproxy stats: " + ctl("stats").splitlines()[0])
    except Exception:  # noqa: BLE001
        pass
    print(f"\n{len(selected) - failed}/{len(selected)} checks passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
