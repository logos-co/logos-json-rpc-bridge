// JSON-RPC 2.0 framing: request parsing, batch splitting, envelope
// construction, and the two params shapes.

#include <logos_test.h>

#include <string>
#include <vector>

#include "rpc_dispatcher.h"

using namespace bridge;

namespace {

nlohmann::json j(const std::string& s) {
    return nlohmann::json::parse(s, nullptr, false);
}

} // namespace

// ── request parsing ─────────────────────────────────────────────────────────

LOGOS_TEST(a_well_formed_request_parses) {
    RpcRequest r;
    MappedError e;
    LOGOS_ASSERT_TRUE(parseRequest(
        j(R"({"jsonrpc":"2.0","id":1,"method":"rpc.ping"})"), &r, &e));
    LOGOS_ASSERT_EQ(r.method, std::string("rpc.ping"));
    LOGOS_ASSERT_FALSE(r.isNotification);
    LOGOS_ASSERT_EQ(r.id.get<int>(), 1);
}

LOGOS_TEST(a_missing_or_wrong_jsonrpc_version_is_refused) {
    RpcRequest r; MappedError e;
    LOGOS_ASSERT_FALSE(parseRequest(j(R"({"id":1,"method":"rpc.ping"})"), &r, &e));
    LOGOS_ASSERT_FALSE(parseRequest(j(R"({"jsonrpc":"1.0","id":1,"method":"rpc.ping"})"), &r, &e));
}

LOGOS_TEST(a_non_object_request_is_refused) {
    RpcRequest r; MappedError e;
    LOGOS_ASSERT_FALSE(parseRequest(j("[]"), &r, &e));
    LOGOS_ASSERT_FALSE(parseRequest(j("7"), &r, &e));
}

// A notification is the id KEY being absent. `id: null` is a valid id that gets
// a response echoing null. Conflating them leaves a WebSocket client's request
// unanswered forever, with no error and no timeout to end the wait.
LOGOS_TEST(notification_is_an_absent_id_not_a_null_one) {
    RpcRequest absent; MappedError e;
    LOGOS_ASSERT_TRUE(parseRequest(j(R"({"jsonrpc":"2.0","method":"rpc.ping"})"), &absent, &e));
    LOGOS_ASSERT_TRUE(absent.isNotification);

    RpcRequest explicitNull;
    LOGOS_ASSERT_TRUE(parseRequest(
        j(R"({"jsonrpc":"2.0","id":null,"method":"rpc.ping"})"), &explicitNull, &e));
    LOGOS_ASSERT_FALSE(explicitNull.isNotification);
    LOGOS_ASSERT_TRUE(explicitNull.id.is_null());
}

LOGOS_TEST(a_structured_id_is_refused) {
    RpcRequest r; MappedError e;
    LOGOS_ASSERT_FALSE(parseRequest(
        j(R"({"jsonrpc":"2.0","id":{"a":1},"method":"rpc.ping"})"), &r, &e));
}

LOGOS_TEST(absent_params_becomes_an_empty_object) {
    RpcRequest r; MappedError e;
    LOGOS_ASSERT_TRUE(parseRequest(j(R"({"jsonrpc":"2.0","id":1,"method":"rpc.ping"})"), &r, &e));
    LOGOS_ASSERT_TRUE(r.params.is_object());
}

// ── rpc.call target ─────────────────────────────────────────────────────────

// rpc.call names module and method as separate fields, mirroring the transport
// spec's Request. The dotted "<module>.<method>" form is the alias entry point.
LOGOS_TEST(a_call_target_needs_both_fields_as_strings) {
    CallTarget t; MappedError e;
    LOGOS_ASSERT_TRUE(parseCallTarget(j(R"({"module":"m","method":"get"})"), &t, &e));
    LOGOS_ASSERT_EQ(t.module, std::string("m"));
    LOGOS_ASSERT_EQ(t.method, std::string("get"));

    LOGOS_ASSERT_FALSE(parseCallTarget(j(R"({"module":"m"})"), &t, &e));
    LOGOS_ASSERT_FALSE(parseCallTarget(j(R"({"method":"get"})"), &t, &e));
    LOGOS_ASSERT_FALSE(parseCallTarget(j(R"({"module":1,"method":"get"})"), &t, &e));
    LOGOS_ASSERT_FALSE(parseCallTarget(j(R"({"module":"","method":"get"})"), &t, &e));
    LOGOS_ASSERT_FALSE(parseCallTarget(j("[]"), &t, &e));
}

