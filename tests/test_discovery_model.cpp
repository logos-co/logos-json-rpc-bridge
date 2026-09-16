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

// ── typed discovery: presence and the lidl() answer ─────────────────────────

LOGOS_TEST(lidl_presence_is_a_zero_parameter_method_of_any_return_spelling) {
    LOGOS_ASSERT_TRUE(lidlPresent(live(R"([{"name":"lidl","returnType":"QString"}])")));
    LOGOS_ASSERT_TRUE(lidlPresent(live(R"([{"name":"lidl","returnType":"tstr","parameters":[]}])")));
    LOGOS_ASSERT_FALSE(lidlPresent(live(R"([{"name":"lidl","parameters":[{"name":"x"}]}])")));
    LOGOS_ASSERT_FALSE(lidlPresent(live(R"([{"type":"event","name":"lidl"}])")));
    LOGOS_ASSERT_FALSE(lidlPresent(live(kProviderIface)));
    // A Qt clone with parameters does not hide a zero-parameter entry.
    LOGOS_ASSERT_TRUE(lidlPresent(
        live(R"([{"name":"lidl","parameters":[{"name":"x"}]},{"name":"lidl"}])")));
}

LOGOS_TEST(utf8_validation_rejects_malformed_sequences) {
    LOGOS_ASSERT_TRUE(isValidUtf8(""));
    LOGOS_ASSERT_TRUE(isValidUtf8("plain ascii"));
    LOGOS_ASSERT_TRUE(isValidUtf8("en dash \xe2\x80\x93 and \xf0\x9f\x98\x80"));
    LOGOS_ASSERT_TRUE(isValidUtf8("\xed\x9f\xbf"));        // U+D7FF
    LOGOS_ASSERT_TRUE(isValidUtf8("\xf4\x8f\xbf\xbf"));    // U+10FFFF
    LOGOS_ASSERT_FALSE(isValidUtf8("\xff"));
    LOGOS_ASSERT_FALSE(isValidUtf8("\x80"));
    LOGOS_ASSERT_FALSE(isValidUtf8("\xc0\xaf"));           // overlong '/'
    LOGOS_ASSERT_FALSE(isValidUtf8("\xe0\x80\xaf"));       // overlong
    LOGOS_ASSERT_FALSE(isValidUtf8("\xed\xa0\x80"));       // surrogate U+D800
    LOGOS_ASSERT_FALSE(isValidUtf8("\xf4\x90\x80\x80"));   // past U+10FFFF
    LOGOS_ASSERT_FALSE(isValidUtf8("\xe2\x80"));           // truncated
    LOGOS_ASSERT_FALSE(isValidUtf8("ok\xe2\x28\xa1"));     // bad continuation
}

LOGOS_TEST(a_lidl_answer_is_text_or_a_constant_reason) {
    const LidlText good = lidlText(true, nlohmann::json("module m {\n}\n"));
    LOGOS_ASSERT_TRUE(good.ok);
    LOGOS_ASSERT_EQ(good.text, std::string("module m {\n}\n"));

    const LidlText failed = lidlText(false, nlohmann::json("/nix/store/leak"));
    LOGOS_ASSERT_FALSE(failed.ok);
    LOGOS_ASSERT_TRUE(failed.retryable);
    LOGOS_ASSERT_EQ(failed.error, std::string("lidl() call failed"));

    const LidlText notString = lidlText(true, j(R"({"text":"x"})"));
    LOGOS_ASSERT_FALSE(notString.ok);
    LOGOS_ASSERT_FALSE(notString.retryable);
    LOGOS_ASSERT_EQ(notString.error, std::string("lidl() did not return a string"));
    LOGOS_ASSERT_FALSE(lidlText(true, nlohmann::json()).ok);

    const LidlText huge = lidlText(true, nlohmann::json(std::string(kMaxContractBytes + 1, 'x')));
    LOGOS_ASSERT_EQ(huge.error, std::string("lidl() returned more than 4 MiB"));
    LOGOS_ASSERT_TRUE(lidlText(true, nlohmann::json(std::string(kMaxContractBytes, 'x'))).ok);

    const LidlText bad = lidlText(true, nlohmann::json(std::string("module \xff")));
    LOGOS_ASSERT_EQ(bad.error, std::string("lidl() returned invalid UTF-8"));
}

