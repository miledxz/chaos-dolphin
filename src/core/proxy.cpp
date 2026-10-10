#include "core/proxy.hpp"

#include <random>
#include <stdexcept>
#include <thread>

#include "control/commands.hpp"
#include "control/control_server.hpp"
#include "core/acceptor.hpp"
#include "core/context.hpp"
#include "core/worker.hpp"
#include "util/log.hpp"

namespace chaos {

Proxy::Proxy(const Config& config)
    : seed_(config.seed ? *config.seed : std::random_device{}() | (std::uint64_t{std::random_device{}()} << 32)),
      workers_(config.workers ? config.workers : std::max(1u, std::thread::hardware_concurrency())),
      timer_([this](std::uint32_t worker, std::vector<TimerEntry> due) {
          Worker& w = pool_->at(worker);
          w.post([&w, due = std::move(due)] { w.on_timers(due); });
      }) {
    if (config.upstream.empty()) throw std::invalid_argument("an upstream address is required");
    ctx_ = std::make_unique<ProxyContext>(ProxyContext{
        net::Address::resolve(config.upstream), rules_, timer_, stats_, seed_, config.connect_timeout});
    pool_ = std::make_unique<WorkerPool>(workers_, *ctx_);
    commands_ = std::make_unique<CommandProcessor>(rules_, *pool_, stats_, timer_);

    for (const auto& cmd : config.rule_commands) {
        auto reply = commands_->execute(cmd);
        if (reply.starts_with("err")) throw std::invalid_argument("rule '" + cmd + "': " + reply.substr(4));
    }

    acceptor_ = std::make_unique<Acceptor>(net::Address::resolve(config.listen), *pool_);
    if (!config.control.empty())
        control_ = std::make_unique<ControlServer>(net::Address::resolve(config.control), *commands_, rules_);
}

Proxy::~Proxy() { stop(); }

void Proxy::start() {
    if (running_) return;
    running_ = true;
    pool_->start();
    timer_.start();
    if (control_) control_->start();
    acceptor_->start();
}

void Proxy::stop() {
    if (!running_) return;
    running_ = false;
    acceptor_->stop();         // no new connections
    if (control_) control_->stop();
    timer_.stop();             // no more wakeups posted to workers
    pool_->stop();             // closes every session
}

std::uint16_t Proxy::listen_port() const { return acceptor_->port(); }
std::uint16_t Proxy::control_port() const { return control_ ? control_->port() : 0; }

std::string Proxy::execute(std::string_view command) { return commands_->execute(command); }

}  // namespace chaos
