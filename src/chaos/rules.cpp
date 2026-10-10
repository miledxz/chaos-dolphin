#include "chaos/rules.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <format>
#include <stdexcept>

namespace chaos {

namespace {

[[noreturn]] void bad(std::string msg) { throw std::invalid_argument(std::move(msg)); }

// Splits "1.5ms" into {1.5, "ms"}.
std::pair<double, std::string_view> split_number(std::string_view s, std::string_view what) {
    std::size_t i = 0;
    while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.')) ++i;
    if (i == 0) bad(std::format("bad {} '{}'", what, s));
    std::string num(s.substr(0, i));  // strtod needs a terminated string
    char* end = nullptr;
    double v = std::strtod(num.c_str(), &end);
    if (end != num.c_str() + num.size() || !std::isfinite(v)) bad(std::format("bad {} '{}'", what, s));
    return {v, s.substr(i)};
}

std::uint64_t parse_uint(std::string_view s, std::string_view what) {
    std::uint64_t v = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || ptr != s.data() + s.size()) bad(std::format("bad {} '{}'", what, s));
    return v;
}

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string format_prob(double p) { return std::format("{:g}%", p * 100.0); }

}  // namespace

// ---------------------------------------------------------------- formatting

std::string format_duration(Duration d) {
    using namespace std::chrono;
    auto us = duration_cast<microseconds>(d).count();
    if (us % 1000 != 0 && us < 10'000) return std::format("{}us", us);
    if (us >= 10'000'000 && us % 1'000'000 == 0) return std::format("{}s", us / 1'000'000);
    if (us % 1000 == 0) return std::format("{}ms", us / 1000);
    return std::format("{:.1f}ms", us / 1000.0);
}

std::string format_rate(std::uint64_t bps) {
    if (bps >= 1'000'000) return std::format("{:.1f}MB/s", bps / 1e6);
    if (bps >= 1'000) return std::format("{:.1f}KB/s", bps / 1e3);
    return std::format("{}B/s", bps);
}

std::string format_bytes(std::uint64_t b) {
    if (b >= 1'000'000'000) return std::format("{:.2f}GB", b / 1e9);
    if (b >= 1'000'000) return std::format("{:.1f}MB", b / 1e6);
    if (b >= 1'000) return std::format("{:.1f}KB", b / 1e3);
    return std::format("{}B", b);
}

// ---------------------------------------------------------------- value parsers

Duration parse_duration(std::string_view s) {
    auto [v, unit] = split_number(s, "duration");
    double us;
    if (unit.empty() || unit == "ms") us = v * 1e3;
    else if (unit == "us") us = v;
    else if (unit == "s") us = v * 1e6;
    else if (unit == "m" || unit == "min") us = v * 60e6;
    else bad(std::format("bad duration unit in '{}' (use us, ms, s, m)", s));
    return std::chrono::duration_cast<Duration>(std::chrono::microseconds(std::llround(us)));
}

std::uint64_t parse_rate(std::string_view s) {
    auto [v, unit] = split_number(s, "rate");
    std::string_view u = unit;
    if (ends_with(u, "/s")) u.remove_suffix(2);
    else if (ends_with(u, "ps")) u.remove_suffix(2);

    double mult = 1;
    if (!u.empty()) {
        switch (u.front()) {
            case 'k': case 'K': mult = 1e3; u.remove_prefix(1); break;
            case 'm': case 'M': mult = 1e6; u.remove_prefix(1); break;
            case 'g': case 'G': mult = 1e9; u.remove_prefix(1); break;
            default: break;
        }
    }
    // Lowercase b / bit = bits, uppercase B or no unit = bytes.
    if (u == "b" || u == "bit" || u == "bits") mult /= 8;
    else if (!(u.empty() || u == "B")) bad(std::format("bad rate unit in '{}' (e.g. 64kbps, 512KB/s, 10mbit)", s));
    double bytes = v * mult;
    if (bytes < 1 && v > 0) bad(std::format("rate '{}' is below 1 byte/s", s));
    return static_cast<std::uint64_t>(std::llround(bytes));
}

double parse_probability(std::string_view s) {
    auto [v, unit] = split_number(s, "probability");
    if (unit == "%") v /= 100.0;
    else if (!unit.empty()) bad(std::format("bad probability '{}' (use 0.05 or 5%)", s));
    if (v < 0 || v > 1) bad(std::format("probability '{}' must be between 0 and 1", s));
    return v;
}

bool parse_bool(std::string_view s) {
    auto l = lower(s);
    if (l == "on" || l == "true" || l == "1" || l == "yes") return true;
    if (l == "off" || l == "false" || l == "0" || l == "no") return false;
    bad(std::format("expected on/off, got '{}'", s));
}

std::vector<std::string_view> split_words(std::string_view s) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        std::size_t start = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i > start) out.push_back(s.substr(start, i - start));
    }
    return out;
}

// ---------------------------------------------------------------- Faults / FaultPatch

std::string Faults::describe() const {
    FaultPatch p;
    if (latency.count()) p.latency = latency;
    if (jitter.count()) p.jitter = jitter;
    if (drop > 0) { p.drop = drop; p.stall = stall; }
    if (throttle_Bps) p.throttle_Bps = throttle_Bps;
    if (reset > 0) p.reset = reset;
    if (blackhole) p.blackhole = true;
    return p.empty() ? "none" : p.describe();
}

bool FaultPatch::empty() const {
    return !latency && !jitter && !stall && !drop && !reset && !throttle_Bps && !blackhole;
}

void FaultPatch::apply_to(Faults& f) const {
    if (latency) f.latency = *latency;
    if (jitter) f.jitter = *jitter;
    if (stall) f.stall = *stall;
    if (drop) f.drop = *drop;
    if (reset) f.reset = *reset;
    if (throttle_Bps) f.throttle_Bps = *throttle_Bps;
    if (blackhole) f.blackhole = *blackhole;
}

void FaultPatch::merge(const FaultPatch& n) {
    if (n.latency) latency = n.latency;
    if (n.jitter) jitter = n.jitter;
    if (n.stall) stall = n.stall;
    if (n.drop) drop = n.drop;
    if (n.reset) reset = n.reset;
    if (n.throttle_Bps) throttle_Bps = n.throttle_Bps;
    if (n.blackhole) blackhole = n.blackhole;
}

std::string FaultPatch::describe() const {
    std::string out;
    auto add = [&](std::string s) {
        if (!out.empty()) out += ' ';
        out += s;
    };
    if (latency) add("latency=" + format_duration(*latency));
    if (jitter) add("jitter=" + format_duration(*jitter));
    if (drop) add("drop=" + format_prob(*drop));
    if (stall) add("stall=" + format_duration(*stall));
    if (throttle_Bps) add("throttle=" + (*throttle_Bps ? format_rate(*throttle_Bps) : std::string("off")));
    if (reset) add("reset=" + format_prob(*reset));
    if (blackhole) add(std::string("blackhole=") + (*blackhole ? "on" : "off"));
    return out;
}

// ---------------------------------------------------------------- Match / Rule / Rules

bool Match::matches(const ConnInfo& c) const {
    if (conn && *conn != c.id) return false;
    if (ip && *ip != c.ip) return false;
    if (cport && *cport != c.port) return false;
    return true;
}

std::string Match::describe() const {
    if (!any()) return "all";
    std::string out;
    auto add = [&](std::string s) {
        if (!out.empty()) out += ',';
        out += s;
    };
    if (conn) add(std::format("conn={}", *conn));
    if (ip) add("ip=" + *ip);
    if (cport) add(std::format("cport={}", *cport));
    return out;
}

bool Rule::applies(const ConnInfo& c, Dir d, Clock::time_point now) const {
    if (expired(now)) return false;
    if (dir == DirMatch::Up && d != Dir::Up) return false;
    if (dir == DirMatch::Down && d != Dir::Down) return false;
    return match.matches(c);
}

Faults Rules::resolve(const ConnInfo& c, Dir d, Clock::time_point now) const {
    Faults f;
    for (const auto& r : rules)
        if (r.applies(c, d, now)) r.patch.apply_to(f);
    return f;
}

Rule* Rules::find(std::string_view name) {
    auto it = std::find_if(rules.begin(), rules.end(), [&](const Rule& r) { return r.name == name; });
    return it == rules.end() ? nullptr : &*it;
}

std::string Rules::describe(Clock::time_point now) const {
    std::string out = std::format("rules v{} ({} rule{})\n", version, rules.size(), rules.size() == 1 ? "" : "s");
    for (const auto& r : rules) {
        const char* dir = r.dir == DirMatch::Both ? "both" : r.dir == DirMatch::Up ? "up" : "down";
        out += std::format("  {:<16} match={:<20} dir={:<4}  {}", r.name, r.match.describe(), dir, r.patch.describe());
        if (r.expires) {
            auto left = std::chrono::duration_cast<Millis>(*r.expires - now);
            out += std::format("  (expires in {:.1f}s)", std::max<long long>(0, left.count()) / 1000.0);
        }
        out += '\n';
    }
    return out;
}

// ---------------------------------------------------------------- set command

SetCommand parse_set(const std::vector<std::string_view>& args) {
    SetCommand cmd;
    Rule& r = cmd.rule;

    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string_view tok = args[i];
        if (tok == "match") continue;  // purely decorative: "set throttle 64kbps match cport=5432"

        std::string key;
        std::string_view value;
        if (auto eq = tok.find('='); eq != std::string_view::npos) {
            key = lower(tok.substr(0, eq));
            value = tok.substr(eq + 1);
        } else {
            key = lower(tok);
            bool next_is_bool = false;
            if (i + 1 < args.size()) {
                auto l = lower(args[i + 1]);
                next_is_bool = l == "on" || l == "off" || l == "true" || l == "false";
            }
            if (key == "blackhole" && !next_is_bool) {
                value = "on";
            } else {
                if (i + 1 >= args.size()) bad(std::format("'{}' needs a value", tok));
                value = args[++i];
            }
        }
        if (value.empty()) bad(std::format("'{}' needs a value", key));

        auto& p = r.patch;
        if (key == "latency" || key == "delay") p.latency = parse_duration(value);
        else if (key == "jitter") p.jitter = parse_duration(value);
        else if (key == "drop" || key == "loss") p.drop = parse_probability(value);
        else if (key == "stall") p.stall = parse_duration(value);
        else if (key == "throttle" || key == "bw" || key == "bandwidth" || key == "rate")
            p.throttle_Bps = (lower(value) == "off") ? 0 : parse_rate(value);
        else if (key == "reset") p.reset = parse_probability(value);
        else if (key == "blackhole") p.blackhole = parse_bool(value);
        else if (key == "name") { r.name = value; cmd.has_name = true; }
        else if (key == "conn") { r.match.conn = parse_uint(value, "conn id"); cmd.has_match = true; }
        else if (key == "ip") { r.match.ip = std::string(value); cmd.has_match = true; }
        else if (key == "cport" || key == "port") {
            auto port = parse_uint(value, "port");
            if (port == 0 || port > 65535) bad(std::format("bad port '{}'", value));
            r.match.cport = static_cast<std::uint16_t>(port);
            cmd.has_match = true;
        } else if (key == "dir") {
            auto l = lower(value);
            if (l == "up") r.dir = DirMatch::Up;
            else if (l == "down") r.dir = DirMatch::Down;
            else if (l == "both") r.dir = DirMatch::Both;
            else bad(std::format("dir must be up, down or both, got '{}'", value));
            cmd.has_dir = true;
        } else if (key == "ttl") {
            cmd.ttl = parse_duration(value);
        } else {
            bad(std::format("unknown setting '{}'", key));
        }
    }

    if (r.patch.empty()) bad("nothing to set (try: set latency 200ms jitter 50ms)");
    if (!cmd.has_name) {
        // Unnamed rules are keyed by what they match, so repeated `set`s merge.
        r.name = r.match.describe();
        if (r.dir == DirMatch::Up) r.name += "/up";
        if (r.dir == DirMatch::Down) r.name += "/down";
    }
    return cmd;
}

std::string apply_set(Rules& rules, const SetCommand& cmd, Clock::time_point now) {
    std::optional<Clock::time_point> expires;
    if (cmd.ttl) expires = now + *cmd.ttl;

    if (Rule* existing = rules.find(cmd.rule.name)) {
        existing->patch.merge(cmd.rule.patch);
        if (cmd.has_match) existing->match = cmd.rule.match;
        if (cmd.has_dir) existing->dir = cmd.rule.dir;
        if (cmd.ttl) existing->expires = expires;
        return existing->name;
    }
    Rule r = cmd.rule;
    r.expires = expires;
    rules.rules.push_back(std::move(r));
    return rules.rules.back().name;
}

bool prune_expired(Rules& rules, Clock::time_point now) {
    auto before = rules.rules.size();
    std::erase_if(rules.rules, [&](const Rule& r) { return r.expired(now); });
    return rules.rules.size() != before;
}

}  // namespace chaos
