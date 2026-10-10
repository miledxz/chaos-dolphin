#include "core/acceptor.hpp"

#include <poll.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>

#include "util/log.hpp"

namespace chaos {

Acceptor::Acceptor(const net::Address& listen, WorkerPool& pool)
    : listen_fd_(net::listen_tcp(listen)), port_(net::local_port(listen_fd_.get())), pool_(pool) {
    net::set_nonblocking(listen_fd_.get());
}

Acceptor::~Acceptor() { stop(); }

void Acceptor::start() {
    stopping_ = false;
    thread_ = std::thread([this] { run(); });
}

void Acceptor::stop() {
    stopping_ = true;
    if (thread_.joinable()) thread_.join();
}

void Acceptor::run() {
    while (!stopping_.load(std::memory_order_relaxed)) {
        // Short timeout so stop() is noticed promptly.
        pollfd pfd{listen_fd_.get(), POLLIN, 0};
        if (::poll(&pfd, 1, 200) <= 0) continue;

        while (true) {
            net::Address peer;
            peer.len = sizeof peer.storage;
            net::Fd client(::accept(listen_fd_.get(), peer.sa(), &peer.len));
            if (!client) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ECONNABORTED) break;
                log::warn("accept: {}", std::strerror(errno));
                if (errno == EMFILE || errno == ENFILE) std::this_thread::sleep_for(Millis(100));
                break;
            }
            try {
                net::set_nonblocking(client.get());
                net::set_nodelay(client.get());
                net::set_nosigpipe(client.get());
            } catch (const std::exception& e) {
                log::warn("accept setup: {}", e.what());
                continue;
            }
            pool_.assign(std::move(client), peer);
        }
    }
}

}  // namespace chaos
