// End-to-end tests: echo server <- proxy <- client sockets, all in-process.

#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <thread>

#include "core/proxy.hpp"
#include "net/socket.hpp"
#include "test.hpp"

using namespace chaos;
using namespace std::chrono_literals;

namespace {

// Echoes everything back; after the client's FIN, finishes and closes.
class EchoServer {
public:
    EchoServer() : listen_(net::listen_tcp(net::Address::resolve("127.0.0.1:0"))) {
        port_ = net::local_port(listen_.get());
        thread_ = std::thread([this] { run(); });
    }
    ~EchoServer() {
        stopping_ = true;
        thread_.join();
        std::lock_guard lock(mutex_);
        for (int fd : open_) ::shutdown(fd, SHUT_RDWR);
        for (auto& t : conns_) t.join();
    }
    std::string address() const { return "127.0.0.1:" + std::to_string(port_); }

private:
    void run() {
        while (!stopping_) {
            pollfd pfd{listen_.get(), POLLIN, 0};
            if (::poll(&pfd, 1, 50) <= 0) continue;
            int fd = ::accept(listen_.get(), nullptr, nullptr);
            if (fd < 0) continue;
            net::set_nosigpipe(fd);
            std::lock_guard lock(mutex_);
            open_.push_back(fd);
            conns_.emplace_back([this, fd] { echo(fd); });
        }
    }
    void echo(int fd) {
        char buf[65536];
        ssize_t n;
        while ((n = ::recv(fd, buf, sizeof buf, 0)) > 0) {
            for (ssize_t off = 0; off < n;) {
                ssize_t w = ::send(fd, buf + off, n - off, 0);
                if (w <= 0) goto done;
                off += w;
            }
        }
    done:
        ::shutdown(fd, SHUT_WR);
        std::lock_guard lock(mutex_);
        std::erase(open_, fd);
        ::close(fd);
    }

    net::Fd listen_;
    std::uint16_t port_;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::vector<int> open_;
    std::vector<std::thread> conns_;
};

struct Harness {
    EchoServer echo;
    std::unique_ptr<Proxy> proxy;

    explicit Harness(std::vector<std::string> rules = {}) {
        Config cfg;
        cfg.listen = "127.0.0.1:0";
        cfg.control = "127.0.0.1:0";
        cfg.upstream = echo.address();
        cfg.workers = 2;
        cfg.seed = 7;
        cfg.rule_commands = std::move(rules);
        proxy = std::make_unique<Proxy>(cfg);
        proxy->start();
    }
    ~Harness() { proxy->stop(); }

    net::Fd connect(int recv_timeout_ms = 5000) {
        auto fd = net::connect_blocking(net::Address::resolve("127.0.0.1:" + std::to_string(proxy->listen_port())));
        set_recv_timeout(fd.get(), recv_timeout_ms);
        return fd;
    }

    void set(const std::string& cmd) {
        auto reply = proxy->execute(cmd);
        if (!reply.starts_with("ok")) test::fail(__FILE__, __LINE__, "command failed: " + reply);
    }

