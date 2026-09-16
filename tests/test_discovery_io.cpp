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

// ── typed discovery through lidl() ──────────────────────────────────────────

namespace {

const char* kTypedIface = R"json([
  {"name":"greet","parameters":[{"name":"who"}]},
  {"name":"name","type":"method","returnType":"QString"},
  {"name":"version","type":"method","returnType":"QString"},
  {"name":"lidl","returnType":"QString","parameters":[]},
  {"type":"event","name":"greeted","parameters":[{"name":"who"}]}
])json";

const char* kContract =
    "module m1 {\n  version \"1.0.0\"\n  depends []\n\n  method greet(who: tstr) -> tstr\n\n"
    "  event greeted(who: tstr)\n}\n";

CallOutcome text(const std::string& s) {
    return CallOutcome{nlohmann::json(s), std::string()};
}

// Drives m1 through getPluginInterface, lidl and (when asked) version.
void discoverTyped(FakeIo& fake, const CallOutcome& lidlAnswer, const char* iface = kTypedIface,
                   const CallOutcome& versionAnswer = CallOutcome{nlohmann::json("1.0.0"), ""}) {
    fake.complete("m1", "getPluginInterface", ok(iface));
    fake.complete("m1", "lidl", lidlAnswer);
    if (fake.pending("m1", "version")) fake.complete("m1", "version", versionAnswer);
}

} // namespace

LOGOS_TEST(a_module_with_lidl_becomes_typed) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    FakeIo fake;
    Discovery d(&cfg, fake.io());
    d.start();
    fake.complete("m1", "getPluginInterface", ok(kTypedIface));
    LOGOS_ASSERT_EQ(status(d, "m1"), std::string("pending"));   // lidl() still outstanding
    LOGOS_ASSERT_EQ(fake.pending("m1", "lidl"), static_cast<size_t>(1));
    fake.complete("m1", "lidl", text(kContract));
    LOGOS_ASSERT_EQ(status(d, "m1"), std::string("pending"));   // version() for the cross-check
    fake.complete("m1", "version", text("1.0.0"));
    LOGOS_ASSERT_EQ(status(d, "m1"), std::string("ok"));
    const nlohmann::json schema = d.describe("m1");
    LOGOS_ASSERT_EQ(schema["source"].get<std::string>(), std::string("lidl"));
    LOGOS_ASSERT_EQ(schema["interface"]["name"].get<std::string>(), std::string("m1"));
    LOGOS_ASSERT_EQ(schema["interface"]["methods"].size(), static_cast<size_t>(4));
    LOGOS_ASSERT_FALSE(d.listModules()[0].contains("interface"));
    LOGOS_ASSERT_TRUE(d.methodPermitted("m1", "greet"));
    LOGOS_ASSERT_TRUE(d.methodPermitted("m1", "lidl"));
    LOGOS_ASSERT_TRUE(d.eventPermitted("m1", "greeted"));
    nlohmann::json out;
    std::string bad;
    LOGOS_ASSERT_FALSE(d.toPositional("m1", "greet", nlohmann::json::object(), &out, &bad));
    LOGOS_ASSERT_EQ(bad, std::string("who"));
    LOGOS_ASSERT_TRUE(fake.timers.empty());
}

LOGOS_TEST(a_module_without_lidl_is_untyped_and_never_asked) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    FakeIo fake;
    Discovery d(&cfg, fake.io());
    d.start();
    fake.complete("m1", "getPluginInterface", ok(kIface));
    LOGOS_ASSERT_EQ(status(d, "m1"), std::string("untyped"));
    LOGOS_ASSERT_TRUE(fake.calls.empty());
}

LOGOS_TEST(a_lidl_that_takes_arguments_is_not_the_built_in) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    FakeIo fake;
    Discovery d(&cfg, fake.io());
    d.start();
    fake.complete("m1", "getPluginInterface",
                  ok(R"([{"name":"lidl","parameters":[{"name":"path"}]}])"));
    LOGOS_ASSERT_EQ(status(d, "m1"), std::string("untyped"));
    LOGOS_ASSERT_TRUE(fake.calls.empty());
}

