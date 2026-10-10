#include "control/control_server.hpp"

#include <poll.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>

#include "util/log.hpp"

namespace chaos {

namespace {
constexpr std::size_t kMaxLine = 4096;

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

void send_all(int fd, std::string_view s) {
    while (!s.empty()) {
        ssize_t n = ::send(fd, s.data(), s.size(), kSendFlags);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        s.remove_prefix(static_cast<std::size_t>(n));
    }
}
}  // namespace

ControlServer::ControlServer(const net::Address& listen, CommandProcessor& commands, RuleStore& rules)
    : listen_fd_(net::listen_tcp(listen, 16)),
      port_(net::local_port(listen_fd_.get())),
      commands_(commands),
      rules_(rules) {}

ControlServer::~ControlServer() { stop(); }

void ControlServer::start() {
    stopping_ = false;
    thread_ = std::thread([this] { run(); });
}

void ControlServer::stop() {
    stopping_ = true;
    if (thread_.joinable()) thread_.join();
    clients_.clear();
}

void ControlServer::run() {
    std::vector<pollfd> fds;
    while (!stopping_.load(std::memory_order_relaxed)) {
        fds.clear();
        fds.push_back({listen_fd_.get(), POLLIN, 0});
        for (auto& c : clients_) fds.push_back({c.fd.get(), POLLIN, 0});

        int n = ::poll(fds.data(), fds.size(), 250);
        expire_rules();
        if (n <= 0) continue;

        // Clients first: indexes in `fds` line up with clients_ before new accepts are added.
        for (std::size_t i = 1; i < fds.size(); ++i)
            if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) handle_readable(clients_[i - 1]);
        std::erase_if(clients_, [](const Client& c) { return c.closing; });

        if (fds[0].revents & POLLIN) {
            net::Fd fd(::accept(listen_fd_.get(), nullptr, nullptr));
            if (fd) {
                net::set_nosigpipe(fd.get());
                clients_.push_back({std::move(fd), {}, false});
            }
        }
    }
}

void ControlServer::handle_readable(Client& c) {
    char buf[1024];
    ssize_t n = ::recv(c.fd.get(), buf, sizeof buf, 0);
    if (n <= 0) {
        if (n < 0 && errno == EINTR) return;
        // EOF: answer any final line that had no trailing newline, then hang up.
        if (!c.inbuf.empty()) send_all(c.fd.get(), commands_.execute(c.inbuf));
        c.closing = true;
        return;
    }
    c.inbuf.append(buf, static_cast<std::size_t>(n));

    std::size_t pos;
    while ((pos = c.inbuf.find('\n')) != std::string::npos) {
        std::string line = c.inbuf.substr(0, pos);
        c.inbuf.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "quit" || line == "exit") {
            send_all(c.fd.get(), "bye\n");
            c.closing = true;
            return;
        }
        log::debug("control: {}", line);
        send_all(c.fd.get(), commands_.execute(line));
    }
    if (c.inbuf.size() > kMaxLine) {
        send_all(c.fd.get(), "err line too long\n");
        c.closing = true;
    }
}

void ControlServer::expire_rules() {
    auto now = Clock::now();
    auto snap = rules_.snapshot();
    bool any_expired = false;
    for (const auto& r : snap->rules) any_expired |= r.expired(now);
    if (!any_expired) return;
    rules_.update([&](Rules& r) { return prune_expired(r, now); });
    log::info("expired rules pruned (rules v{})", rules_.version());
}

}  // namespace chaos