// Inside rpc.call params a dotted name is taken literally: only the alias entry
// point splits names, and without a "module" field the call is refused.
LOGOS_TEST(a_dotted_name_inside_rpc_call_params_is_literal) {
    CallTarget t; MappedError e;
    LOGOS_ASSERT_FALSE(parseCallTarget(j(R"({"method":"storage_module.get"})"), &t, &e));
    LOGOS_ASSERT_TRUE(parseCallTarget(j(R"({"module":"m","method":"storage_module.get"})"), &t, &e));
    LOGOS_ASSERT_EQ(t.module, std::string("m"));
    LOGOS_ASSERT_EQ(t.method, std::string("storage_module.get"));
}

LOGOS_TEST(both_params_shapes_are_accepted_and_absent_becomes_an_array) {
    CallTarget t; MappedError e;
    LOGOS_ASSERT_TRUE(parseCallTarget(
        j(R"({"module":"m","method":"get","params":{"key":"k"}})"), &t, &e));
    LOGOS_ASSERT_TRUE(t.params.is_object());

    LOGOS_ASSERT_TRUE(parseCallTarget(
        j(R"({"module":"m","method":"get","params":["k"]})"), &t, &e));
    LOGOS_ASSERT_TRUE(t.params.is_array());

    LOGOS_ASSERT_TRUE(parseCallTarget(j(R"({"module":"m","method":"get"})"), &t, &e));
    LOGOS_ASSERT_TRUE(t.params.is_array());

    LOGOS_ASSERT_TRUE(parseCallTarget(
        j(R"({"module":"m","method":"get","params":null})"), &t, &e));
    LOGOS_ASSERT_TRUE(t.params.is_array());

    LOGOS_ASSERT_FALSE(parseCallTarget(
        j(R"({"module":"m","method":"get","params":"scalar"})"), &t, &e));
}

// ── subscribe target ────────────────────────────────────────────────────────

// The subscription id is the CALLER's, per the transport spec, so a client can
// correlate without waiting for the ack.
LOGOS_TEST(subscribe_requires_a_caller_assigned_id_plus_module_and_event) {
    SubscribeTarget t; MappedError e;
    LOGOS_ASSERT_TRUE(parseSubscribeTarget(
        j(R"({"subscription":"s1","module":"m","event":"m.ready_event"})"), &t, &e, true));
    LOGOS_ASSERT_EQ(t.event, std::string("m.ready_event"));

    LOGOS_ASSERT_FALSE(parseSubscribeTarget(
        j(R"({"module":"m","event":"e"})"), &t, &e, true));
    LOGOS_ASSERT_FALSE(parseSubscribeTarget(
        j(R"({"subscription":"s1","module":"m"})"), &t, &e, true));
}

// Unsubscribe carries only the id: nothing else is needed to name what to stop.
LOGOS_TEST(unsubscribe_needs_only_the_subscription_id) {
    SubscribeTarget t; MappedError e;
    LOGOS_ASSERT_TRUE(parseSubscribeTarget(j(R"({"subscription":"s1"})"), &t, &e, false));
    LOGOS_ASSERT_FALSE(parseSubscribeTarget(j(R"({})"), &t, &e, false));
}

LOGOS_TEST(a_numeric_subscription_id_is_accepted) {
    SubscribeTarget t; MappedError e;
    LOGOS_ASSERT_TRUE(parseSubscribeTarget(j(R"({"subscription":42})"), &t, &e, false));
}

// ── batches ─────────────────────────────────────────────────────────────────

LOGOS_TEST(a_single_object_body_splits_to_one_and_reports_single) {
    std::vector<nlohmann::json> out; bool single = false; nlohmann::json err;
    LOGOS_ASSERT_TRUE(splitBody(R"({"jsonrpc":"2.0","id":1,"method":"rpc.ping"})",
                                &out, &single, &err));
    LOGOS_ASSERT_TRUE(single);
    LOGOS_ASSERT_EQ(out.size(), static_cast<size_t>(1));
}

