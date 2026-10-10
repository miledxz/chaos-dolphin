#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "chaos/rule_store.hpp"
#include "control/commands.hpp"
#include "net/fd.hpp"
#include "net/socket.hpp"

namespace chaos {

// The control thread: a line-oriented TCP server (use `nc` or `chaosproxy ctl`).
// It also expires rules whose ttl has run out.
class ControlServer {
public:
    ControlServer(const net::Address& listen, CommandProcessor& commands, RuleStore& rules);
    ~ControlServer();

    void start();
    void stop();
    std::uint16_t port() const { return port_; }

private:
    struct Client {
        net::Fd fd;
        std::string inbuf;
        bool closing = false;
    };

    void run();
    void handle_readable(Client& c);
    void expire_rules();

    net::Fd listen_fd_;
    std::uint16_t port_;
    CommandProcessor& commands_;
    RuleStore& rules_;
    std::vector<Client> clients_;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};

}  // namespace chaos
