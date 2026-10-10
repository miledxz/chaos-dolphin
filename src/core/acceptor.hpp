#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include "core/worker.hpp"
#include "net/fd.hpp"
#include "net/socket.hpp"

namespace chaos {

// Accepts client connections and hands them to the worker pool. It never
// touches the upstream: the owning worker starts a non-blocking connect, so a
// slow or dead upstream can't stall new accepts.
class Acceptor {
public:
    Acceptor(const net::Address& listen, WorkerPool& pool);
    ~Acceptor();

    void start();
    void stop();
    std::uint16_t port() const { return port_; }

private:
    void run();

    net::Fd listen_fd_;
    std::uint16_t port_;
    WorkerPool& pool_;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};

}  // namespace chaos