// A failed call is the only transient reason, so it alone is retried.
LOGOS_TEST(a_lidl_timeout_is_invalid_and_retried) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    FakeIo fake;
    Discovery d(&cfg, fake.io());
    d.start();
    discoverTyped(fake, failed("timeout"));
    LOGOS_ASSERT_EQ(status(d, "m1"), std::string("invalid"));
    LOGOS_ASSERT_EQ(d.describe("m1")["interface_error"].get<std::string>(),
                    std::string("lidl() call failed"));
    LOGOS_ASSERT_TRUE(d.methodPermitted("m1", "greet"));   // falls back to live names
    LOGOS_ASSERT_EQ(fake.delays.back(), 500L);
    LOGOS_ASSERT_TRUE(fake.fire("m1"));
    discoverTyped(fake, failed("object_unavailable"));
    LOGOS_ASSERT_EQ(fake.delays.back(), 1000L);
    LOGOS_ASSERT_TRUE(fake.fire("m1"));
    discoverTyped(fake, text(kContract));
    LOGOS_ASSERT_EQ(status(d, "m1"), std::string("ok"));
    LOGOS_ASSERT_TRUE(fake.timers.empty());
}

LOGOS_TEST(unusable_lidl_answers_are_invalid_with_constant_reasons) {
    const struct { CallOutcome answer; const char* reason; } cases[] = {
        {ok(R"({"lidl":"module m1 {}"})"), "lidl() did not return a string"},
        {ok("null"), "lidl() did not return a string"},
        {text(std::string(kMaxContractBytes + 1, ' ')), "lidl() returned more than 4 MiB"},
        {text("module m1 {\xc3\x28}"), "lidl() returned invalid UTF-8"},
    };
    for (const auto& c : cases) {
        const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
        FakeIo fake;
        Discovery d(&cfg, fake.io());
        d.start();
        discoverTyped(fake, c.answer);
        LOGOS_ASSERT_EQ(status(d, "m1"), std::string("invalid"));
        LOGOS_ASSERT_EQ(d.describe("m1")["interface_error"].get<std::string>(),
                        std::string(c.reason));
        LOGOS_ASSERT_TRUE(fake.timers.empty());   // deterministic: not retried
    }
}

LOGOS_TEST(a_contract_that_does_not_parse_is_invalid_with_its_position) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    FakeIo fake;
    Discovery d(&cfg, fake.io());
    d.start();
    discoverTyped(fake, text("module m1 {\n  method (\n}\n"));
    LOGOS_ASSERT_EQ(status(d, "m1"), std::string("invalid"));
    const std::string why = d.describe("m1")["interface_error"].get<std::string>();
    LOGOS_ASSERT_EQ(why.rfind("2:", 0), static_cast<size_t>(0));
    LOGOS_ASSERT_CONTAINS(why, "(lidl reader " + lidlReaderRev() + ")");
}

LOGOS_TEST(a_contract_for_another_module_is_invalid) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    FakeIo fake;
    Discovery d(&cfg, fake.io());
    d.start();
    discoverTyped(fake, text("module other {\n  depends []\n}\n"));
    LOGOS_ASSERT_EQ(status(d, "m1"), std::string("invalid"));
    LOGOS_ASSERT_CONTAINS(d.describe("m1")["interface_error"].get<std::string>(), "'other'");
    LOGOS_ASSERT_FALSE(d.describe("m1").contains("interface"));
}