LOGOS_TEST(a_contract_error_names_its_position_and_the_reader) {
    ContractResult parse;
    parse.error = "expected identifier";
    parse.errorLine = 3;
    parse.errorCol = 10;
    LOGOS_ASSERT_EQ(contractErrorText(parse, "9df8e00"),
                    std::string("3:10: expected identifier (lidl reader 9df8e00)"));
    ContractResult other;
    other.error = "the contract declares module 'a', not 'b'";
    LOGOS_ASSERT_EQ(contractErrorText(other, "9df8e00"),
                    std::string("the contract declares module 'a', not 'b' (lidl reader 9df8e00)"));
}

// ── typed discovery: status machine, gating, views ──────────────────────────

namespace {

// greet(who, ?style), plus the injected built-ins; one event.
std::shared_ptr<const TypedContract> contractFor(const std::string& module) {
    ContractResult c;
    c.ok = true;
    c.contractVersion = "1.2.3";
    c.methods = {
        {"greet", false, {{"who", false}, {"style", true}}},
        {"internalReset", false, {}},
        {"name", true, {}}, {"version", true, {}}, {"lidl", true, {}},
    };
    c.events = {{"greeted", {{"who", false}}}};
    c.astJson = std::string(R"({"name":")") + module + R"(","methods":[],"events":[]})";
    return typedContract(c);
}

const char* kTypedIface = R"json([
  {"name":"greet","parameters":[{"name":"who"},{"name":"style"}]},
  {"name":"internalReset"},
  {"name":"undeclaredExtra"},
  {"name":"name"},{"name":"version"},{"name":"lidl","returnType":"QString"},
  {"type":"event","name":"greeted","parameters":[{"name":"who"}]}
])json";

ModuleView typed(const std::string& module) {
    return okView(pendingView(module), live(kTypedIface), contractFor(module));
}

} // namespace

LOGOS_TEST(a_typed_contract_parses_its_ast_and_rejects_garbage) {
    LOGOS_ASSERT_TRUE(contractFor("m1") != nullptr);
    LOGOS_ASSERT_EQ(contractFor("m1")->interface["name"].get<std::string>(), std::string("m1"));
    ContractResult broken;
    broken.astJson = "not json";
    LOGOS_ASSERT_TRUE(typedContract(broken) == nullptr);
    broken.astJson = "[1]";
    LOGOS_ASSERT_TRUE(typedContract(broken) == nullptr);
}

LOGOS_TEST(the_status_machine_moves_between_typed_and_untyped_across_reloads) {
    ModuleView v = pendingView("m1");
    v = okView(v, live(kTypedIface), contractFor("m1"));
    LOGOS_ASSERT_EQ(std::string(interfaceStatusName(v.status)), std::string("ok"));
    LOGOS_ASSERT_TRUE(v.typed());
    v = staleView(v);
    LOGOS_ASSERT_TRUE(v.stale);
    LOGOS_ASSERT_TRUE(v.typed());   // gating keeps the contract until rediscovery
    v = untypedView(v, live(kProviderIface));   // reloaded from an older build
    LOGOS_ASSERT_EQ(std::string(interfaceStatusName(v.status)), std::string("untyped"));
    LOGOS_ASSERT_FALSE(v.typed());
    LOGOS_ASSERT_TRUE(v.contract == nullptr);
    v = okView(staleView(v), live(kTypedIface), contractFor("m1"));
    LOGOS_ASSERT_TRUE(v.typed());
    LOGOS_ASSERT_FALSE(v.stale);
    LOGOS_ASSERT_EQ(v.generation, static_cast<std::uint64_t>(3));
}

