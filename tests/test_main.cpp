#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "test.hpp"
#include "util/log.hpp"

namespace test {

namespace {
struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};
}  // namespace

std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

void fail(const char* file, int line, const std::string& msg) {
    throw Failure(std::format("{}:{}: {}", file, line, msg));
}

}  // namespace test

// usage: chaos_tests [substring-filter]
int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
    chaos::log::set_level(chaos::log::Level::Warn);
    const char* filter = argc > 1 ? argv[1] : nullptr;

    int passed = 0, failed = 0;
    for (const auto& c : test::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        auto start = std::chrono::steady_clock::now();
        try {
            c.fn();
            ++passed;
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
            std::printf("  ok    %-55s %5lldms\n", c.name, static_cast<long long>(ms.count()));
        } catch (const std::exception& e) {
            ++failed;
            std::printf("  FAIL  %s\n        %s\n", c.name, e.what());
        }
        std::fflush(stdout);
    }
    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