// A reload can swap a typed build for an untyped one and back.
LOGOS_TEST(rediscovery_after_a_loss_follows_the_reloaded_build) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    FakeIo fake;
    Discovery d(&cfg, fake.io());
    d.start();
    discoverTyped(fake, text(kContract));
    LOGOS_ASSERT_EQ(status(d, "m1"), std::string("ok"));

    d.onProviderLost("m1");
    LOGOS_ASSERT_TRUE(fake.fire("m1"));
    fake.complete("m1", "getPluginInterface", ok(kIface));   // an older, untyped build
    LOGOS_ASSERT_EQ(status(d, "m1"), std::string("untyped"));
    LOGOS_ASSERT_FALSE(d.describe("m1").contains("interface"));

    d.onProviderLost("m1");
    d.onProviderArmed("m1", 2);
    LOGOS_ASSERT_TRUE(fake.fire("m1"));
    discoverTyped(fake, text(kContract));
    LOGOS_ASSERT_EQ(status(d, "m1"), std::string("ok"));
    LOGOS_ASSERT_FALSE(d.view("m1")->stale);
    LOGOS_ASSERT_EQ(d.view("m1")->generation, static_cast<std::uint64_t>(3));
}

// A lidl() answer from before the loss belongs to the old provider.
LOGOS_TEST(a_contract_that_arrives_after_a_loss_is_dropped) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    FakeIo fake;
    Discovery d(&cfg, fake.io());
    d.start();
    fake.complete("m1", "getPluginInterface", ok(kTypedIface));
    d.onProviderLost("m1");
    fake.complete("m1", "lidl", text(kContract));
    LOGOS_ASSERT_EQ(status(d, "m1"), std::string("pending"));
}

LOGOS_TEST(reserved_introspection_is_refused_while_pending) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    FakeIo fake;
    Discovery d(&cfg, fake.io());
    LOGOS_ASSERT_FALSE(d.methodPermitted("m1", "getPluginInterface"));
    LOGOS_ASSERT_TRUE(d.methodPermitted("m1", "anything"));
    d.start();
    discoverTyped(fake, text(kContract));
    LOGOS_ASSERT_FALSE(d.methodPermitted("m1", "getPluginInterface"));
}

// ── cross-check, digests and exposure through discovery ─────────────────────

LOGOS_TEST(a_typed_view_serves_both_digests_and_a_consistent_check) {
    const BridgeConfig cfg = config(
        R"({"expose":{"modules":[{"name":"m1","methods":{"allow":[]}}]}})");
    FakeIo fake;
    Discovery d(&cfg, fake.io());
    d.start();
    discoverTyped(fake, text(kContract));
    const nlohmann::json schema = d.describe("m1");
    LOGOS_ASSERT_EQ(schema["interface_status"].get<std::string>(), std::string("ok"));
    LOGOS_ASSERT_EQ(schema["contract_sha256"].get<std::string>(), sha256Hex(kContract));
    LOGOS_ASSERT_EQ(schema["interface_sha256"].get<std::string>(),
                    sha256Hex(schema["interface"].dump()));
    LOGOS_ASSERT_EQ(schema["cross_check"]["state"].get<std::string>(), std::string("consistent"));
    LOGOS_ASSERT_EQ(schema["exposure"]["methods"].dump(),
                    std::string(R"(["name","version","lidl"])"));
    LOGOS_ASSERT_EQ(schema["exposure"]["events"].dump(), std::string(R"(["greeted"])"));
    const nlohmann::json listed = d.listModules()[0];
    LOGOS_ASSERT_EQ(listed["interface_sha256"], schema["interface_sha256"]);
    LOGOS_ASSERT_EQ(listed["exposure"], schema["exposure"]);
}

LOGOS_TEST(an_unanswered_version_is_only_an_info_finding) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    FakeIo fake;
    Discovery d(&cfg, fake.io());
    d.start();
    discoverTyped(fake, text(kContract), kTypedIface, failed("timeout"));
    LOGOS_ASSERT_EQ(status(d, "m1"), std::string("ok"));
    const nlohmann::json cc = d.describe("m1")["cross_check"];
    LOGOS_ASSERT_EQ(cc["state"].get<std::string>(), std::string("consistent"));
    LOGOS_ASSERT_EQ(cc["findings"][0]["code"].get<std::string>(),
                    std::string("runtime_version_unavailable"));
}