LOGOS_TEST(an_invalid_view_keeps_its_live_report_and_its_reason) {
    const ModuleView v = invalidView(pendingView("m1"), live(kTypedIface), "lidl() call failed");
    LOGOS_ASSERT_EQ(std::string(interfaceStatusName(v.status)), std::string("invalid"));
    LOGOS_ASSERT_FALSE(v.typed());
    LOGOS_ASSERT_EQ(v.interfaceError, std::string("lidl() call failed"));
    LOGOS_ASSERT_TRUE(v.live.hasMethod("undeclaredExtra"));
}

// Typed: callable iff declared AND permitted; the live list no longer decides.
LOGOS_TEST(a_typed_module_gates_calls_by_its_declarations_and_policy) {
    const BridgeConfig cfg = config(
        R"({"expose":{"modules":[{"name":"m1","methods":{"deny":["internalReset"]}}]}})");
    const ModuleView v = typed("m1");
    LOGOS_ASSERT_TRUE(methodPermitted(cfg, v, "greet"));
    LOGOS_ASSERT_FALSE(methodPermitted(cfg, v, "internalReset"));   // declared, denied
    LOGOS_ASSERT_FALSE(methodPermitted(cfg, v, "undeclaredExtra"));  // live only
    LOGOS_ASSERT_FALSE(methodPermitted(cfg, v, "missing"));
    for (const char* b : {"name", "version", "lidl"})
        LOGOS_ASSERT_TRUE(methodPermitted(cfg, v, b));
}

LOGOS_TEST(a_typed_allow_list_still_leaves_the_built_ins_callable) {
    const BridgeConfig cfg =
        config(R"({"expose":{"modules":[{"name":"m1","methods":{"allow":["greet"]}}]}})");
    const ModuleView v = typed("m1");
    LOGOS_ASSERT_TRUE(methodPermitted(cfg, v, "lidl"));
    LOGOS_ASSERT_TRUE(methodPermitted(cfg, v, "greet"));
    LOGOS_ASSERT_FALSE(methodPermitted(cfg, v, "internalReset"));
}

// Non-typed statuses keep today's live-name gating; built-ins still need to exist.
LOGOS_TEST(an_invalid_module_gates_by_live_names) {
    const BridgeConfig cfg =
        config(R"({"expose":{"modules":[{"name":"m1","methods":{"allow":["greet"]}}]}})");
    const ModuleView v = invalidView(pendingView("m1"), live(kTypedIface), "bad");
    LOGOS_ASSERT_TRUE(methodPermitted(cfg, v, "lidl"));
    LOGOS_ASSERT_FALSE(methodPermitted(cfg, v, "undeclaredExtra"));   // not allowed
    const ModuleView bare = invalidView(pendingView("m1"), live(R"([{"name":"greet"}])"), "bad");
    LOGOS_ASSERT_FALSE(methodPermitted(cfg, bare, "lidl"));
}

LOGOS_TEST(a_typed_module_gates_events_by_its_declarations) {
    const BridgeConfig cfg = config(
        R"({"expose":{"modules":["m1",{"name":"m2","events":{"deny":["greeted"]}}]}})");
    LOGOS_ASSERT_TRUE(eventPermitted(cfg, typed("m1"), "greeted"));
    LOGOS_ASSERT_FALSE(eventPermitted(cfg, typed("m1"), "undeclared"));
    LOGOS_ASSERT_FALSE(eventPermitted(cfg, typed("m2"), "greeted"));
    // Untagged live events do not matter once the contract declares them.
    const ModuleView untagged = okView(pendingView("m1"), live(R"([{"name":"greet"}])"),
                                       contractFor("m1"));
    LOGOS_ASSERT_TRUE(eventPermitted(cfg, untagged, "greeted"));
    LOGOS_ASSERT_FALSE(eventPermitted(cfg, untagged, "other"));
}

