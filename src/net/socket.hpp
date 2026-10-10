#pragma once

#include <sys/socket.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "net/fd.hpp"

namespace chaos::net {

struct SystemError : std::runtime_error {
    SystemError(const std::string& what, int err);
    int code;
};

// A resolved socket address (IPv4 or IPv6).
struct Address {
    sockaddr_storage storage{};
    socklen_t len = 0;

    const sockaddr* sa() const { return reinterpret_cast<const sockaddr*>(&storage); }
    sockaddr* sa() { return reinterpret_cast<sockaddr*>(&storage); }

    std::string ip() const;
    std::uint16_t port() const;
    std::string to_string() const;  // "ip:port" or "[ip]:port"

    // Parses "host:port", "[v6]:port" or ":port" and resolves the host. Throws on failure.
    static Address resolve(std::string_view host_port);
};

void set_nonblocking(int fd);
void set_nodelay(int fd);
void set_nosigpipe(int fd);
// Arms SO_LINGER{1,0} so the next close() sends RST instead of FIN.
void set_linger_reset(int fd);

Fd listen_tcp(const Address& addr, int backlog = 512);
std::uint16_t local_port(int fd);

// Starts a non-blocking connect. The socket becomes writable when the connect
// finishes; check pending_error() to see whether it succeeded.
Fd connect_nonblocking(const Address& addr);
int pending_error(int fd);

// Blocking connect, used by the `ctl` client and tests.
Fd connect_blocking(const Address& addr);

}  // namespace chaos::net
