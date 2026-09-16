// Config parsing, validation, and allow/deny resolution.
//
// This is the module's whole policy surface, and it is the part where being
// wrong is silent: a config that parses but resolves too permissively exposes
// something the operator did not intend, and nothing at runtime says so. So the
// refusals are pinned as hard as the acceptances.

#include <logos_test.h>

#include <string>

#include "bridge_config.h"

using namespace bridge;

namespace {

ConfigParseResult parse(const std::string& json) {
    return parseBridgeConfig(json, "json_rpc_bridge");
}

const char* kMinimal = R"({"expose":{"modules":["m1"]}})";

} // namespace

// ── defaults ────────────────────────────────────────────────────────────────

LOGOS_TEST(minimal_config_defaults_to_loopback_and_no_auth) {
    auto r = parse(kMinimal);
    LOGOS_ASSERT_TRUE(r.ok);
    LOGOS_ASSERT_EQ(r.config.host, std::string("127.0.0.1"));
    LOGOS_ASSERT_EQ(r.config.port, 8645);
    LOGOS_ASSERT_TRUE(r.config.authMode == AuthMode::None);
    LOGOS_ASSERT_EQ(r.config.modules.size(), static_cast<size_t>(1));
}

// Empty allowed_origins is the SAFE default: it refuses any request that
// carries an Origin at all. A browser will happily connect to 127.0.0.1 from
// any page, so "no origins configured" must not mean "any origin".
LOGOS_TEST(allowed_origins_defaults_to_empty) {
    auto r = parse(kMinimal);
    LOGOS_ASSERT_TRUE(r.ok);
    LOGOS_ASSERT_TRUE(r.config.allowedOrigins.empty());
}

// ── bind refusals ───────────────────────────────────────────────────────────

LOGOS_TEST(non_loopback_host_is_refused) {
    auto r = parse(R"({"http":{"host":"0.0.0.0"},"expose":{"modules":["m1"]}})");
    LOGOS_ASSERT_FALSE(r.ok);
    LOGOS_ASSERT_CONTAINS(r.error, "loopback");
}

LOGOS_TEST(routable_host_is_refused) {
    auto r = parse(R"({"http":{"host":"192.168.1.10"},"expose":{"modules":["m1"]}})");
    LOGOS_ASSERT_FALSE(r.ok);
    LOGOS_ASSERT_CONTAINS(r.error, "loopback");
}

