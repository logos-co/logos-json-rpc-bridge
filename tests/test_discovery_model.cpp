// The discovery model: live-report parsing, the status machine, backoff,
// gating, by-name translation and the JSON views. All pure.

#include <logos_test.h>

#include <string>

#include "discovery_model.h"

using namespace bridge;

namespace {

nlohmann::json j(const std::string& s) {
    return nlohmann::json::parse(s, nullptr, false);
}

BridgeConfig config(const std::string& json) {
    ConfigParseResult r = parseBridgeConfig(json, "json_rpc_bridge");
    if (!r.ok) throw LogosTestFailure("bad test config: " + r.error);
    return r.config;
}

LiveReport live(const std::string& json) {
    LiveReport r;
    if (!parseLiveReport(j(json), &r)) throw LogosTestFailure("bad live report");
    return r;
}

// A Qt-glue-shaped report: typed events, a Qt default-argument clone.
const char* kProviderIface = R"json([
  {"name":"greet","signature":"greet(QString)","returnType":"QString",
   "parameters":[{"type":"QString","name":"who"}]},
  {"name":"add","parameters":[{"name":"a"},{"name":"b"}]},
  {"name":"add","parameters":[{"name":"a"}]},
  {"name":"version","type":"method","returnType":"QString"},
  {"type":"event","name":"greeted","parameters":[{"name":"who"}]}
])json";

ModuleView resolved(const std::string& module, const std::string& iface) {
    return untypedView(pendingView(module), live(iface));
}

} // namespace

// ── live report ─────────────────────────────────────────────────────────────

LOGOS_TEST(live_report_splits_methods_from_tagged_events) {
    const LiveReport r = live(kProviderIface);
    LOGOS_ASSERT_TRUE(r.eventsDeclared);
    LOGOS_ASSERT_EQ(r.methods.size(), static_cast<size_t>(4));
    LOGOS_ASSERT_EQ(r.events.size(), static_cast<size_t>(1));
    LOGOS_ASSERT_EQ(r.method("greet")->params.at(0), std::string("who"));
    LOGOS_ASSERT_TRUE(r.hasEvent("greeted"));
    LOGOS_ASSERT_FALSE(r.hasMethod("greeted"));
}

// The first entry of a repeated name is the one by-name params use.
LOGOS_TEST(live_report_keeps_qt_default_argument_clones_in_order) {
    const LiveReport r = live(kProviderIface);
    LOGOS_ASSERT_EQ(r.method("add")->params.size(), static_cast<size_t>(2));
    LOGOS_ASSERT_EQ(r.methods[2].params.size(), static_cast<size_t>(1));
}

LOGOS_TEST(live_report_without_event_tags_cannot_know_its_events) {
    const LiveReport r = live(R"([{"name":"get"}])");
    LOGOS_ASSERT_FALSE(r.eventsDeclared);
    LOGOS_ASSERT_TRUE(r.events.empty());
}

// A malformed entry is skipped, never thrown on: this runs on a pump thread.
LOGOS_TEST(live_report_skips_malformed_entries_without_throwing) {
    const LiveReport r = live(
        R"([7, {"name":1}, {"type":5,"name":"a"}, {"name":"b","parameters":[3,{"name":2}]},)"
        R"( {"name":"c","parameters":"x"}])");
    LOGOS_ASSERT_EQ(r.methods.size(), static_cast<size_t>(3));
    LOGOS_ASSERT_EQ(r.method("b")->params.size(), static_cast<size_t>(2));
    LOGOS_ASSERT_EQ(r.method("b")->params[0], std::string());
    LOGOS_ASSERT_TRUE(r.method("c")->params.empty());
}

LOGOS_TEST(a_non_array_interface_is_not_a_live_report) {
    LiveReport r;
    LOGOS_ASSERT_FALSE(parseLiveReport(nlohmann::json(), &r));
    LOGOS_ASSERT_FALSE(parseLiveReport(j(R"({"name":"x"})"), &r));
    LOGOS_ASSERT_TRUE(parseLiveReport(j("[]"), &r));
}

// ── status machine ──────────────────────────────────────────────────────────

LOGOS_TEST(a_module_starts_pending_and_unresolved) {
    const ModuleView v = pendingView("m1");
    LOGOS_ASSERT_EQ(std::string(interfaceStatusName(v.status)), std::string("pending"));
    LOGOS_ASSERT_FALSE(v.resolved());
    LOGOS_ASSERT_EQ(v.generation, static_cast<std::uint64_t>(0));
}