// ModuleProxy answers these before its gate: refused in every state, any policy.
LOGOS_TEST(reserved_introspection_is_refused_in_every_state) {
    const BridgeConfig cfg = config(
        R"({"expose":{"modules":[{"name":"m1","methods":{"allow":)"
        R"(["getPluginInterface","getPluginMethods","getPluginEvents","greet"]}}]}})");
    const char* listed = R"([{"name":"getPluginInterface"},{"name":"getPluginMethods"},)"
                         R"({"name":"getPluginEvents"},{"name":"greet"}])";
    const ModuleView states[] = {
        pendingView("m1"),
        resolved("m1", listed),
        invalidView(pendingView("m1"), live(listed), "bad"),
        okView(pendingView("m1"), live(listed), contractFor("m1")),
        staleView(resolved("m1", listed)),
    };
    for (const ModuleView& v : states) {
        for (const char* m : {"getPluginInterface", "getPluginMethods", "getPluginEvents"})
            LOGOS_ASSERT_FALSE(methodPermitted(cfg, v, m));
        LOGOS_ASSERT_TRUE(methodPermitted(cfg, v, "greet"));
    }
}

LOGOS_TEST(typed_by_name_params_follow_the_declarations) {
    nlohmann::json out;
    std::string bad;
    const ModuleView v = typed("m1");
    LOGOS_ASSERT_TRUE(toPositional(v, "greet", j(R"({"style":"loud","who":"w"})"), &out, &bad));
    LOGOS_ASSERT_EQ(out.dump(), std::string(R"(["w","loud"])"));
    // A missing optional param is null; an explicit null passes through.
    LOGOS_ASSERT_TRUE(toPositional(v, "greet", j(R"({"who":"w"})"), &out, &bad));
    LOGOS_ASSERT_EQ(out.dump(), std::string(R"(["w",null])"));
    LOGOS_ASSERT_TRUE(toPositional(v, "greet", j(R"({"who":null})"), &out, &bad));
    LOGOS_ASSERT_TRUE(toPositional(v, "lidl", j("{}"), &out, &bad));
    LOGOS_ASSERT_EQ(out.dump(), std::string("[]"));
}

LOGOS_TEST(typed_by_name_params_name_what_is_wrong) {
    nlohmann::json out;
    std::string bad;
    const ModuleView v = typed("m1");
    LOGOS_ASSERT_FALSE(toPositional(v, "greet", j(R"({"style":"loud"})"), &out, &bad));
    LOGOS_ASSERT_EQ(bad, std::string("who"));   // missing required
    LOGOS_ASSERT_FALSE(toPositional(v, "greet", j(R"({"who":"w","colour":1})"), &out, &bad));
    LOGOS_ASSERT_EQ(bad, std::string("colour"));   // unknown name
    LOGOS_ASSERT_FALSE(toPositional(v, "undeclaredExtra", j("{}"), &out, &bad));
    LOGOS_ASSERT_EQ(bad, std::string("undeclaredExtra"));
}

LOGOS_TEST(the_schema_view_serves_the_contract_and_the_list_view_does_not) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    const ModuleView v = typed("m1");
    const nlohmann::json schema = describeView(*cfg.find("m1"), v);
    LOGOS_ASSERT_EQ(schema["interface_status"].get<std::string>(), std::string("ok"));
    LOGOS_ASSERT_EQ(schema["source"].get<std::string>(), std::string("lidl"));
    LOGOS_ASSERT_FALSE(schema["authoritative"].get<bool>());
    LOGOS_ASSERT_EQ(schema["interface"]["name"].get<std::string>(), std::string("m1"));
    LOGOS_ASSERT_FALSE(schema.contains("interface_error"));
    const nlohmann::json listed = listView(*cfg.find("m1"), v);
    LOGOS_ASSERT_FALSE(listed.contains("interface"));
    LOGOS_ASSERT_EQ(listed["interface_status"].get<std::string>(), std::string("ok"));
    LOGOS_ASSERT_EQ(listed["source"].get<std::string>(), std::string("lidl"));
}

// The contract is public: a denied member is still in `interface`.
LOGOS_TEST(the_served_contract_is_never_filtered_by_policy) {
    const BridgeConfig all = config(R"({"expose":{"modules":["m1"]}})");
    const BridgeConfig some = config(
        R"({"expose":{"modules":[{"name":"m1","methods":{"allow":[]},"events":{"allow":[]}}]}})");
    const ModuleView v = typed("m1");
    LOGOS_ASSERT_EQ(describeView(*some.find("m1"), v)["interface"].dump(),
                    describeView(*all.find("m1"), v)["interface"].dump());
}

