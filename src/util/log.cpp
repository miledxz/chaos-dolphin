#include "util/log.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace chaos::log {

namespace {
std::atomic<Level> g_level{Level::Info};
std::mutex g_mutex;

const char* tag(Level level) {
    switch (level) {
        case Level::Debug: return "debug";
        case Level::Info: return "info ";
        case Level::Warn: return "warn ";
        case Level::Error: return "error";
    }
    return "?";
}
}  // namespace

void set_level(Level level) { g_level.store(level, std::memory_order_relaxed); }
Level level() { return g_level.load(std::memory_order_relaxed); }

void write(Level lvl, std::string_view msg) {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto secs = system_clock::to_time_t(now);
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm tm{};
    localtime_r(&secs, &tm);
    char ts[16];
    std::strftime(ts, sizeof ts, "%H:%M:%S", &tm);

    std::lock_guard lock(g_mutex);
    std::fprintf(stderr, "%s.%03lld %s %.*s\n", ts, static_cast<long long>(ms), tag(lvl),
                 static_cast<int>(msg.size()), msg.data());
}

}  // namespace chaos::log