LOGOS_TEST(a_live_report_moves_pending_to_untyped) {
    const ModuleView v = resolved("m1", kProviderIface);
    LOGOS_ASSERT_EQ(std::string(interfaceStatusName(v.status)), std::string("untyped"));
    LOGOS_ASSERT_TRUE(v.resolved());
    LOGOS_ASSERT_EQ(v.generation, static_cast<std::uint64_t>(1));
    LOGOS_ASSERT_FALSE(v.stale);
}

// A loss keeps the last view for gating; the next discovery replaces it.
LOGOS_TEST(a_loss_marks_a_resolved_view_stale_until_rediscovered) {
    const ModuleView lost = staleView(resolved("m1", kProviderIface));
    LOGOS_ASSERT_TRUE(lost.stale);
    LOGOS_ASSERT_TRUE(lost.live.hasMethod("greet"));
    const ModuleView again = untypedView(lost, live(R"([{"name":"other"}])"));
    LOGOS_ASSERT_FALSE(again.stale);
    LOGOS_ASSERT_EQ(again.generation, static_cast<std::uint64_t>(2));
    LOGOS_ASSERT_FALSE(again.live.hasMethod("greet"));
}

LOGOS_TEST(a_loss_before_resolution_leaves_the_view_pending) {
    const ModuleView v = staleView(pendingView("m1"));
    LOGOS_ASSERT_FALSE(v.stale);
    LOGOS_ASSERT_FALSE(v.resolved());
}

// ── backoff ─────────────────────────────────────────────────────────────────

LOGOS_TEST(retry_delay_doubles_from_half_a_second_to_a_thirty_second_cap) {
    const long expected[] = {500, 1000, 2000, 4000, 8000, 16000, 30000, 30000};
    for (int i = 0; i < 8; ++i)
        LOGOS_ASSERT_EQ(static_cast<long>(retryDelay(i + 1).count()), expected[i]);
    LOGOS_ASSERT_EQ(static_cast<long>(retryDelay(1000000).count()), 30000L);
    LOGOS_ASSERT_EQ(static_cast<long>(retryDelay(0).count()), 500L);
}

// ── gating (live names) ─────────────────────────────────────────────────────

LOGOS_TEST(an_unexposed_module_permits_nothing_in_any_state) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    LOGOS_ASSERT_FALSE(methodPermitted(cfg, pendingView("m2"), "greet"));
    LOGOS_ASSERT_FALSE(methodPermitted(cfg, resolved("m2", kProviderIface), "greet"));
    LOGOS_ASSERT_FALSE(eventPermitted(cfg, resolved("m2", kProviderIface), "greeted"));
}

// "Starting" must not look like "forbidden"; policy still applies.
LOGOS_TEST(a_pending_module_passes_policy_allowed_names_upstream) {
    const BridgeConfig cfg =
        config(R"({"expose":{"modules":[{"name":"m1","methods":{"deny":["wipe"]}}]}})");
    LOGOS_ASSERT_TRUE(methodPermitted(cfg, pendingView("m1"), "anything"));
    LOGOS_ASSERT_FALSE(methodPermitted(cfg, pendingView("m1"), "wipe"));
    LOGOS_ASSERT_TRUE(eventPermitted(cfg, pendingView("m1"), "any_event"));
}

LOGOS_TEST(a_resolved_module_permits_only_live_names_the_policy_allows) {
    const BridgeConfig cfg =
        config(R"({"expose":{"modules":[{"name":"m1","methods":{"deny":["add"]}}]}})");
    const ModuleView v = resolved("m1", kProviderIface);
    LOGOS_ASSERT_TRUE(methodPermitted(cfg, v, "greet"));
    LOGOS_ASSERT_FALSE(methodPermitted(cfg, v, "add"));
    LOGOS_ASSERT_FALSE(methodPermitted(cfg, v, "missing"));
}

// Legacy behaviour kept: an empty live list gates nothing but policy.
LOGOS_TEST(a_resolved_module_with_no_live_methods_defers_to_upstream) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    LOGOS_ASSERT_TRUE(methodPermitted(cfg, resolved("m1", "[]"), "anything"));
}

LOGOS_TEST(tagged_events_are_checked_and_untagged_ones_defer_to_policy) {
    const BridgeConfig cfg = config(
        R"({"expose":{"modules":["m1",{"name":"m2","events":{"allow":["ready"]}}]}})");
    LOGOS_ASSERT_TRUE(eventPermitted(cfg, resolved("m1", kProviderIface), "greeted"));
    LOGOS_ASSERT_FALSE(eventPermitted(cfg, resolved("m1", kProviderIface), "typo"));
    const ModuleView legacy = resolved("m2", R"([{"name":"get"}])");
    LOGOS_ASSERT_TRUE(eventPermitted(cfg, legacy, "ready"));
    LOGOS_ASSERT_FALSE(eventPermitted(cfg, legacy, "other"));
}

