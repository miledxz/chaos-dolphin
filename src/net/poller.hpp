#pragma once

#include <cstdint>
#include <span>

#include "net/fd.hpp"

namespace chaos::net {

struct Interest {
    bool read = false;
    bool write = false;
    bool operator==(const Interest&) const = default;
};

struct PollEvent {
    std::uint64_t token = 0;
    bool readable = false;
    bool writable = false;
};

// Level-triggered readiness poller: kqueue on BSD/macOS, epoll on Linux.
// Every method except wakeup() must be called from the owning thread only.
class Poller {
public:
    // Token reported for events raised by wakeup().
    static constexpr std::uint64_t kWakeToken = ~std::uint64_t{0};

    Poller();
    ~Poller();
    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

    void add(int fd, std::uint64_t token, Interest interest);
    void modify(int fd, std::uint64_t token, Interest interest);
    void remove(int fd);

    // Waits up to timeout_ms (-1 = forever). Returns the number of events written.
    int wait(std::span<PollEvent> out, int timeout_ms);

    // Thread-safe: makes a blocked wait() return with a kWakeToken event.
    void wakeup();

private:
    Fd fd_;
#ifdef __linux__
    Fd wake_fd_;  // eventfd
#endif
};

}  // namespace chaos::net