LOGOS_TEST(invalid_and_untyped_views_carry_no_contract) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    const nlohmann::json bad =
        describeView(*cfg.find("m1"), invalidView(pendingView("m1"), live("[]"), "why"));
    LOGOS_ASSERT_EQ(bad["interface_status"].get<std::string>(), std::string("invalid"));
    LOGOS_ASSERT_EQ(bad["interface_error"].get<std::string>(), std::string("why"));
    LOGOS_ASSERT_FALSE(bad.contains("interface"));
    LOGOS_ASSERT_EQ(bad["source"].get<std::string>(), std::string("getPluginInterface"));
    const nlohmann::json plain = describeView(*cfg.find("m1"), resolved("m1", kProviderIface));
    LOGOS_ASSERT_FALSE(plain.contains("interface"));
    LOGOS_ASSERT_FALSE(plain.contains("interface_error"));
}

// ── cross-check ─────────────────────────────────────────────────────────────

namespace {

// greet(who), notify(), the built-ins; events greeted(who), idle().
std::shared_ptr<TypedContract> checkedContract() {
    auto c = std::make_shared<TypedContract>();
    c->version = "1.0.0";
    c->methods = {{"greet", false, {{"who", false}}}, {"notify", false, {}},
                  {"name", true, {}}, {"version", true, {}}, {"lidl", true, {}}};
    c->events = {{"greeted", {{"who", false}}}, {"idle", {}}};
    c->interface = nlohmann::json::object();
    c->interfaceSha256 = interfaceDigest(c->interface);
    return c;
}

const char* kMatchingIface = R"json([
  {"name":"greet","parameters":[{"name":"who"}]},{"name":"notify"},
  {"name":"name"},{"name":"version"},{"name":"lidl"},
  {"type":"event","name":"greeted","parameters":[{"name":"who"}]},
  {"type":"event","name":"idle"}
])json";

RuntimeVersion runtime(const std::string& v) { return RuntimeVersion{true, v}; }

bool hasFinding(const CrossCheck& cc, const std::string& severity, const std::string& code,
                const std::string& member = "") {
    for (const auto& f : cc.findings)
        if (f.severity == severity && f.code == code && (member.empty() || f.member == member))
            return true;
    return false;
}

std::string describe(const CrossCheck& cc) { return cc.toJson().dump(); }

} // namespace

LOGOS_TEST(a_matching_module_is_consistent) {
    const CrossCheck cc = crossCheck(*checkedContract(), live(kMatchingIface), runtime("1.0.0"));
    LOGOS_ASSERT_EQ(std::string(cc.state()), std::string("consistent"));
    if (!cc.findings.empty()) throw LogosTestFailure("unexpected findings: " + describe(cc));
}

LOGOS_TEST(a_declared_method_missing_live_is_an_error) {
    const CrossCheck cc = crossCheck(*checkedContract(),
        live(R"([{"name":"greet","parameters":[{"name":"who"}]},{"name":"lidl"},)"
             R"({"type":"event","name":"greeted","parameters":[{"name":"who"}]},)"
             R"({"type":"event","name":"idle"}])"), runtime("1.0.0"));
    LOGOS_ASSERT_TRUE(hasFinding(cc, "error", "declared_method_missing", "notify"));
    LOGOS_ASSERT_EQ(std::string(cc.state()), std::string("inconsistent"));
    // name/version are answered by the host without being listed: info only.
    LOGOS_ASSERT_TRUE(hasFinding(cc, "info", "derived_method_unlisted", "name"));
    LOGOS_ASSERT_TRUE(hasFinding(cc, "info", "derived_method_unlisted", "version"));
    LOGOS_ASSERT_FALSE(hasFinding(cc, "error", "declared_method_missing", "name"));
}