// Contract 5: lidl/name/version bypass the policy, but must still exist live.
LOGOS_TEST(built_ins_bypass_the_method_policy_but_not_the_live_check) {
    const BridgeConfig cfg =
        config(R"({"expose":{"modules":[{"name":"m1","methods":{"allow":["greet"]}}]}})");
    LOGOS_ASSERT_TRUE(methodPermitted(cfg, pendingView("m1"), "lidl"));
    LOGOS_ASSERT_FALSE(methodPermitted(cfg, pendingView("m1"), "add"));
    const ModuleView v = resolved("m1", kProviderIface);
    LOGOS_ASSERT_TRUE(methodPermitted(cfg, v, "version"));
    LOGOS_ASSERT_FALSE(methodPermitted(cfg, v, "lidl"));   // not listed by this module
    LOGOS_ASSERT_FALSE(methodPermitted(cfg, v, "add"));
    const nlohmann::json d = describeView(*cfg.find("m1"), v);
    LOGOS_ASSERT_EQ(d["methods"].dump(), std::string(R"(["greet","version"])"));
}

// ── by-name params ──────────────────────────────────────────────────────────

LOGOS_TEST(by_name_params_follow_the_live_order_and_fill_gaps_with_null) {
    nlohmann::json out;
    std::string bad;
    const ModuleView v = resolved("m1", kProviderIface);
    LOGOS_ASSERT_TRUE(toPositional(v, "add", j(R"({"b":2,"a":1})"), &out, &bad));
    LOGOS_ASSERT_EQ(out.dump(), std::string("[1,2]"));
    LOGOS_ASSERT_TRUE(toPositional(v, "add", j(R"({"b":2})"), &out, &bad));
    LOGOS_ASSERT_EQ(out.dump(), std::string("[null,2]"));
}

LOGOS_TEST(by_name_params_name_the_offending_key_or_method) {
    nlohmann::json out;
    std::string bad;
    const ModuleView v = resolved("m1", kProviderIface);
    LOGOS_ASSERT_FALSE(toPositional(v, "add", j(R"({"c":3})"), &out, &bad));
    LOGOS_ASSERT_EQ(bad, std::string("c"));
    LOGOS_ASSERT_FALSE(toPositional(v, "nope", j(R"({})"), &out, &bad));
    LOGOS_ASSERT_EQ(bad, std::string("nope"));
    LOGOS_ASSERT_FALSE(toPositional(pendingView("m1"), "add", j(R"({})"), &out, &bad));
    LOGOS_ASSERT_EQ(bad, std::string("add"));
}

// ── views ───────────────────────────────────────────────────────────────────

LOGOS_TEST(the_view_keeps_its_keys_and_filters_names_by_policy) {
    const BridgeConfig cfg = config(
        R"({"expose":{"modules":[{"name":"m1","methods":{"deny":["add"]},)"
        R"("events":{"deny":["greeted"]}}]}})");
    const nlohmann::json d = describeView(*cfg.find("m1"), resolved("m1", kProviderIface));
    for (const char* key : {"module", "resolved", "events_declared", "methods", "events",
                            "source", "authoritative", "interface_status", "stale"})
        LOGOS_ASSERT_TRUE(d.contains(key));
    LOGOS_ASSERT_EQ(d["methods"].dump(), std::string(R"(["greet","version"])"));
    LOGOS_ASSERT_EQ(d["events"].dump(), std::string("[]"));
    LOGOS_ASSERT_EQ(d["source"].get<std::string>(), std::string("getPluginInterface"));
    LOGOS_ASSERT_FALSE(d["authoritative"].get<bool>());
    LOGOS_ASSERT_EQ(d["interface_status"].get<std::string>(), std::string("untyped"));
}

LOGOS_TEST(a_pending_view_reports_nothing_resolved) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    const nlohmann::json d = listView(*cfg.find("m1"), pendingView("m1"));
    LOGOS_ASSERT_FALSE(d["resolved"].get<bool>());
    LOGOS_ASSERT_EQ(d["interface_status"].get<std::string>(), std::string("pending"));
    LOGOS_ASSERT_TRUE(d["methods"].empty());
}