LOGOS_TEST(an_array_body_splits_to_many_and_reports_batch) {
    std::vector<nlohmann::json> out; bool single = true; nlohmann::json err;
    LOGOS_ASSERT_TRUE(splitBody(
        R"([{"jsonrpc":"2.0","id":1,"method":"rpc.ping"},)"
        R"({"jsonrpc":"2.0","id":2,"method":"rpc.ping"}])", &out, &single, &err));
    LOGOS_ASSERT_FALSE(single);
    LOGOS_ASSERT_EQ(out.size(), static_cast<size_t>(2));
}

LOGOS_TEST(unparseable_and_empty_batches_produce_protocol_errors) {
    std::vector<nlohmann::json> out; bool single = true; nlohmann::json err;
    LOGOS_ASSERT_FALSE(splitBody("{oops", &out, &single, &err));
    LOGOS_ASSERT_EQ(err["error"]["code"].get<int>(), static_cast<int>(kParseError));

    out.clear();
    LOGOS_ASSERT_FALSE(splitBody("[]", &out, &single, &err));
    LOGOS_ASSERT_EQ(err["error"]["code"].get<int>(), static_cast<int>(kInvalidRequest));
}

// ── envelopes ───────────────────────────────────────────────────────────────

LOGOS_TEST(envelopes_carry_jsonrpc_and_echo_the_id_verbatim) {
    const auto ok = makeResult(nlohmann::json("abc"), nlohmann::json{{"v", 1}});
    LOGOS_ASSERT_EQ(ok["jsonrpc"].get<std::string>(), std::string("2.0"));
    LOGOS_ASSERT_EQ(ok["id"].get<std::string>(), std::string("abc"));
    LOGOS_ASSERT_TRUE(ok.contains("result"));
    LOGOS_ASSERT_FALSE(ok.contains("error"));

    const auto bad = makeError(nlohmann::json(7), notFound());
    LOGOS_ASSERT_EQ(bad["id"].get<int>(), 7);
    LOGOS_ASSERT_TRUE(bad.contains("error"));
    LOGOS_ASSERT_FALSE(bad.contains("result"));
}

// A server-initiated event carries no id: it is a notification, and a client
// must never try to correlate it with a request.
LOGOS_TEST(server_notifications_carry_no_id) {
    const auto n = makeNotification(op::kEvent, nlohmann::json{{"subscription", "s1"}});
    LOGOS_ASSERT_FALSE(n.contains("id"));
    LOGOS_ASSERT_EQ(n["method"].get<std::string>(), std::string("rpc.event"));
}

LOGOS_TEST(the_operation_namespace_is_rpc_not_logos) {
    LOGOS_ASSERT_EQ(std::string(op::kCall), std::string("rpc.call"));
    LOGOS_ASSERT_EQ(std::string(op::kSubscribe), std::string("rpc.subscribe"));
    LOGOS_ASSERT_EQ(std::string(op::kTerminated), std::string("rpc.subscription_terminated"));
}

// ── aliases: "<module>.<method>" is exactly rpc.call ────────────────────────

namespace {

bool alias(const std::string& body, CallTarget* t, MappedError* e) {
    RpcRequest r;
    if (!parseRequest(j(body), &r, e)) return false;
    return parseAliasCall(r, t, e);
}

} // namespace

LOGOS_TEST(rpc_prefixed_names_are_always_bridge_operations) {
    LOGOS_ASSERT_TRUE(isBridgeOp("rpc.call"));
    LOGOS_ASSERT_TRUE(isBridgeOp("rpc.no_such_op"));
    LOGOS_ASSERT_TRUE(isBridgeOp("rpc."));
    LOGOS_ASSERT_FALSE(isBridgeOp("rpc"));
    LOGOS_ASSERT_FALSE(isBridgeOp("rpcx.get"));
    LOGOS_ASSERT_FALSE(isBridgeOp("storage_module.get"));
}

LOGOS_TEST(an_alias_splits_at_the_first_dot) {
    std::string module, method;
    LOGOS_ASSERT_TRUE(splitAlias("storage_module.get", &module, &method));
    LOGOS_ASSERT_EQ(module, std::string("storage_module"));
    LOGOS_ASSERT_EQ(method, std::string("get"));
    LOGOS_ASSERT_TRUE(splitAlias("m.a.b", &module, &method));
    LOGOS_ASSERT_EQ(module, std::string("m"));
    LOGOS_ASSERT_EQ(method, std::string("a.b"));
    LOGOS_ASSERT_TRUE(splitAlias("m.x", &module, &method));
}