LOGOS_TEST(an_arity_mismatch_is_an_error_for_methods_and_events) {
    const CrossCheck cc = crossCheck(*checkedContract(), live(R"json([
      {"name":"greet","parameters":[{"name":"who"},{"name":"extra"}]},
      {"name":"notify","parameters":[{"name":"x"}]},
      {"name":"name"},{"name":"version"},{"name":"lidl"},
      {"type":"event","name":"greeted"},{"type":"event","name":"idle"}
    ])json"), runtime("1.0.0"));
    LOGOS_ASSERT_TRUE(hasFinding(cc, "error", "method_arity_mismatch", "greet"));
    LOGOS_ASSERT_TRUE(hasFinding(cc, "error", "method_arity_mismatch", "notify"));
    LOGOS_ASSERT_TRUE(hasFinding(cc, "error", "event_arity_mismatch", "greeted"));
    LOGOS_ASSERT_TRUE(cc.hasErrors());
}

// A Qt default-argument clone lists the same name at several arities.
LOGOS_TEST(one_matching_arity_among_clones_is_enough) {
    const CrossCheck cc = crossCheck(*checkedContract(), live(R"json([
      {"name":"greet","parameters":[{"name":"who"},{"name":"loud"}]},
      {"name":"greet","parameters":[{"name":"who"}]},
      {"name":"notify"},{"name":"name"},{"name":"version"},{"name":"lidl"},
      {"type":"event","name":"greeted","parameters":[{"name":"who"}]},
      {"type":"event","name":"idle"}
    ])json"), runtime("1.0.0"));
    LOGOS_ASSERT_FALSE(cc.hasErrors());
}

LOGOS_TEST(a_declared_event_missing_live_is_an_error_only_when_events_are_tagged) {
    const CrossCheck tagged = crossCheck(*checkedContract(), live(R"json([
      {"name":"greet","parameters":[{"name":"who"}]},{"name":"notify"},
      {"name":"name"},{"name":"version"},{"name":"lidl"},
      {"type":"event","name":"greeted","parameters":[{"name":"who"}]}
    ])json"), runtime("1.0.0"));
    LOGOS_ASSERT_TRUE(hasFinding(tagged, "error", "declared_event_missing", "idle"));

    const CrossCheck untagged = crossCheck(*checkedContract(), live(R"json([
      {"name":"greet","parameters":[{"name":"who"}]},{"name":"notify"},
      {"name":"name"},{"name":"version"},{"name":"lidl"}
    ])json"), runtime("1.0.0"));
    LOGOS_ASSERT_FALSE(untagged.hasErrors());
    LOGOS_ASSERT_TRUE(hasFinding(untagged, "info", "events_untagged"));
    LOGOS_ASSERT_EQ(std::string(untagged.state()), std::string("consistent"));
}

LOGOS_TEST(undeclared_live_members_are_warnings_but_host_names_are_not) {
    const CrossCheck cc = crossCheck(*checkedContract(), live(R"json([
      {"name":"greet","parameters":[{"name":"who"}]},{"name":"notify"},
      {"name":"name"},{"name":"version"},{"name":"lidl"},
      {"name":"debugDump"},{"name":"debugDump","parameters":[{"name":"x"}]},
      {"name":"getPluginInterface"},
      {"type":"event","name":"greeted","parameters":[{"name":"who"}]},
      {"type":"event","name":"idle"},{"type":"event","name":"extraEvent"}
    ])json"), runtime("1.0.0"));
    LOGOS_ASSERT_TRUE(hasFinding(cc, "warning", "undeclared_method", "debugDump"));
    LOGOS_ASSERT_TRUE(hasFinding(cc, "warning", "undeclared_event", "extraEvent"));
    LOGOS_ASSERT_FALSE(hasFinding(cc, "warning", "undeclared_method", "getPluginInterface"));
    LOGOS_ASSERT_EQ(std::string(cc.state()), std::string("warnings"));
    size_t dumps = 0;
    for (const auto& f : cc.findings) dumps += f.member == "debugDump";
    LOGOS_ASSERT_EQ(dumps, static_cast<size_t>(1));
}

