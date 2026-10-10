#pragma once

#include <chrono>
#include <cstdint>
#include <random>

namespace chaos {

using Clock = std::chrono::steady_clock;
using Duration = Clock::duration;
using Millis = std::chrono::milliseconds;

using Rng = std::mt19937_64;

// Direction of a byte stream through the proxy.
enum class Dir : std::uint8_t { Up = 0, Down = 1 };  // Up: client -> upstream

inline const char* to_string(Dir d) { return d == Dir::Up ? "up" : "down"; }

}  // namespace chaos