LOGOS_TEST(a_version_that_disagrees_is_a_warning) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    FakeIo fake;
    Discovery d(&cfg, fake.io());
    d.start();
    discoverTyped(fake, text(kContract), kTypedIface, text("9.9.9"));
    LOGOS_ASSERT_EQ(status(d, "m1"), std::string("ok"));
    LOGOS_ASSERT_EQ(d.describe("m1")["cross_check"]["state"].get<std::string>(),
                    std::string("warnings"));
}

// The contract does not describe the running binary: types are not served.
LOGOS_TEST(a_contract_the_module_does_not_implement_is_invalid_with_findings) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    FakeIo fake;
    Discovery d(&cfg, fake.io());
    d.start();
    discoverTyped(fake, text(kContract),
                  R"([{"name":"lidl"},{"name":"other"},{"type":"event","name":"greeted"}])");
    LOGOS_ASSERT_EQ(status(d, "m1"), std::string("invalid"));
    const nlohmann::json schema = d.describe("m1");
    LOGOS_ASSERT_EQ(schema["interface_error"].get<std::string>(),
                    std::string(kInconsistentContract));
    LOGOS_ASSERT_EQ(schema["cross_check"]["state"].get<std::string>(), std::string("inconsistent"));
    LOGOS_ASSERT_EQ(schema["contract_sha256"].get<std::string>(), sha256Hex(kContract));
    LOGOS_ASSERT_TRUE(schema["interface_sha256"].is_null());
    LOGOS_ASSERT_FALSE(schema.contains("interface"));
    // Live-name gating and exposure take over.
    LOGOS_ASSERT_TRUE(d.methodPermitted("m1", "other"));
    LOGOS_ASSERT_FALSE(d.methodPermitted("m1", "greet"));
    LOGOS_ASSERT_EQ(schema["exposure"]["methods"].dump(), std::string(R"(["lidl","other"])"));
    LOGOS_ASSERT_TRUE(fake.timers.empty());
}

// Bytes were read, so their digest is known even when they do not parse.
LOGOS_TEST(an_unparseable_contract_still_reports_its_bytes_digest) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    FakeIo fake;
    Discovery d(&cfg, fake.io());
    d.start();
    discoverTyped(fake, text("module m1 {"));
    const nlohmann::json schema = d.describe("m1");
    LOGOS_ASSERT_EQ(schema["contract_sha256"].get<std::string>(), sha256Hex("module m1 {"));
    LOGOS_ASSERT_TRUE(schema["cross_check"].is_null());
    LOGOS_ASSERT_TRUE(fake.calls.empty());   // no version() without a contract
}

LOGOS_TEST(a_version_answer_after_a_loss_is_dropped) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    FakeIo fake;
    Discovery d(&cfg, fake.io());
    d.start();
    fake.complete("m1", "getPluginInterface", ok(kTypedIface));
    fake.complete("m1", "lidl", text(kContract));
    d.onProviderLost("m1");
    fake.complete("m1", "version", text("1.0.0"));
    LOGOS_ASSERT_EQ(status(d, "m1"), std::string("pending"));
}

LOGOS_TEST(an_untyped_view_is_exposed_from_its_live_names) {
    const BridgeConfig cfg = config(
        R"({"expose":{"modules":[{"name":"m1","methods":{"deny":["greet"]}}]}})");
    FakeIo fake;
    Discovery d(&cfg, fake.io());
    d.start();
    fake.complete("m1", "getPluginInterface",
                  ok(R"([{"name":"greet"},{"name":"get"},{"type":"event","name":"greeted"}])"));
    const nlohmann::json listed = d.listModules()[0];
    LOGOS_ASSERT_EQ(listed["exposure"]["methods"].dump(), std::string(R"(["get"])"));
    LOGOS_ASSERT_EQ(listed["exposure"]["events"].dump(), std::string(R"(["greeted"])"));
    LOGOS_ASSERT_TRUE(listed["cross_check"].is_null());
    LOGOS_ASSERT_TRUE(listed["contract_sha256"].is_null());
}