// Names are compared where the host gave them; types never are.
LOGOS_TEST(differing_param_names_are_a_warning) {
    const CrossCheck cc = crossCheck(*checkedContract(), live(R"json([
      {"name":"greet","parameters":[{"name":"person","type":"QString"}]},{"name":"notify"},
      {"name":"name"},{"name":"version"},{"name":"lidl"},
      {"type":"event","name":"greeted","parameters":[{"type":"int"}]},
      {"type":"event","name":"idle"}
    ])json"), runtime("1.0.0"));
    LOGOS_ASSERT_TRUE(hasFinding(cc, "warning", "param_names_differ", "greet"));
    LOGOS_ASSERT_FALSE(hasFinding(cc, "warning", "param_names_differ", "greeted"));
    LOGOS_ASSERT_FALSE(cc.hasErrors());
}

LOGOS_TEST(reader_warnings_and_lint_are_warnings) {
    auto c = checkedContract();
    c->warnings = {"non_canonical"};
    c->lint = {"Redundant nested optional in x"};
    const CrossCheck cc = crossCheck(*c, live(kMatchingIface), runtime("1.0.0"));
    LOGOS_ASSERT_TRUE(hasFinding(cc, "warning", "non_canonical"));
    LOGOS_ASSERT_TRUE(hasFinding(cc, "warning", "contract_lint"));
    LOGOS_ASSERT_EQ(std::string(cc.state()), std::string("warnings"));
}

LOGOS_TEST(the_runtime_version_is_compared_or_reported_unavailable) {
    const CrossCheck mismatch = crossCheck(*checkedContract(), live(kMatchingIface), runtime("1.1.0"));
    LOGOS_ASSERT_TRUE(hasFinding(mismatch, "warning", "version_mismatch", "version"));
    LOGOS_ASSERT_CONTAINS(describe(mismatch), "contract 1.0.0, runtime 1.1.0");

    const CrossCheck unknown = crossCheck(*checkedContract(), live(kMatchingIface), RuntimeVersion{});
    LOGOS_ASSERT_TRUE(hasFinding(unknown, "info", "runtime_version_unavailable"));
    LOGOS_ASSERT_EQ(std::string(unknown.state()), std::string("consistent"));

    auto unversioned = checkedContract();
    unversioned->version.clear();
    LOGOS_ASSERT_TRUE(crossCheck(*unversioned, live(kMatchingIface), runtime("1.0.0")).findings.empty());

    LOGOS_ASSERT_FALSE(runtimeVersion(false, nlohmann::json("1.0.0")).known);
    LOGOS_ASSERT_FALSE(runtimeVersion(true, nlohmann::json(1)).known);
    LOGOS_ASSERT_EQ(runtimeVersion(true, nlohmann::json("2.0")).value, std::string("2.0"));
}

LOGOS_TEST(findings_serialise_with_stable_keys) {
    const CrossCheck cc = crossCheck(*checkedContract(), live("[]"), RuntimeVersion{});
    const nlohmann::json j = cc.toJson();
    LOGOS_ASSERT_EQ(j["state"].get<std::string>(), std::string("inconsistent"));
    for (const auto& f : j["findings"]) {
        LOGOS_ASSERT_TRUE(f.contains("severity"));
        LOGOS_ASSERT_TRUE(f.contains("code"));
        LOGOS_ASSERT_TRUE(f.contains("detail"));
    }
    Finding general{"info", "events_untagged", "", "x"};
    LOGOS_ASSERT_FALSE(general.toJson().contains("member"));
}