LOGOS_TEST(loopback_spellings_are_accepted) {
    for (const char* h : {"127.0.0.1", "127.0.0.53", "localhost", "::1"}) {
        const std::string j =
            std::string(R"({"http":{"host":")") + h + R"("},"expose":{"modules":["m1"]}})";
        auto r = parse(j);
        LOGOS_ASSERT_TRUE(r.ok);
    }
}

// A hostname that merely resolves to loopback is NOT accepted: resolving here
// would let a hosts file decide, which is the rebinding class the check exists
// to close.
LOGOS_TEST(loopback_lookalike_hostname_is_refused) {
    auto r = parse(R"({"http":{"host":"localhost.evil.example"},"expose":{"modules":["m1"]}})");
    LOGOS_ASSERT_FALSE(r.ok);
}

LOGOS_TEST(port_out_of_range_is_refused) {
    LOGOS_ASSERT_FALSE(parse(R"({"http":{"port":0},"expose":{"modules":["m1"]}})").ok);
    LOGOS_ASSERT_FALSE(parse(R"({"http":{"port":70000},"expose":{"modules":["m1"]}})").ok);
}

// ── exposure ────────────────────────────────────────────────────────────────

LOGOS_TEST(expose_is_required) {
    LOGOS_ASSERT_FALSE(parse(R"({})").ok);
    LOGOS_ASSERT_FALSE(parse(R"({"expose":{}})").ok);
}

// There is no wildcard, by design. An empty list would otherwise be the
// easiest way to write "everything" by accident.
LOGOS_TEST(empty_module_list_is_refused) {
    auto r = parse(R"({"expose":{"modules":[]}})");
    LOGOS_ASSERT_FALSE(r.ok);
    LOGOS_ASSERT_CONTAINS(r.error, "no wildcard");
}

LOGOS_TEST(exposing_self_is_refused) {
    auto r = parse(R"({"expose":{"modules":["json_rpc_bridge"]}})");
    LOGOS_ASSERT_FALSE(r.ok);
    LOGOS_ASSERT_CONTAINS(r.error, "recurses");
}

// rpc.* is the bridge's namespace; a module named rpc could never be aliased.
LOGOS_TEST(a_module_named_rpc_is_refused) {
    auto bare = parse(R"({"expose":{"modules":["rpc"]}})");
    LOGOS_ASSERT_FALSE(bare.ok);
    LOGOS_ASSERT_CONTAINS(bare.error, "rpc");
    LOGOS_ASSERT_FALSE(parse(R"({"expose":{"modules":[{"name":"rpc"}]}})").ok);
    LOGOS_ASSERT_TRUE(parse(R"({"expose":{"modules":["rpc_module"]}})").ok);
}

// A '.' would make "<module>.<method>" ambiguous; whitespace and control
// characters have no business in a name that is also a path segment.
LOGOS_TEST(module_names_with_dots_whitespace_or_control_characters_are_refused) {
    for (const char* name : {"a.b", ".a", "a.", "a b", " a", "a\\tb", "a\\nb",
                             "a\\u0001", "a\\u007f"}) {
        const std::string j = std::string(R"({"expose":{"modules":[")") + name + R"("]}})";
        auto r = parse(j);
        LOGOS_ASSERT_FALSE(r.ok);
    }
    auto dotted = parse(R"({"expose":{"modules":["storage.module"]}})");
    LOGOS_ASSERT_CONTAINS(dotted.error, "'.'");
}

LOGOS_TEST(non_ascii_module_names_are_accepted) {
    LOGOS_ASSERT_TRUE(parse(R"({"expose":{"modules":["m\u00f3dulo","m_1-x"]}})").ok);
}

// Policy restricts calls, not knowledge: the built-ins cannot be denied.
LOGOS_TEST(denying_a_built_in_method_is_refused) {
    for (const char* b : {"lidl", "name", "version"}) {
        const std::string j = std::string(R"({"expose":{"modules":[{"name":"m1",)") +
                              R"("methods":{"deny":[")" + b + R"("]}}]}})";
        auto r = parse(j);
        LOGOS_ASSERT_FALSE(r.ok);
        LOGOS_ASSERT_CONTAINS(r.error, "built-ins");
        LOGOS_ASSERT_CONTAINS(r.error, b);
    }
}

// An allow list need not name them: they are implicitly allowed.
LOGOS_TEST(an_allow_list_leaves_the_built_ins_callable) {
    auto r = parse(R"({"expose":{"modules":[{"name":"m1","methods":{"allow":["get"]}}]}})");
    LOGOS_ASSERT_TRUE(r.ok);
    const ExposedModule* m = r.config.find("m1");
    for (const char* b : {"lidl", "name", "version"}) {
        LOGOS_ASSERT_FALSE(m->methods.permits(b));
        LOGOS_ASSERT_TRUE(m->methodAllowed(b));
    }
    LOGOS_ASSERT_TRUE(m->methodAllowed("get"));
    LOGOS_ASSERT_FALSE(m->methodAllowed("other"));
}

// The rule is about methods; an event that happens to be called "version" is not special.
LOGOS_TEST(an_event_policy_may_name_a_built_in) {
    LOGOS_ASSERT_TRUE(
        parse(R"({"expose":{"modules":[{"name":"m1","events":{"deny":["version"]}}]}})").ok);
}

LOGOS_TEST(duplicate_module_is_refused) {
    auto r = parse(R"({"expose":{"modules":["m1","m1"]}})");
    LOGOS_ASSERT_FALSE(r.ok);
    LOGOS_ASSERT_CONTAINS(r.error, "twice");
}

// ── allow / deny resolution ─────────────────────────────────────────────────

LOGOS_TEST(bare_string_module_permits_everything) {
    auto r = parse(kMinimal);
    LOGOS_ASSERT_TRUE(r.ok);
    const ExposedModule* m = r.config.find("m1");
    LOGOS_ASSERT_TRUE(m != nullptr);
    LOGOS_ASSERT_TRUE(m->methods.permits("anything"));
    LOGOS_ASSERT_TRUE(m->events.permits("any.event"));
}

// ABSENT allow means unconstrained; EMPTY allow permits nothing. Collapsing
// those is how `"allow": []` written as a placeholder ends up exposing
// everything.
LOGOS_TEST(absent_allow_is_unconstrained_but_empty_allow_permits_nothing) {
    auto absent = parse(R"({"expose":{"modules":[{"name":"m1","methods":{"deny":["x"]}}]}})");
    LOGOS_ASSERT_TRUE(absent.ok);
    LOGOS_ASSERT_TRUE(absent.config.find("m1")->methods.permits("anything"));

    auto empty = parse(R"({"expose":{"modules":[{"name":"m1","methods":{"allow":[]}}]}})");
    LOGOS_ASSERT_TRUE(empty.ok);
    LOGOS_ASSERT_FALSE(empty.config.find("m1")->methods.permits("anything"));
}

LOGOS_TEST(deny_subtracts_from_an_otherwise_open_module) {
    auto r = parse(R"({"expose":{"modules":[{"name":"m1","methods":{"deny":["wipe"]}}]}})");
    LOGOS_ASSERT_TRUE(r.ok);
    LOGOS_ASSERT_FALSE(r.config.find("m1")->methods.permits("wipe"));
    LOGOS_ASSERT_TRUE(r.config.find("m1")->methods.permits("get"));
}

// Both lists naming the same thing must deny. The opposite precedence is just
// as defensible and silently more permissive, so it is pinned.
LOGOS_TEST(deny_beats_allow) {
    auto r = parse(
        R"({"expose":{"modules":[{"name":"m1","methods":{"allow":["a","b"],"deny":["b"]}}]}})");
    LOGOS_ASSERT_TRUE(r.ok);
    LOGOS_ASSERT_TRUE(r.config.find("m1")->methods.permits("a"));
    LOGOS_ASSERT_FALSE(r.config.find("m1")->methods.permits("b"));
    LOGOS_ASSERT_FALSE(r.config.find("m1")->methods.permits("c"));
}

LOGOS_TEST(methods_and_events_are_independent_axes) {
    auto r = parse(
        R"({"expose":{"modules":[{"name":"m1","methods":{"allow":["get"]},)"
        R"("events":{"allow":["storage.ready_event"]}}]}})");
    LOGOS_ASSERT_TRUE(r.ok);
    const ExposedModule* m = r.config.find("m1");
    LOGOS_ASSERT_TRUE(m->methods.permits("get"));
    LOGOS_ASSERT_FALSE(m->methods.permits("storage.ready_event"));
    LOGOS_ASSERT_TRUE(m->events.permits("storage.ready_event"));
    LOGOS_ASSERT_FALSE(m->events.permits("get"));
}

LOGOS_TEST(duplicate_entry_in_a_policy_list_is_refused) {
    LOGOS_ASSERT_FALSE(
        parse(R"({"expose":{"modules":[{"name":"m1","methods":{"allow":["a","a"]}}]}})").ok);
    LOGOS_ASSERT_FALSE(
        parse(R"({"expose":{"modules":[{"name":"m1","events":{"deny":["e","e"]}}]}})").ok);
}

// ── auth ────────────────────────────────────────────────────────────────────

LOGOS_TEST(bearer_mode_parses) {
    auto r = parse(R"({"auth":{"mode":"bearer"},"expose":{"modules":["m1"]}})");
    LOGOS_ASSERT_TRUE(r.ok);
    LOGOS_ASSERT_TRUE(r.config.authMode == AuthMode::Bearer);
}

LOGOS_TEST(unknown_auth_mode_is_refused) {
    LOGOS_ASSERT_FALSE(parse(R"({"auth":{"mode":"basic"},"expose":{"modules":["m1"]}})").ok);
}

// A secret in config gets logged, echoed by getInfo(), and pasted into issues.
// Refusing the key outright is the only way to keep it from being written.
LOGOS_TEST(a_secret_in_config_is_refused_rather_than_ignored) {
    auto tok = parse(R"({"auth":{"mode":"bearer","token":"hunter2"},"expose":{"modules":["m1"]}})");
    LOGOS_ASSERT_FALSE(tok.ok);
    LOGOS_ASSERT_CONTAINS(tok.error, "persistence");

    auto file = parse(R"({"auth":{"mode":"bearer","token_file":"/x"},"expose":{"modules":["m1"]}})");
    LOGOS_ASSERT_FALSE(file.ok);
}

// ── limits ──────────────────────────────────────────────────────────────────

LOGOS_TEST(limits_override_defaults_and_reject_non_positive) {
    auto r = parse(R"({"expose":{"modules":["m1"]},"limits":{"max_connections":7}})");
    LOGOS_ASSERT_TRUE(r.ok);
    LOGOS_ASSERT_EQ(r.config.limits.maxConnections, 7);
    LOGOS_ASSERT_EQ(r.config.limits.maxInFlightTotal, 256);   // untouched default

    LOGOS_ASSERT_FALSE(parse(R"({"expose":{"modules":["m1"]},"limits":{"max_body_bytes":0}})").ok);
    LOGOS_ASSERT_FALSE(parse(R"({"expose":{"modules":["m1"]},"limits":{"call_timeout_ms":-1}})").ok);
}

// ── malformed input ─────────────────────────────────────────────────────────

LOGOS_TEST(malformed_json_is_refused_without_throwing) {
    LOGOS_ASSERT_FALSE(parse("not json").ok);
    LOGOS_ASSERT_FALSE(parse("[]").ok);
    LOGOS_ASSERT_FALSE(parse("").ok);
}

LOGOS_TEST(wrong_types_are_refused) {
    LOGOS_ASSERT_FALSE(parse(R"({"expose":{"modules":"m1"}})").ok);
    LOGOS_ASSERT_FALSE(parse(R"({"expose":{"modules":[123]}})").ok);
    LOGOS_ASSERT_FALSE(parse(R"({"expose":{"modules":[{"name":1}]}})").ok);
    LOGOS_ASSERT_FALSE(parse(R"({"http":{"host":1},"expose":{"modules":["m1"]}})").ok);
}
