#if defined(__linux__)

#include <sys/epoll.h>
#include <sys/eventfd.h>

#include <algorithm>
#include <cerrno>
#include <iterator>
#include <cstdint>

#include "net/poller.hpp"
#include "net/socket.hpp"

namespace chaos::net {

namespace {
std::uint32_t to_mask(Interest interest) {
    std::uint32_t mask = EPOLLRDHUP;
    if (interest.read) mask |= EPOLLIN;
    if (interest.write) mask |= EPOLLOUT;
    return mask;
}
}  // namespace

Poller::Poller() : fd_(::epoll_create1(EPOLL_CLOEXEC)), wake_fd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
    if (!fd_) throw SystemError("epoll_create1", errno);
    if (!wake_fd_) throw SystemError("eventfd", errno);
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.u64 = kWakeToken;
    if (epoll_ctl(fd_.get(), EPOLL_CTL_ADD, wake_fd_.get(), &ev) < 0)
        throw SystemError("epoll_ctl(eventfd)", errno);
}

Poller::~Poller() = default;

void Poller::add(int fd, std::uint64_t token, Interest interest) {
    epoll_event ev{};
    ev.events = to_mask(interest);
    ev.data.u64 = token;
    if (epoll_ctl(fd_.get(), EPOLL_CTL_ADD, fd, &ev) < 0) throw SystemError("epoll_ctl(ADD)", errno);
}

void Poller::modify(int fd, std::uint64_t token, Interest interest) {
    epoll_event ev{};
    ev.events = to_mask(interest);
    ev.data.u64 = token;
    if (epoll_ctl(fd_.get(), EPOLL_CTL_MOD, fd, &ev) < 0) throw SystemError("epoll_ctl(MOD)", errno);
}

void Poller::remove(int fd) { epoll_ctl(fd_.get(), EPOLL_CTL_DEL, fd, nullptr); }

int Poller::wait(std::span<PollEvent> out, int timeout_ms) {
    epoll_event events[128];
    int max = static_cast<int>(std::min<std::size_t>(out.size(), std::size(events)));
    int n = epoll_wait(fd_.get(), events, max, timeout_ms);
    if (n < 0) {
        if (errno == EINTR) return 0;
        throw SystemError("epoll_wait", errno);
    }
    for (int i = 0; i < n; ++i) {
        PollEvent& pe = out[i];
        pe = {};
        pe.token = events[i].data.u64;
        if (pe.token == kWakeToken) {
            std::uint64_t drained;
            [[maybe_unused]] auto r = ::read(wake_fd_.get(), &drained, sizeof drained);
            continue;
        }
        std::uint32_t e = events[i].events;
        // Errors and hangups surface through the next read/write, so report both as ready.
        bool fault = e & (EPOLLERR | EPOLLHUP);
        pe.readable = (e & (EPOLLIN | EPOLLRDHUP)) || fault;
        pe.writable = (e & EPOLLOUT) || fault;
    }
    return n;
}

void Poller::wakeup() {
    std::uint64_t one = 1;
    [[maybe_unused]] auto r = ::write(wake_fd_.get(), &one, sizeof one);
}

}  // namespace chaos::net

#endif