LOGOS_TEST(a_name_without_an_inner_dot_is_not_an_alias) {
    std::string module, method;
    for (const char* name : {"get", ".get", "m.", ".", "", "..", ".m.x"})
        LOGOS_ASSERT_FALSE(splitAlias(name, &module, &method));
}

// Not an alias and not an op: the same -32601 as every other unknown name.
LOGOS_TEST(a_non_alias_is_refused_with_the_shared_not_found_bytes) {
    CallTarget t; MappedError e;
    LOGOS_ASSERT_FALSE(alias(R"({"jsonrpc":"2.0","id":1,"method":"get"})", &t, &e));
    LOGOS_ASSERT_EQ(e.toJson().dump(), notFound().toJson().dump());
    LOGOS_ASSERT_FALSE(alias(R"({"jsonrpc":"2.0","id":1,"method":"m."})", &t, &e));
    LOGOS_ASSERT_EQ(e.toJson().dump(), notFound().toJson().dump());
}

LOGOS_TEST(alias_params_absent_become_an_empty_array) {
    CallTarget t; MappedError e;
    LOGOS_ASSERT_TRUE(alias(R"({"jsonrpc":"2.0","id":1,"method":"m.version"})", &t, &e));
    LOGOS_ASSERT_EQ(t.module, std::string("m"));
    LOGOS_ASSERT_EQ(t.method, std::string("version"));
    LOGOS_ASSERT_EQ(t.params.dump(), std::string("[]"));
}

LOGOS_TEST(alias_params_objects_and_arrays_pass_through) {
    CallTarget t; MappedError e;
    LOGOS_ASSERT_TRUE(alias(R"({"jsonrpc":"2.0","id":1,"method":"m.get","params":{"key":"k"}})",
                            &t, &e));
    LOGOS_ASSERT_EQ(t.params.dump(), std::string(R"({"key":"k"})"));
    LOGOS_ASSERT_TRUE(alias(R"({"jsonrpc":"2.0","id":1,"method":"m.get","params":["k",{}]})",
                            &t, &e));
    LOGOS_ASSERT_EQ(t.params.dump(), std::string(R"(["k",{}])"));
    LOGOS_ASSERT_TRUE(alias(R"({"jsonrpc":"2.0","id":1,"method":"m.get","params":{}})", &t, &e));
    LOGOS_ASSERT_TRUE(t.params.is_object());
}

LOGOS_TEST(alias_null_or_scalar_params_are_invalid_params) {
    CallTarget t; MappedError e;
    for (const char* p : {"null", "5", "\"k\"", "true"}) {
        const std::string body =
            std::string(R"({"jsonrpc":"2.0","id":1,"method":"m.get","params":)") + p + "}";
        LOGOS_ASSERT_FALSE(alias(body, &t, &e));
        LOGOS_ASSERT_EQ(e.jsonRpcCode, static_cast<int>(kInvalidParams));
    }
}

// Refused before any lookup, so an unexposed module and an exposed one answer alike.
LOGOS_TEST(an_alias_notification_is_an_invalid_request) {
    CallTarget t; MappedError e;
    LOGOS_ASSERT_FALSE(alias(R"({"jsonrpc":"2.0","method":"m.get"})", &t, &e));
    LOGOS_ASSERT_EQ(e.jsonRpcCode, static_cast<int>(kInvalidRequest));
    LOGOS_ASSERT_EQ(e.message, std::string(kCallNotificationRefused));
    MappedError other;
    LOGOS_ASSERT_FALSE(alias(R"({"jsonrpc":"2.0","method":"not_exposed.get"})", &t, &other));
    LOGOS_ASSERT_EQ(other.toJson().dump(), e.toJson().dump());
}

// `id: null` is a request, not a notification, for aliases too.
LOGOS_TEST(an_alias_with_a_null_id_is_a_request) {
    CallTarget t; MappedError e;
    LOGOS_ASSERT_TRUE(alias(R"({"jsonrpc":"2.0","id":null,"method":"m.get"})", &t, &e));
}
