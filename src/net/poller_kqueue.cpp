#if !defined(__linux__)

#include <sys/event.h>
#include <sys/time.h>

#include <algorithm>
#include <cerrno>
#include <iterator>

#include "net/poller.hpp"
#include "net/socket.hpp"

namespace chaos::net {

namespace {
constexpr uintptr_t kWakeIdent = 0;

void* to_udata(std::uint64_t token) { return reinterpret_cast<void*>(static_cast<uintptr_t>(token)); }
}  // namespace

Poller::Poller() : fd_(::kqueue()) {
    if (!fd_) throw SystemError("kqueue", errno);
    struct kevent ev;
    EV_SET(&ev, kWakeIdent, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (kevent(fd_.get(), &ev, 1, nullptr, 0, nullptr) < 0) throw SystemError("kevent(USER)", errno);
}

Poller::~Poller() = default;

void Poller::add(int fd, std::uint64_t token, Interest interest) { modify(fd, token, interest); }

void Poller::modify(int fd, std::uint64_t token, Interest interest) {
    struct kevent changes[2];
    EV_SET(&changes[0], fd, EVFILT_READ, EV_ADD | (interest.read ? EV_ENABLE : EV_DISABLE), 0, 0,
           to_udata(token));
    EV_SET(&changes[1], fd, EVFILT_WRITE, EV_ADD | (interest.write ? EV_ENABLE : EV_DISABLE), 0, 0,
           to_udata(token));
    if (kevent(fd_.get(), changes, 2, nullptr, 0, nullptr) < 0) throw SystemError("kevent(ADD)", errno);
}

void Poller::remove(int fd) {
    struct kevent changes[2];
    EV_SET(&changes[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    EV_SET(&changes[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    kevent(fd_.get(), changes, 2, nullptr, 0, nullptr);  // closing the fd removes it anyway
}

int Poller::wait(std::span<PollEvent> out, int timeout_ms) {
    struct kevent events[128];
    int max = static_cast<int>(std::min<std::size_t>(out.size(), std::size(events)));
    timespec ts{timeout_ms / 1000, (timeout_ms % 1000) * 1'000'000L};
    int n = kevent(fd_.get(), nullptr, 0, events, max, timeout_ms < 0 ? nullptr : &ts);
    if (n < 0) {
        if (errno == EINTR) return 0;
        throw SystemError("kevent(wait)", errno);
    }
    for (int i = 0; i < n; ++i) {
        const auto& ev = events[i];
        PollEvent& pe = out[i];
        pe = {};
        if (ev.filter == EVFILT_USER) {
            pe.token = kWakeToken;
            continue;
        }
        pe.token = reinterpret_cast<uintptr_t>(ev.udata);
        // EV_EOF/EV_ERROR still count as ready: the following read/write reports the condition.
        if (ev.filter == EVFILT_READ) pe.readable = true;
        if (ev.filter == EVFILT_WRITE) pe.writable = true;
    }
    return n;
}

void Poller::wakeup() {
    struct kevent ev;
    EV_SET(&ev, kWakeIdent, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    kevent(fd_.get(), &ev, 1, nullptr, 0, nullptr);
}

}  // namespace chaos::net

#endif