    static void set_recv_timeout(int fd, int ms) {
        timeval tv{ms / 1000, (ms % 1000) * 1000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }
};

void send_all(int fd, std::string_view s) {
    while (!s.empty()) {
        ssize_t n = ::send(fd, s.data(), s.size(), 0);
        if (n <= 0) test::fail(__FILE__, __LINE__, std::string("send failed: ") + std::strerror(errno));
        s.remove_prefix(static_cast<std::size_t>(n));
    }
}

// Reads exactly n bytes (fails the test on timeout/EOF).
std::string recv_exact(int fd, std::size_t n) {
    std::string out(n, '\0');
    std::size_t got = 0;
    while (got < n) {
        ssize_t r = ::recv(fd, out.data() + got, n - got, 0);
        if (r <= 0) test::fail(__FILE__, __LINE__, std::format("recv after {}/{} bytes: {}", got, n,
                                                               r == 0 ? "EOF" : std::strerror(errno)));
        got += static_cast<std::size_t>(r);
    }
    return out;
}

std::string recv_until_eof(int fd) {
    std::string out;
    char buf[65536];
    ssize_t r;
    while ((r = ::recv(fd, buf, sizeof buf, 0)) > 0) out.append(buf, static_cast<std::size_t>(r));
    if (r < 0) test::fail(__FILE__, __LINE__, std::string("recv: ") + std::strerror(errno));
    return out;
}

std::string pattern(std::size_t n, unsigned salt = 0) {
    std::string s(n, '\0');
    std::uint32_t x = 2463534242u + salt;
    for (auto& c : s) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        c = static_cast<char>(x);
    }
    return s;
}

// Sends `data` on a separate thread, half-closes, and returns everything echoed back.
std::string round_trip(int fd, const std::string& data) {
    std::thread writer([&] {
        send_all(fd, data);
        ::shutdown(fd, SHUT_WR);
    });
    std::string back = recv_until_eof(fd);
    writer.join();
    return back;
}

long long ping_ms(int fd) {
    auto t0 = Clock::now();
    send_all(fd, "ping");
    CHECK_EQ(recv_exact(fd, 4), std::string("ping"));
    return std::chrono::duration_cast<Millis>(Clock::now() - t0).count();
}

// Returns true if the next recv reports a reset or EOF (not a timeout).
bool sees_close(int fd) {
    char c;
    ssize_t r = ::recv(fd, &c, 1, 0);
    return r == 0 || (r < 0 && (errno == ECONNRESET || errno == EPIPE));
}

}  // namespace

TEST_CASE("proxy: pass-through keeps a 4MB stream intact, including half-close") {
    Harness h;
    auto fd = h.connect();
    auto data = pattern(4 << 20);
    CHECK(round_trip(fd.get(), data) == data);
}

TEST_CASE("proxy: 64 concurrent connections across workers") {
    Harness h;
    std::vector<std::thread> clients;
    std::atomic<int> ok{0};
    for (int i = 0; i < 64; ++i) {
        clients.emplace_back([&, i] {
            try {
                auto fd = h.connect();
                auto data = pattern(32 * 1024, static_cast<unsigned>(i));
                if (round_trip(fd.get(), data) == data) ++ok;
            } catch (...) {
            }
        });
    }
    for (auto& t : clients) t.join();
    CHECK_EQ(ok.load(), 64);
    CHECK_EQ(Stats::get(h.proxy->stats().conns_total), 64u);
}

TEST_CASE("proxy: latency applies in both directions") {
    Harness h({"set latency 60ms"});
    auto fd = h.connect();
    auto rtt = ping_ms(fd.get());
    CHECK(rtt >= 120);
    CHECK(rtt < 600);
}

TEST_CASE("proxy: jitter and drops never reorder or corrupt bytes") {
    Harness h({"set latency 5ms jitter 5ms drop 20% stall 30ms"});
    auto fd = h.connect();
    // Many small writes so there are many chunks with different release times.
    std::string sent;
    std::thread writer([&] {
        for (int i = 0; i < 400; ++i) {
            auto piece = std::format("<{:05}>", i);
            sent += piece;
            send_all(fd.get(), piece);
            if (i % 20 == 0) std::this_thread::sleep_for(1ms);
        }
        ::shutdown(fd.get(), SHUT_WR);
    });
    std::string back = recv_until_eof(fd.get());
    writer.join();
    CHECK(back == sent);
    CHECK(Stats::get(h.proxy->stats().chunks_stalled) > 0);
}

