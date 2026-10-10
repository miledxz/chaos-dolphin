#include "net/socket.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <cerrno>
#include <cstring>
#include <format>

namespace chaos::net {

SystemError::SystemError(const std::string& what, int err)
    : std::runtime_error(std::format("{}: {}", what, std::strerror(err))), code(err) {}

std::string Address::ip() const {
    char buf[INET6_ADDRSTRLEN] = {};
    if (storage.ss_family == AF_INET) {
        auto* in = reinterpret_cast<const sockaddr_in*>(&storage);
        inet_ntop(AF_INET, &in->sin_addr, buf, sizeof buf);
    } else if (storage.ss_family == AF_INET6) {
        auto* in6 = reinterpret_cast<const sockaddr_in6*>(&storage);
        inet_ntop(AF_INET6, &in6->sin6_addr, buf, sizeof buf);
    }
    return buf;
}

std::uint16_t Address::port() const {
    if (storage.ss_family == AF_INET)
        return ntohs(reinterpret_cast<const sockaddr_in*>(&storage)->sin_port);
    if (storage.ss_family == AF_INET6)
        return ntohs(reinterpret_cast<const sockaddr_in6*>(&storage)->sin6_port);
    return 0;
}

std::string Address::to_string() const {
    if (storage.ss_family == AF_INET6) return std::format("[{}]:{}", ip(), port());
    return std::format("{}:{}", ip(), port());
}

Address Address::resolve(std::string_view host_port) {
    std::string host, port;
    if (!host_port.empty() && host_port.front() == '[') {
        auto close = host_port.find(']');
        if (close == std::string_view::npos || close + 1 >= host_port.size() ||
            host_port[close + 1] != ':')
            throw std::invalid_argument(std::format("bad address '{}'", host_port));
        host = host_port.substr(1, close - 1);
        port = host_port.substr(close + 2);
    } else {
        auto colon = host_port.rfind(':');
        if (colon == std::string_view::npos)
            throw std::invalid_argument(std::format("address '{}' needs host:port", host_port));
        host = host_port.substr(0, colon);
        port = host_port.substr(colon + 1);
    }
    if (host.empty()) host = "0.0.0.0";

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &res); rc != 0)
        throw std::invalid_argument(
            std::format("cannot resolve '{}': {}", host_port, gai_strerror(rc)));

    Address addr;
    std::memcpy(&addr.storage, res->ai_addr, res->ai_addrlen);
    addr.len = res->ai_addrlen;
    freeaddrinfo(res);
    return addr;
}

void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        throw SystemError("fcntl(O_NONBLOCK)", errno);
}

void set_nodelay(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

void set_nosigpipe([[maybe_unused]] int fd) {
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}

void set_linger_reset(int fd) {
    linger lg{1, 0};
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
}

namespace {
Fd make_socket(const Address& addr) {
    Fd fd(::socket(addr.storage.ss_family, SOCK_STREAM, 0));
    if (!fd) throw SystemError("socket", errno);
    set_nosigpipe(fd.get());
    return fd;
}
}  // namespace

Fd listen_tcp(const Address& addr, int backlog) {
    Fd fd = make_socket(addr);
    int one = 1;
    setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (::bind(fd.get(), addr.sa(), addr.len) < 0)
        throw SystemError(std::format("bind {}", addr.to_string()), errno);
    if (::listen(fd.get(), backlog) < 0) throw SystemError("listen", errno);
    return fd;
}

std::uint16_t local_port(int fd) {
    Address addr;
    addr.len = sizeof addr.storage;
    if (getsockname(fd, addr.sa(), &addr.len) < 0) throw SystemError("getsockname", errno);
    return addr.port();
}

Fd connect_nonblocking(const Address& addr) {
    Fd fd = make_socket(addr);
    set_nonblocking(fd.get());
    set_nodelay(fd.get());
    if (::connect(fd.get(), addr.sa(), addr.len) < 0 && errno != EINPROGRESS)
        throw SystemError(std::format("connect {}", addr.to_string()), errno);
    return fd;
}

int pending_error(int fd) {
    int err = 0;
    socklen_t len = sizeof err;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) return errno;
    return err;
}

Fd connect_blocking(const Address& addr) {
    Fd fd = make_socket(addr);
    if (::connect(fd.get(), addr.sa(), addr.len) < 0)
        throw SystemError(std::format("connect {}", addr.to_string()), errno);
    set_nodelay(fd.get());
    return fd;
}

}  // namespace chaos::net
