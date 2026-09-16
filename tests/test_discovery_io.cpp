// Discovery IO driven through a fake DiscoveryIo: calls complete when the test
// says so, posts run inline, and timers fire by hand.

#include <logos_test.h>

#include <map>
#include <string>
#include <vector>

#include "discovery.h"

using namespace bridge;

namespace {

struct FakeIo {
    struct Call {
        std::string module, method;
        DiscoveryIo::Done done;
    };
    std::vector<Call> calls;                          // started, not completed
    std::map<std::string, DiscoveryIo::Job> timers;   // key -> job
    std::vector<long> delays;                         // every schedule() delay, in order
    bool postAccepts = true;

    DiscoveryIo io() {
        DiscoveryIo io;
        io.invoke = [this](const std::string& module, const std::string& method, int,
                           DiscoveryIo::Done done) {
            calls.push_back({module, method, std::move(done)});
        };
        io.post = [this](DiscoveryIo::Job job) {
            if (!postAccepts) return false;
            job();
            return true;
        };
        io.schedule = [this](const std::string& key, std::chrono::milliseconds delay,
                             DiscoveryIo::Job job) {
            timers[key] = std::move(job);
            delays.push_back(static_cast<long>(delay.count()));
            return true;
        };
        return io;
    }

    size_t pending(const std::string& module, const std::string& method) const {
        size_t n = 0;
        for (const auto& c : calls)
            if (c.module == module && c.method == method) ++n;
        return n;
    }

    // Completes the oldest outstanding call to (module, method).
    void complete(const std::string& module, const std::string& method, CallOutcome r) {
        for (auto it = calls.begin(); it != calls.end(); ++it) {
            if (it->module != module || it->method != method) continue;
            DiscoveryIo::Done done = std::move(it->done);
            calls.erase(it);
            done(std::move(r));
            return;
        }
        throw LogosTestFailure("no outstanding " + module + "." + method);
    }

    bool fire(const std::string& key) {
        auto it = timers.find(key);
        if (it == timers.end()) return false;
        DiscoveryIo::Job job = std::move(it->second);
        timers.erase(it);
        job();
        return true;
    }
};

BridgeConfig config(const std::string& json) {
    ConfigParseResult r = parseBridgeConfig(json, "json_rpc_bridge");
    if (!r.ok) throw LogosTestFailure("bad test config: " + r.error);
    return r.config;
}

CallOutcome ok(const std::string& json) {
    return CallOutcome{nlohmann::json::parse(json), std::string()};
}

CallOutcome failed(const std::string& code) {
    return CallOutcome{nlohmann::json(), code};
}

const char* kIface = R"([{"name":"greet","parameters":[{"name":"who"}]},)"
                     R"({"type":"event","name":"greeted"}])";

std::string status(const Discovery& d, const std::string& module) {
    return interfaceStatusName(d.view(module)->status);
}

} // namespace

LOGOS_TEST(warm_up_asks_every_exposed_module_for_its_interface) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1","m2"]}})");
    FakeIo fake;
    Discovery d(&cfg, fake.io());
    LOGOS_ASSERT_EQ(status(d, "m1"), std::string("pending"));
    d.start();
    LOGOS_ASSERT_EQ(fake.pending("m1", "getPluginInterface"), static_cast<size_t>(1));
    LOGOS_ASSERT_EQ(fake.pending("m2", "getPluginInterface"), static_cast<size_t>(1));
    fake.complete("m1", "getPluginInterface", ok(kIface));
    LOGOS_ASSERT_EQ(status(d, "m1"), std::string("untyped"));
    LOGOS_ASSERT_EQ(status(d, "m2"), std::string("pending"));
    LOGOS_ASSERT_TRUE(d.methodPermitted("m1", "greet"));
    LOGOS_ASSERT_FALSE(d.methodPermitted("m1", "missing"));
    LOGOS_ASSERT_TRUE(d.eventPermitted("m1", "greeted"));
}

LOGOS_TEST(an_unreachable_module_stays_pending_and_retries_with_backoff) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    FakeIo fake;
    Discovery d(&cfg, fake.io());
    d.start();
    fake.complete("m1", "getPluginInterface", failed("object_unavailable"));
    LOGOS_ASSERT_EQ(status(d, "m1"), std::string("pending"));
    LOGOS_ASSERT_TRUE(fake.fire("m1"));
    fake.complete("m1", "getPluginInterface", failed("timeout"));
    LOGOS_ASSERT_TRUE(fake.fire("m1"));
    fake.complete("m1", "getPluginInterface", ok("null"));   // not an array: no report
    LOGOS_ASSERT_TRUE(fake.fire("m1"));
    fake.complete("m1", "getPluginInterface", ok(kIface));
    LOGOS_ASSERT_EQ(status(d, "m1"), std::string("untyped"));
    LOGOS_ASSERT_EQ(fake.delays.size(), static_cast<size_t>(3));
    LOGOS_ASSERT_EQ(fake.delays[0], 500L);
    LOGOS_ASSERT_EQ(fake.delays[1], 1000L);
    LOGOS_ASSERT_EQ(fake.delays[2], 2000L);
    LOGOS_ASSERT_TRUE(fake.timers.empty());
}

