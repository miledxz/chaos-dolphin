#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "chaos/rules.hpp"
#include "core/proxy.hpp"
#include "net/socket.hpp"
#include "util/log.hpp"

namespace {

constexpr const char* kUsage = R"(chaosproxy - a TCP proxy that injects latency, loss, throttling and resets

usage:
  chaosproxy --upstream HOST:PORT [options]
  chaosproxy ctl [--control HOST:PORT] COMMAND...

options:
  --listen HOST:PORT    address clients connect to        (default 127.0.0.1:8000)
  --upstream HOST:PORT  the real server                   (required)
  --control HOST:PORT   control port for live rule changes (default 127.0.0.1:9100)
  --workers N           worker threads                    (default: CPU count)
  --seed N              RNG seed for repeatable fault decisions
  --connect-timeout D   give up connecting to the upstream after D     (default 5s)
  --set "RULE"          rule to apply at startup, e.g. --set "latency 200ms jitter 50ms"
  --rules FILE          file of control commands to run at startup (one per line, # comments)
  --stats SECONDS       print a stats line every N seconds (default 0 = off)
  -v, --verbose         debug logging
  -q, --quiet           warnings and errors only

examples:
  chaosproxy --upstream 127.0.0.1:5432 --listen :15432 --set "latency 100ms jitter 20ms"
  chaosproxy ctl set throttle 64kbps match cport=51234
  chaosproxy ctl conns
)";

int g_signal_pipe[2] = {-1, -1};

void on_signal(int) {
    char c = 1;
    [[maybe_unused]] auto r = ::write(g_signal_pipe[1], &c, 1);
}

[[noreturn]] void usage_error(const std::string& msg) {
    std::cerr << "chaosproxy: " << msg << "\n\n" << kUsage;
    std::exit(2);
}

// `chaosproxy ctl ...`: send one command to a running proxy and print the reply.
int run_ctl(std::vector<std::string> args) {
    std::string control = "127.0.0.1:9100";
    if (args.size() >= 2 && args[0] == "--control") {
        control = args[1];
        args.erase(args.begin(), args.begin() + 2);
    }
    std::string line;
    for (const auto& a : args) line += (line.empty() ? "" : " ") + a;
    if (line.empty()) line = "help";

    try {
        auto fd = chaos::net::connect_blocking(chaos::net::Address::resolve(control));
        line += '\n';
        if (::send(fd.get(), line.data(), line.size(), 0) < 0) throw chaos::net::SystemError("send", errno);
        ::shutdown(fd.get(), SHUT_WR);  // server replies, then closes on EOF
        char buf[4096];
        ssize_t n;
        std::string reply;
        while ((n = ::recv(fd.get(), buf, sizeof buf, 0)) > 0) reply.append(buf, static_cast<std::size_t>(n));
        std::cout << reply;
        return reply.starts_with("err") ? 1 : 0;
    } catch (const std::exception& e) {
        std::cerr << "chaosproxy ctl: " << e.what() << "\n";
        return 1;
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (!args.empty() && args[0] == "ctl") return run_ctl({args.begin() + 1, args.end()});

    // Accept "--opt=value" as well as "--opt value".
    for (std::size_t i = 0; i < args.size(); ++i) {
        auto eq = args[i].find('=');
        if (args[i].starts_with("--") && eq != std::string::npos) {
            args.insert(args.begin() + static_cast<std::ptrdiff_t>(i) + 1, args[i].substr(eq + 1));
            args[i].resize(eq);
            ++i;
        }
    }

    chaos::Config config;
    int stats_interval = 0;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto value = [&]() -> const std::string& {
            if (i + 1 >= args.size()) usage_error(a + " needs a value");
            return args[++i];
        };
        try {
            if (a == "-h" || a == "--help") {
                std::cout << kUsage;
                return 0;
            } else if (a == "--listen") config.listen = value();
            else if (a == "--upstream") config.upstream = value();
            else if (a == "--control") config.control = value();
            else if (a == "--workers") config.workers = static_cast<unsigned>(std::stoul(value()));
            else if (a == "--seed") config.seed = std::stoull(value());
            else if (a == "--connect-timeout") config.connect_timeout = chaos::parse_duration(value());
            else if (a == "--set") config.rule_commands.push_back("set " + value());
            else if (a == "--stats") stats_interval = std::stoi(value());
            else if (a == "-v" || a == "--verbose") chaos::log::set_level(chaos::log::Level::Debug);
            else if (a == "-q" || a == "--quiet") chaos::log::set_level(chaos::log::Level::Warn);
            else if (a == "--rules") {
                std::ifstream in(value());
                if (!in) usage_error("cannot open rules file " + args[i]);
                for (std::string line; std::getline(in, line);) {
                    auto start = line.find_first_not_of(" \t");
                    if (start == std::string::npos || line[start] == '#') continue;
                    config.rule_commands.push_back(line.substr(start));
                }
            } else usage_error("unknown option " + a);
        } catch (const std::logic_error&) {  // stoi, parse_duration & co.
            usage_error("bad value for " + a);
        }
    }
    if (config.upstream.empty()) usage_error("--upstream is required");

    if (::pipe(g_signal_pipe) < 0) {
        std::perror("pipe");
        return 1;
    }
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    try {
        chaos::Proxy proxy(config);
        proxy.start();
        chaos::log::info("chaosproxy listening on port {} -> {} | control port {} | {} workers | seed {}",
                         proxy.listen_port(), config.upstream, proxy.control_port(), proxy.workers(),
                         proxy.seed());
        if (!config.rule_commands.empty()) std::cerr << proxy.execute("rules");

        // Main thread: wait for a signal, printing stats periodically.
        int timeout_ms = stats_interval > 0 ? stats_interval * 1000 : -1;
        while (true) {
            pollfd pfd{g_signal_pipe[0], POLLIN, 0};
            int n = ::poll(&pfd, 1, timeout_ms);
            if (n > 0) break;
            if (n == 0) chaos::log::info("{}", proxy.stats().line());
        }
        chaos::log::info("shutting down");
        proxy.stop();
        chaos::log::info("{}", proxy.stats().line());
    } catch (const std::exception& e) {
        chaos::log::error("{}", e.what());
        return 1;
    }
    return 0;
}