TEST_CASE("proxy: throttle limits throughput") {
    Harness h({"set throttle 200KB/s dir=up"});
    auto fd = h.connect();
    auto data = pattern(200'000);
    auto t0 = Clock::now();
    CHECK(round_trip(fd.get(), data) == data);
    auto ms = std::chrono::duration_cast<Millis>(Clock::now() - t0).count();
    CHECK(ms >= 800);   // ~1s at 200KB/s, minus the initial burst
    CHECK(ms < 2500);
}

TEST_CASE("proxy: blackhole swallows bytes until cleared") {
    Harness h;
    auto fd = h.connect(300);
    CHECK(ping_ms(fd.get()) < 200);
    h.set("set blackhole on");
    send_all(fd.get(), "lost");
    char c;
    CHECK(::recv(fd.get(), &c, 1, 0) < 0);  // times out: nothing comes back
    h.set("clear");
    send_all(fd.get(), "back");
    CHECK_EQ(recv_exact(fd.get(), 4), std::string("back"));
    CHECK(Stats::get(h.proxy->stats().chunks_blackholed) >= 1);
}

TEST_CASE("proxy: injected reset closes the connection") {
    Harness h({"set reset 100%"});
    auto fd = h.connect();
    send_all(fd.get(), "boom");
    CHECK(sees_close(fd.get()));
    CHECK_EQ(Stats::get(h.proxy->stats().resets_injected), 1u);
}

TEST_CASE("proxy: rule changes apply to live connections") {
    Harness h;
    auto fd = h.connect();
    CHECK(ping_ms(fd.get()) < 50);
    h.set("set latency 100ms");
    CHECK(ping_ms(fd.get()) >= 200);
    h.set("del all");
    CHECK(ping_ms(fd.get()) < 50);
}

TEST_CASE("proxy: per-connection rules only hit their connection") {
    Harness h;
    auto a = h.connect();
    auto b = h.connect();
    ping_ms(a.get());
    ping_ms(b.get());
    h.set("set latency 150ms match conn=2");
    CHECK(ping_ms(a.get()) < 50);
    CHECK(ping_ms(b.get()) >= 300);
}

TEST_CASE("proxy: dead upstream closes the client") {
    std::uint16_t dead_port;
    {
        auto tmp = net::listen_tcp(net::Address::resolve("127.0.0.1:0"));
        dead_port = net::local_port(tmp.get());
    }  // closed: connections to it are refused
    Config cfg;
    cfg.listen = "127.0.0.1:0";
    cfg.control = "";
    cfg.upstream = "127.0.0.1:" + std::to_string(dead_port);
    cfg.workers = 1;
    Proxy proxy(cfg);
    proxy.start();
    auto fd = net::connect_blocking(net::Address::resolve("127.0.0.1:" + std::to_string(proxy.listen_port())));
    Harness::set_recv_timeout(fd.get(), 3000);
    CHECK(sees_close(fd.get()));
    CHECK_EQ(Stats::get(proxy.stats().connect_failures), 1u);
}

TEST_CASE("control: TCP protocol, conns and kill") {
    Harness h;
    auto client = h.connect();
    ping_ms(client.get());

    auto ctl = net::connect_blocking(net::Address::resolve("127.0.0.1:" + std::to_string(h.proxy->control_port())));
    send_all(ctl.get(), "set latency 5ms match conn=1\nrules\nconns\nbogus\nkill 1 rst\n");
    ::shutdown(ctl.get(), SHUT_WR);
    auto reply = recv_until_eof(ctl.get());

    CHECK(reply.find("ok conn=1 -> latency=5ms") != std::string::npos);
    CHECK(reply.find("rules v1 (1 rule)") != std::string::npos);
    CHECK(reply.find("1 connection") != std::string::npos);
    CHECK(reply.find("faults: latency=5ms") != std::string::npos);
    CHECK(reply.find("err unknown command 'bogus'") != std::string::npos);
    CHECK(reply.find("ok connection #1 reset") != std::string::npos);
    CHECK(sees_close(client.get()));
}

TEST_CASE("control: ttl rules expire on their own") {
    Harness h;
    auto fd = h.connect();
    h.set("set latency 100ms ttl=300ms");
    CHECK(ping_ms(fd.get()) >= 200);
    std::this_thread::sleep_for(600ms);
    CHECK(h.proxy->rules().snapshot()->rules.empty());
    CHECK(ping_ms(fd.get()) < 50);
}