LOGOS_TEST(a_provider_loss_marks_the_view_stale_and_rediscovers_it) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    FakeIo fake;
    Discovery d(&cfg, fake.io());
    d.start();
    fake.complete("m1", "getPluginInterface", ok(kIface));
    d.onProviderLost("m1");
    LOGOS_ASSERT_TRUE(d.view("m1")->stale);
    LOGOS_ASSERT_TRUE(d.methodPermitted("m1", "greet"));   // gated by the last view meanwhile
    LOGOS_ASSERT_EQ(fake.delays.back(), 500L);
    LOGOS_ASSERT_TRUE(fake.fire("m1"));
    fake.complete("m1", "getPluginInterface", failed("object_unavailable"));
    LOGOS_ASSERT_TRUE(d.view("m1")->stale);
    LOGOS_ASSERT_EQ(fake.delays.back(), 1000L);
    LOGOS_ASSERT_TRUE(fake.fire("m1"));
    fake.complete("m1", "getPluginInterface", ok(R"([{"name":"renamed"}])"));
    const auto v = d.view("m1");
    LOGOS_ASSERT_FALSE(v->stale);
    LOGOS_ASSERT_EQ(v->generation, static_cast<std::uint64_t>(2));
    LOGOS_ASSERT_TRUE(d.methodPermitted("m1", "renamed"));
    LOGOS_ASSERT_FALSE(d.methodPermitted("m1", "greet"));
}

// An answer that left before the loss describes the provider that went away.
LOGOS_TEST(a_completion_from_before_a_loss_is_dropped) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    FakeIo fake;
    Discovery d(&cfg, fake.io());
    d.start();
    d.onProviderLost("m1");
    fake.complete("m1", "getPluginInterface", ok(kIface));
    LOGOS_ASSERT_EQ(status(d, "m1"), std::string("pending"));
    LOGOS_ASSERT_TRUE(fake.fire("m1"));
    fake.complete("m1", "getPluginInterface", ok(kIface));
    LOGOS_ASSERT_EQ(status(d, "m1"), std::string("untyped"));
}

LOGOS_TEST(a_re_established_provider_is_rediscovered_at_once) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    FakeIo fake;
    Discovery d(&cfg, fake.io());
    d.start();
    fake.complete("m1", "getPluginInterface", ok(kIface));
    d.onProviderArmed("m1", 1);   // the arm that goes with the first subscribe
    d.onProviderArmed("m1", 1);   // a replay
    LOGOS_ASSERT_TRUE(fake.timers.empty());
    d.onProviderLost("m1");
    d.onProviderArmed("m1", 2);
    LOGOS_ASSERT_EQ(fake.delays.back(), 0L);
    LOGOS_ASSERT_TRUE(fake.fire("m1"));
    LOGOS_ASSERT_EQ(fake.pending("m1", "getPluginInterface"), static_cast<size_t>(1));
    fake.complete("m1", "getPluginInterface", ok(kIface));
    LOGOS_ASSERT_FALSE(d.view("m1")->stale);
}

// The loss watcher only exists for subscribed modules; a failed call covers the rest.
LOGOS_TEST(an_unavailable_call_marks_the_view_stale_once) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    FakeIo fake;
    Discovery d(&cfg, fake.io());
    d.start();
    fake.complete("m1", "getPluginInterface", ok(kIface));
    d.onCallUnavailable("m1");
    d.onCallUnavailable("m1");
    LOGOS_ASSERT_TRUE(d.view("m1")->stale);
    LOGOS_ASSERT_EQ(fake.delays.size(), static_cast<size_t>(1));
    LOGOS_ASSERT_TRUE(fake.fire("m1"));
    d.onCallUnavailable("m1");   // a refresh is already in flight
    LOGOS_ASSERT_EQ(fake.delays.size(), static_cast<size_t>(1));
    fake.complete("m1", "getPluginInterface", ok(kIface));
    LOGOS_ASSERT_FALSE(d.view("m1")->stale);
}

LOGOS_TEST(a_stopping_pump_drops_late_completions) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    FakeIo fake;
    Discovery d(&cfg, fake.io());
    d.start();
    fake.postAccepts = false;
    fake.complete("m1", "getPluginInterface", ok(kIface));
    LOGOS_ASSERT_EQ(status(d, "m1"), std::string("pending"));
    LOGOS_ASSERT_TRUE(fake.timers.empty());
}

LOGOS_TEST(signals_for_an_unexposed_module_are_ignored) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    FakeIo fake;
    Discovery d(&cfg, fake.io());
    d.onProviderLost("zz");
    d.onProviderArmed("zz", 3);
    d.onCallUnavailable("zz");
    d.refresh("zz");
    LOGOS_ASSERT_TRUE(fake.timers.empty());
    LOGOS_ASSERT_TRUE(fake.calls.empty());
    LOGOS_ASSERT_TRUE(d.describe("zz").is_null());
    LOGOS_ASSERT_FALSE(d.methodPermitted("zz", "greet"));
    LOGOS_ASSERT_EQ(d.listModules().size(), static_cast<size_t>(1));
}
