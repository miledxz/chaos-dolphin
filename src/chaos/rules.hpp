#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "util/clock.hpp"

namespace chaos {

// The fully resolved faults applied to one direction of one connection.
struct Faults {
    Duration latency{0};
    Duration jitter{0};              // uniform +/- around latency
    double drop = 0.0;               // probability a chunk is "lost" and stalls for `stall`
    Duration stall{Millis{300}};     // retransmit-like penalty for a dropped chunk
    std::uint64_t throttle_Bps = 0;  // bytes per second, 0 = unlimited
    double reset = 0.0;              // probability per chunk of resetting the connection
    bool blackhole = false;          // swallow bytes instead of forwarding them

    bool operator==(const Faults&) const = default;
    std::string describe() const;  // "latency=200ms jitter=50ms" or "none"
};

// A partial set of fault fields; rules layer patches onto the default Faults.
struct FaultPatch {
    std::optional<Duration> latency, jitter, stall;
    std::optional<double> drop, reset;
    std::optional<std::uint64_t> throttle_Bps;
    std::optional<bool> blackhole;

    bool empty() const;
    void apply_to(Faults& f) const;
    void merge(const FaultPatch& newer);  // fields set in `newer` win
    std::string describe() const;
};

struct ConnInfo {
    std::uint64_t id = 0;
    std::string ip;
    std::uint16_t port = 0;
};

struct Match {
    std::optional<std::uint64_t> conn;
    std::optional<std::string> ip;
    std::optional<std::uint16_t> cport;  // client (source) port

    bool any() const { return conn || ip || cport; }
    bool matches(const ConnInfo& c) const;
    std::string describe() const;  // "all" or "conn=3,ip=10.0.0.1"
    bool operator==(const Match&) const = default;
};

enum class DirMatch : std::uint8_t { Both, Up, Down };

struct Rule {
    std::string name;
    Match match;
    DirMatch dir = DirMatch::Both;
    FaultPatch patch;
    std::optional<Clock::time_point> expires;

    bool applies(const ConnInfo& c, Dir d, Clock::time_point now) const;
    bool expired(Clock::time_point now) const { return expires && *expires <= now; }
};

// Immutable once published by the RuleStore.
struct Rules {
    std::uint64_t version = 0;
    std::vector<Rule> rules;  // later rules override earlier ones, field by field

    Faults resolve(const ConnInfo& c, Dir d, Clock::time_point now) const;
    Rule* find(std::string_view name);
    std::string describe(Clock::time_point now) const;
};

// A parsed `set` command. `has_*` flags say which parts were given explicitly,
// so `set` on an existing rule can merge instead of replace.
struct SetCommand {
    Rule rule;
    bool has_name = false;
    bool has_match = false;
    bool has_dir = false;
    std::optional<Duration> ttl;
};

// Parses the arguments after `set`, e.g. "latency 200ms jitter=50ms match cport=5432 dir=up".
// Throws std::invalid_argument with a user-facing message.
SetCommand parse_set(const std::vector<std::string_view>& args);

// Merges a `set` into the rule list (by name) and returns the affected rule's name.
std::string apply_set(Rules& rules, const SetCommand& cmd, Clock::time_point now);

// Removes expired rules. Returns true if anything was removed.
bool prune_expired(Rules& rules, Clock::time_point now);

// Value parsers, exposed for tests. All throw std::invalid_argument.
Duration parse_duration(std::string_view s);    // "200ms", "1.5s", "300us", "2m"; bare = ms
std::uint64_t parse_rate(std::string_view s);   // bytes/s: "64kbps"=8000, "64KB/s"=64000, "1mbit"
double parse_probability(std::string_view s);   // "0.05" or "5%"
bool parse_bool(std::string_view s);

std::string format_rate(std::uint64_t bytes_per_sec);
std::string format_bytes(std::uint64_t bytes);
std::string format_duration(Duration d);

std::vector<std::string_view> split_words(std::string_view s);

}  // namespace chaos