// Errors make the module invalid, with the findings kept; otherwise it is ok.
LOGOS_TEST(the_checked_view_is_ok_or_invalid_by_its_findings) {
    const ModuleView good = checkedView(pendingView("m1"), live(kMatchingIface),
                                        checkedContract(), runtime("1.0.0"), "abc123");
    LOGOS_ASSERT_TRUE(good.typed());
    LOGOS_ASSERT_TRUE(good.crossChecked);
    LOGOS_ASSERT_EQ(good.contractSha256, std::string("abc123"));
    LOGOS_ASSERT_EQ(good.interfaceSha256, checkedContract()->interfaceSha256);

    const ModuleView bad = checkedView(pendingView("m1"), live(R"([{"name":"lidl"}])"),
                                       checkedContract(), runtime("1.0.0"), "abc123");
    LOGOS_ASSERT_EQ(std::string(interfaceStatusName(bad.status)), std::string("invalid"));
    LOGOS_ASSERT_EQ(bad.interfaceError, std::string(kInconsistentContract));
    LOGOS_ASSERT_TRUE(bad.crossCheck.hasErrors());
    LOGOS_ASSERT_EQ(bad.contractSha256, std::string("abc123"));
    LOGOS_ASSERT_TRUE(bad.interfaceSha256.empty());
    LOGOS_ASSERT_TRUE(bad.contract == nullptr);
}

// ── views: digests, cross-check, exposure ───────────────────────────────────

LOGOS_TEST(every_view_carries_the_new_keys_in_both_shapes) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    const ExposedModule& em = *cfg.find("m1");
    const ModuleView states[] = {
        pendingView("m1"),
        resolved("m1", kProviderIface),
        invalidView(pendingView("m1"), live("[]"), "why"),
        withExposure(em, checkedView(pendingView("m1"), live(kMatchingIface), checkedContract(),
                                     runtime("1.0.0"), "abc123")),
    };
    for (const ModuleView& v : states) {
        for (const nlohmann::json& d : {describeView(em, v), listView(em, v)}) {
            for (const char* key : {"exposure", "interface_sha256", "contract_sha256",
                                    "cross_check", "interface_status", "source"})
                LOGOS_ASSERT_TRUE(d.contains(key));
            LOGOS_ASSERT_TRUE(d["exposure"]["methods"].is_array());
            LOGOS_ASSERT_TRUE(d["exposure"]["events"].is_array());
        }
    }
    const nlohmann::json pending = describeView(em, states[0]);
    LOGOS_ASSERT_TRUE(pending["interface_sha256"].is_null());
    LOGOS_ASSERT_TRUE(pending["contract_sha256"].is_null());
    LOGOS_ASSERT_TRUE(pending["cross_check"].is_null());

    const nlohmann::json ok = describeView(em, states[3]);
    LOGOS_ASSERT_EQ(ok["contract_sha256"].get<std::string>(), std::string("abc123"));
    LOGOS_ASSERT_EQ(ok["interface_sha256"].get<std::string>(), sha256Hex("{}"));
    LOGOS_ASSERT_EQ(ok["cross_check"]["state"].get<std::string>(), std::string("consistent"));
    LOGOS_ASSERT_EQ(ok["exposure"]["methods"].dump(),
                    std::string(R"(["greet","notify","name","version","lidl"])"));
    LOGOS_ASSERT_EQ(listView(em, states[3])["interface_sha256"], ok["interface_sha256"]);
}

// interface_sha256 is a property of the contract, not of this bridge's policy.
LOGOS_TEST(the_digests_do_not_depend_on_policy) {
    const BridgeConfig open = config(R"({"expose":{"modules":["m1"]}})");
    const BridgeConfig closed = config(
        R"({"expose":{"modules":[{"name":"m1","methods":{"allow":[]},"events":{"allow":[]}}]}})");
    const ModuleView v = checkedView(pendingView("m1"), live(kMatchingIface), checkedContract(),
                                     runtime("1.0.0"), "abc123");
    const nlohmann::json a = describeView(*open.find("m1"), withExposure(*open.find("m1"), v));
    const nlohmann::json b = describeView(*closed.find("m1"), withExposure(*closed.find("m1"), v));
    LOGOS_ASSERT_EQ(a["interface_sha256"], b["interface_sha256"]);
    LOGOS_ASSERT_EQ(a["contract_sha256"], b["contract_sha256"]);
    LOGOS_ASSERT_EQ(a["interface"].dump(), b["interface"].dump());
    LOGOS_ASSERT_TRUE(a["exposure"] != b["exposure"]);
    LOGOS_ASSERT_EQ(b["exposure"]["methods"].dump(), std::string(R"(["name","version","lidl"])"));
}
