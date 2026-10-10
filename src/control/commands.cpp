#include "control/commands.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <future>
#include <memory>
#include <stdexcept>

namespace chaos {

namespace {

constexpr auto kWorkerReplyTimeout = std::chrono::seconds(2);

// Runs `fn` on every worker and collects the results.
template <class T, class Fn>
std::vector<T> ask_workers(WorkerPool& pool, Fn fn) {
    std::vector<std::future<T>> futures;
    for (std::size_t i = 0; i < pool.size(); ++i) {
        auto promise = std::make_shared<std::promise<T>>();
        futures.push_back(promise->get_future());
        Worker& w = pool.at(i);
        w.post([promise, &w, fn] { promise->set_value(fn(w)); });
    }
    std::vector<T> results;
    for (auto& f : futures) {
        if (f.wait_for(kWorkerReplyTimeout) != std::future_status::ready)
            throw std::runtime_error("a worker did not respond");
        results.push_back(f.get());
    }
    return results;
}

}  // namespace

std::string CommandProcessor::help() {
    return R"(commands:
  set <faults> [match <filters>] [dir=up|down|both] [name=N] [ttl=30s]
      faults:  latency 200ms   jitter 50ms   drop 5%   stall 300ms
               throttle 64kbps (or 512KB/s, 10mbit, off)   reset 0.1%   blackhole on|off
      filters: conn=ID   ip=ADDR   cport=PORT (client source port)
      Unnamed rules are keyed by their filters, so repeated sets merge.
  rules                 list rules (later rules override earlier ones)
  del <name>            delete a rule
  clear                 delete all rules
  conns                 list connections with their effective faults
  kill <id> [rst]       close a connection (rst = send a TCP reset)
  stats                 counters
  quit
)";
}

std::string CommandProcessor::execute(std::string_view line) {
    auto words = split_words(line);
    if (words.empty()) return "\n";
    std::string_view cmd = words[0];
    std::vector<std::string_view> args(words.begin() + 1, words.end());

    try {
        if (cmd == "help" || cmd == "?") return help();

        if (cmd == "rules" || cmd == "list") return rules_.snapshot()->describe(Clock::now());

        if (cmd == "set") {
            SetCommand set = parse_set(args);
            std::string name;
            rules_.update([&](Rules& r) {
                name = apply_set(r, set, Clock::now());
                return true;
            });
            auto snap = rules_.snapshot();
            for (const auto& r : snap->rules)
                if (r.name == name) return std::format("ok {} -> {} (rules v{})\n", name, r.patch.describe(), snap->version);
            return "ok\n";
        }

        if (cmd == "del" || cmd == "delete" || cmd == "rm") {
            if (args.size() != 1) return "err usage: del <name>\n";
            bool removed = rules_.update([&](Rules& r) {
                return std::erase_if(r.rules, [&](const Rule& rule) { return rule.name == args[0]; }) > 0;
            });
            return removed ? std::format("ok deleted {}\n", args[0]) : std::format("err no rule named '{}'\n", args[0]);
        }

        if (cmd == "clear") {
            rules_.update([](Rules& r) {
                r.rules.clear();
                return true;
            });
            return "ok all rules cleared\n";
        }

        if (cmd == "conns") return cmd_conns();

        if (cmd == "kill") {
            if (args.empty() || args.size() > 2 || (args.size() == 2 && args[1] != "rst"))
                return "err usage: kill <id> [rst]\n";
            std::uint64_t id = 0;
            auto [p, ec] = std::from_chars(args[0].data(), args[0].data() + args[0].size(), id);
            if (ec != std::errc{} || p != args[0].data() + args[0].size())
                return std::format("err bad connection id '{}'\n", args[0]);
            return cmd_kill(id, args.size() == 2);
        }

        if (cmd == "stats") {
            return std::format("{}\ntimers pending {} | rules v{}\n", stats_.line(), timer_.pending(),
                               rules_.version());
        }

        return std::format("err unknown command '{}' (try: help)\n", cmd);
    } catch (const std::exception& e) {
        return std::format("err {}\n", e.what());
    }
}

std::string CommandProcessor::cmd_conns() {
    auto per_worker = ask_workers<std::vector<ConnSnapshot>>(pool_, [](Worker& w) { return w.snapshot(); });
    std::vector<ConnSnapshot> all;
    for (auto& v : per_worker) all.insert(all.end(), v.begin(), v.end());
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.id < b.id; });

    std::string out = std::format("{} connection{}\n", all.size(), all.size() == 1 ? "" : "s");
    for (const auto& c : all) {
        auto age = std::chrono::duration_cast<Millis>(c.age).count() / 1000.0;
        out += std::format("  #{:<5} {:<22} w{} {:<10} {:>7.1f}s  up {:>8} (queued {:>8})  down {:>8} (queued {:>8})\n",
                           c.id, c.peer, c.worker, c.state, age, format_bytes(c.bytes_up),
                           format_bytes(c.queued_up), format_bytes(c.bytes_down), format_bytes(c.queued_down));
        if (c.faults_up == c.faults_down) {
            out += std::format("         faults: {}\n", c.faults_up);
        } else {
            out += std::format("         up:   {}\n         down: {}\n", c.faults_up, c.faults_down);
        }
    }
    return out;
}

std::string CommandProcessor::cmd_kill(std::uint64_t id, bool rst) {
    auto found = ask_workers<bool>(pool_, [id, rst](Worker& w) { return w.kill(id, rst); });
    if (std::find(found.begin(), found.end(), true) == found.end())
        return std::format("err no connection #{}\n", id);
    return std::format("ok connection #{} {}\n", id, rst ? "reset" : "closed");
}

}  // namespace chaos
