#include "chaos/rule_store.hpp"
#include "chaos/rules.hpp"
#include "test.hpp"

using namespace chaos;
using namespace std::chrono_literals;

namespace {
SetCommand parse(std::string_view s) { return parse_set(split_words(s)); }
long long ms(Duration d) { return std::chrono::duration_cast<Millis>(d).count(); }
long long us(Duration d) { return std::chrono::duration_cast<std::chrono::microseconds>(d).count(); }
}  // namespace

TEST_CASE("parse_duration units") {
    CHECK_EQ(ms(parse_duration("200ms")), 200);
    CHECK_EQ(ms(parse_duration("200")), 200);
    CHECK_EQ(ms(parse_duration("1.5s")), 1500);
    CHECK_EQ(us(parse_duration("300us")), 300);
    CHECK_EQ(ms(parse_duration("2m")), 120000);
    CHECK_THROWS(parse_duration("fast"));
    CHECK_THROWS(parse_duration("10h"));
    CHECK_THROWS(parse_duration(""));
}

TEST_CASE("parse_rate bits vs bytes") {
    CHECK_EQ(parse_rate("64kbps"), 8000u);      // kilobits
    CHECK_EQ(parse_rate("64KBps"), 64000u);     // kilobytes
    CHECK_EQ(parse_rate("64KB/s"), 64000u);
    CHECK_EQ(parse_rate("64k"), 64000u);        // bare prefix = bytes
    CHECK_EQ(parse_rate("1mbit"), 125000u);
    CHECK_EQ(parse_rate("10mbps"), 1250000u);
    CHECK_EQ(parse_rate("512"), 512u);
    CHECK_THROWS(parse_rate("64kfoo"));
    CHECK_THROWS(parse_rate("fast"));
}

TEST_CASE("parse_probability") {
    CHECK_EQ(parse_probability("0.05"), 0.05);
    CHECK_EQ(parse_probability("5%"), 0.05);
    CHECK_EQ(parse_probability("1"), 1.0);
    CHECK_THROWS(parse_probability("1.5"));
    CHECK_THROWS(parse_probability("5x"));
}

TEST_CASE("parse_set accepts both 'key value' and 'key=value'") {
    auto cmd = parse("latency 200ms jitter=50ms match cport=5432 dir=up");
    CHECK_EQ(ms(*cmd.rule.patch.latency), 200);
    CHECK_EQ(ms(*cmd.rule.patch.jitter), 50);
    CHECK(cmd.rule.match.cport == std::optional<std::uint16_t>(5432));
    CHECK(cmd.rule.dir == DirMatch::Up);
    CHECK_EQ(cmd.rule.name, std::string("cport=5432/up"));
}

TEST_CASE("parse_set blackhole forms and errors") {
    CHECK(*parse("blackhole").rule.patch.blackhole);
    CHECK(*parse("blackhole on").rule.patch.blackhole);
    CHECK(!*parse("blackhole off").rule.patch.blackhole);
    CHECK(*parse("blackhole conn=3").rule.patch.blackhole);
    CHECK_THROWS(parse(""));
    CHECK_THROWS(parse("conn=3"));          // no fault given
    CHECK_THROWS(parse("latency"));         // missing value
    CHECK_THROWS(parse("wobble 3"));        // unknown key
    CHECK_THROWS(parse("latency 1ms dir=sideways"));
    CHECK_THROWS(parse("latency 1ms cport=70000"));
}

TEST_CASE("unnamed sets with the same filters merge") {
    Rules rules;
    auto now = Clock::now();
    apply_set(rules, parse("latency 200ms"), now);
    apply_set(rules, parse("jitter 50ms"), now);
    apply_set(rules, parse("latency 100ms"), now);
    CHECK_EQ(rules.rules.size(), 1u);
    auto f = rules.resolve({1, "127.0.0.1", 5000}, Dir::Up, now);
    CHECK_EQ(ms(f.latency), 100);
    CHECK_EQ(ms(f.jitter), 50);
}

TEST_CASE("later rules override earlier ones per field, with matching") {
    Rules rules;
    auto now = Clock::now();
    apply_set(rules, parse("latency 100ms throttle 1mbit"), now);
    apply_set(rules, parse("latency 500ms match conn=7"), now);
    apply_set(rules, parse("drop 10% dir=down ip=10.0.0.9"), now);

    ConnInfo seven{7, "127.0.0.1", 1111};
    ConnInfo other{8, "10.0.0.9", 2222};

    auto f7 = rules.resolve(seven, Dir::Up, now);
    CHECK_EQ(ms(f7.latency), 500);
    CHECK_EQ(f7.throttle_Bps, 125000u);  // inherited from the global rule
    CHECK_EQ(f7.drop, 0.0);

    auto f8up = rules.resolve(other, Dir::Up, now);
    auto f8down = rules.resolve(other, Dir::Down, now);
    CHECK_EQ(ms(f8up.latency), 100);
    CHECK_EQ(f8up.drop, 0.0);
    CHECK_EQ(f8down.drop, 0.1);
}

TEST_CASE("named rules and ttl expiry") {
    Rules rules;
    auto now = Clock::now();
    auto name = apply_set(rules, parse("name=spike latency 2s ttl=10s"), now);
    CHECK_EQ(name, std::string("spike"));
    CHECK_EQ(ms(rules.resolve({1, "a", 1}, Dir::Up, now).latency), 2000);
    CHECK_EQ(ms(rules.resolve({1, "a", 1}, Dir::Up, now + 11s).latency), 0);  // expired
    CHECK(!prune_expired(rules, now + 5s));
    CHECK(prune_expired(rules, now + 11s));
    CHECK(rules.rules.empty());
}

TEST_CASE("RuleStore publishes new versions") {
    RuleStore store;
    CHECK_EQ(store.version(), 0u);
    auto before = store.snapshot();
    store.update([](Rules& r) {
        apply_set(r, parse_set(split_words("latency 10ms")), Clock::now());
        return true;
    });
    CHECK_EQ(store.version(), 1u);
    CHECK(before->rules.empty());  // old snapshots are untouched
    CHECK_EQ(store.snapshot()->rules.size(), 1u);
    CHECK(!store.update([](Rules&) { return false; }));
    CHECK_EQ(store.version(), 1u);
}

TEST_CASE("throttle off and describe") {
    auto cmd = parse("throttle off");
    CHECK_EQ(*cmd.rule.patch.throttle_Bps, 0u);
    Faults f;
    CHECK_EQ(f.describe(), std::string("none"));
    f.latency = 200ms;
    f.throttle_Bps = 8000;
    CHECK_EQ(f.describe(), std::string("latency=200ms throttle=8.0KB/s"));
}
