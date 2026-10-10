#pragma once

// A tiny self-contained test harness, so the project builds with no downloads.

#include <format>
#include <functional>
#include <string>
#include <vector>

namespace test {

struct Case {
    const char* name;
    std::function<void()> fn;
};

std::vector<Case>& registry();
void fail(const char* file, int line, const std::string& msg);

struct Register {
    Register(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

}  // namespace test

#define TEST_CAT2(a, b) a##b
#define TEST_CAT(a, b) TEST_CAT2(a, b)

#define TEST_CASE(name)                                                         \
    static void TEST_CAT(test_fn_, __LINE__)();                                 \
    static ::test::Register TEST_CAT(test_reg_, __LINE__)(name, TEST_CAT(test_fn_, __LINE__)); \
    static void TEST_CAT(test_fn_, __LINE__)()

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) ::test::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)

#define CHECK_EQ(a, b)                                                                         \
    do {                                                                                       \
        auto&& va_ = (a);                                                                      \
        auto&& vb_ = (b);                                                                      \
        if (!(va_ == vb_))                                                                     \
            ::test::fail(__FILE__, __LINE__, std::format("CHECK_EQ({}, {}): {} != {}", #a, #b, va_, vb_)); \
    } while (0)

#define CHECK_THROWS(expr)                                                         \
    do {                                                                           \
        bool threw_ = false;                                                       \
        try {                                                                      \
            (void)(expr);                                                          \
        } catch (...) {                                                            \
            threw_ = true;                                                         \
        }                                                                          \
        if (!threw_) ::test::fail(__FILE__, __LINE__, "CHECK_THROWS(" #expr ")"); \
    } while (0)
