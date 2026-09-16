// The OpenRPC, OpenAPI and AsyncAPI builders: names and ordering, x-logos-*
// keys, request/response shapes, exposure, statuses, text and determinism.

#include <logos_test.h>

#include <algorithm>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "doc_test_util.h"

using namespace bridge::docs;
using namespace bridge_test;

namespace {

json j(const char* text) { return json::parse(text); }

const json* findByName(const json& list, const std::string& name) {
    for (const auto& item : list)
        if (item.value("name", "") == name) return &item;
    return nullptr;
}

std::vector<std::string> names(const json& list) {
    std::vector<std::string> out;
    for (const auto& item : list) out.push_back(item.value("name", ""));
    return out;
}

// Keys used as schema keywords anywhere in the document (property names excluded).
std::set<std::string> keywords(const json& doc) {
    std::set<std::string> out;
    std::function<void(const json&, bool)> visit = [&](const json& n, bool namesOnly) {
        if (n.is_object()) {
            for (auto it = n.begin(); it != n.end(); ++it) {
                if (!namesOnly) out.insert(it.key());
                const bool nameMap = !namesOnly && (it.key() == "properties" || it.key() == "patternProperties");
                visit(it.value(), nameMap);
            }
        } else if (n.is_array()) {
            for (const auto& v : n) visit(v, false);
        }
    };
    visit(doc, false);
    return out;
}

DocContext sample() { return sampleContext(typedDoc("sample", sampleAst())); }

// Every catalog entry as an OpenRPC reference, in catalog order.
json allErrorRefs() {
    json out = json::array();
    for (const bridge::CatalogError& e : bridge::errorCatalog())
        out.push_back(json{{"$ref", std::string("#/components/errors/") + e.key}});
    return out;
}

// An untyped module, an invalid one and a pending one, as discovery reports them.
DocContext namesOnlyContext() {
    DocContext ctx;
    ctx.bridgeVersion = "0.1.0";
    ModuleDoc legacy;
    legacy.name = "legacy";
    legacy.status = kStatusUntyped;
    legacy.liveMethods = {{"add", {"a", "b"}}, {"odd", {"", "x"}}};
    legacy.exposedMethods = {"add", "odd", "unlisted"};
    legacy.exposedEvents = {"tick"};
    ctx.modules.push_back(legacy);
    ModuleDoc broken;
    broken.name = "broken";
    broken.status = kStatusInvalid;
    broken.interfaceError = "3:1: expected 'module'";
    broken.interfaceSha256 = "ignored";
    broken.exposedMethods = {"lidl"};
    ctx.modules.push_back(broken);
    ModuleDoc pending;
    pending.name = "later";
    pending.status = kStatusPending;
    ctx.modules.push_back(pending);
    return ctx;
}

// A typed module whose policy denies the only method and event using its record.
DocContext vaultContext() {
    json types = json::array({record("Secret", json::array({field("pin", prim("tstr"))}))});
    json methods = json::array({
        method("open", json::array(), prim("bool")),
        method("reveal", json::array({param("s", named("Secret"))}), named("Secret")),
    });
    json events = json::array({event("leaked", json::array({param("s", named("Secret"))})),
                               event("opened", json::array())});
    return sampleContext(typedDoc("vault", moduleAst("vault", types, methods, events), {"reveal"}, {"leaked"}));
}

} // namespace

// ── OpenRPC ─────────────────────────────────────────────────────────────────

LOGOS_TEST(openrpc_lists_bridge_ops_first_then_modules_in_config_order) {
    DocContext ctx = sample();
    ctx.modules.insert(ctx.modules.begin(), typedDoc("first", moduleAst("first", json::array(),
        json::array({method("hello", json::array(), prim("tstr"))}), json::array())));
    const json doc = buildOpenRpc(ctx);
    LOGOS_ASSERT_EQ(doc["openrpc"], json("1.3.2"));
    const std::vector<std::string> expected = {
        "rpc.call", "rpc.subscribe", "rpc.unsubscribe", "rpc.schema", "rpc.list_modules",
        "rpc.ping", "rpc.cancel", "rpc.discover",
        "first.hello", "first.name", "first.version", "first.lidl",
        "sample.move", "sample.reset", "sample.blob", "sample.name", "sample.version", "sample.lidl",
    };
    LOGOS_ASSERT_TRUE(names(doc["methods"]) == expected);
    for (std::size_t i = 0; i < 8; ++i) {
        LOGOS_ASSERT_EQ(doc["methods"][i]["x-logos-bridge-op"], json(true));
        LOGOS_ASSERT_FALSE(doc["methods"][i].contains("x-logos-module"));
    }
}

LOGOS_TEST(openrpc_module_methods_carry_x_logos_keys_params_and_errors) {
    const json doc = buildOpenRpc(sample());
    const json& move = *findByName(doc["methods"], "sample.move");
    LOGOS_ASSERT_EQ(move["x-logos-module"], json("sample"));
    LOGOS_ASSERT_EQ(move["x-logos-method"], json("move"));
    LOGOS_ASSERT_FALSE(move.contains("x-logos-derived"));
    LOGOS_ASSERT_FALSE(move.contains("x-logos-untyped"));
    LOGOS_ASSERT_FALSE(move.contains("x-logos-bridge-op"));
    LOGOS_ASSERT_EQ(move["paramStructure"], json("either"));
    LOGOS_ASSERT_EQ(move["tags"], j(R"([{"name":"sample"}])"));
    LOGOS_ASSERT_EQ(move["params"], j(R"([
        {"name":"to","required":true,"schema":{"$ref":"#/components/schemas/sample.Point"}},
        {"name":"speed","required":false,"schema":{"anyOf":[{"type":"number","format":"double"},{"type":"null"}]}}])"));
    LOGOS_ASSERT_EQ(move["result"], j(R"({"name":"result","schema":{"anyOf":[
        {"$ref":"#/components/schemas/sample.Point"},{"$ref":"#/components/schemas/ProviderRejection"}]}})"));
    LOGOS_ASSERT_EQ(move["errors"], allErrorRefs());
    LOGOS_ASSERT_EQ(move["summary"], json("Move to a point."));

    const json& reset = *findByName(doc["methods"], "sample.reset");
    LOGOS_ASSERT_EQ(reset["params"], json::array());
    LOGOS_ASSERT_EQ(reset["result"]["schema"]["anyOf"][0], j(R"({"const":true})"));
    LOGOS_ASSERT_FALSE(reset.contains("summary"));
}

LOGOS_TEST(openrpc_bridge_ops_have_the_dispatcher_shapes) {
    const json doc = buildOpenRpc(sample());
    const json& subscribe = *findByName(doc["methods"], "rpc.subscribe");
    LOGOS_ASSERT_EQ(subscribe["paramStructure"], json("by-name"));
    LOGOS_ASSERT_EQ(subscribe["result"]["schema"]["required"],
                    j(R"(["subscription","operation","module","event","state"])"));
    LOGOS_ASSERT_EQ(subscribe["result"]["schema"]["properties"]["state"]["enum"], j(R"(["registered","active"])"));
    LOGOS_ASSERT_CONTAINS(subscribe["description"].get<std::string>(), "WebSocket-only");
    const json& unsubscribe = *findByName(doc["methods"], "rpc.unsubscribe");
    LOGOS_ASSERT_EQ(unsubscribe["result"]["schema"]["properties"]["operation"], j(R"({"const":"unsubscribe"})"));
    LOGOS_ASSERT_EQ((*findByName(doc["methods"], "rpc.ping"))["result"]["schema"], j(R"({"const":"pong"})"));
    const json& cancel = *findByName(doc["methods"], "rpc.cancel");
    LOGOS_ASSERT_EQ(cancel["result"]["schema"]["properties"],
                    j(R"({"cancelled":{"const":false},"reason":{"const":"not_supported_upstream"}})"));
    // The document it returns is described in place, not by an external reference.
    const json& discover = *findByName(doc["methods"], "rpc.discover");
    LOGOS_ASSERT_EQ(discover["result"]["schema"]["type"], json("object"));
    LOGOS_ASSERT_FALSE(discover["result"]["schema"].contains("$ref"));
    const json& call = *findByName(doc["methods"], "rpc.call");
    LOGOS_ASSERT_TRUE(names(call["params"]) == (std::vector<std::string>{"module", "method", "params"}));
    LOGOS_ASSERT_EQ(call["errors"], allErrorRefs());
    // A ping can be refused only at the envelope, so it lists a subset, still in catalog order.
    LOGOS_ASSERT_EQ((*findByName(doc["methods"], "rpc.ping"))["errors"], j(R"([
        {"$ref":"#/components/errors/ParseError"},{"$ref":"#/components/errors/InvalidRequest"},
        {"$ref":"#/components/errors/ShuttingDown"},{"$ref":"#/components/errors/Overloaded"}])"));
    // Every error, on every method, is a reference into components.errors.
    const json& components = doc["components"]["errors"];
    for (const auto& m : doc["methods"])
        for (const auto& e : m["errors"]) {
            LOGOS_ASSERT_EQ(e.size(), static_cast<size_t>(1));
            const std::string ref = e["$ref"].get<std::string>();
            LOGOS_ASSERT_EQ(ref.rfind("#/components/errors/", 0), static_cast<size_t>(0));
            LOGOS_ASSERT_TRUE(components.contains(ref.substr(20)));
        }
}

// components.errors is the catalog itself, keyed by its stable names.
LOGOS_TEST(openrpc_error_components_are_the_catalog) {
    const json errors = buildOpenRpc(sample())["components"]["errors"];
    const std::vector<bridge::CatalogError> catalog = bridge::errorCatalog();
    LOGOS_ASSERT_EQ(errors.size(), catalog.size());
    for (const bridge::CatalogError& e : catalog) LOGOS_ASSERT_EQ(errors[e.key], e.error.toJson());
    LOGOS_ASSERT_EQ(errors["MethodNotFound"], bridge::notFound().toJson());
    LOGOS_ASSERT_EQ(errors["UpstreamTimeout"]["code"], json(-32002));
}

LOGOS_TEST(identity_methods_are_derived_and_lidl_says_it_returns_the_contract) {
    const json doc = buildOpenRpc(sample());
    for (const char* builtin : {"sample.name", "sample.version", "sample.lidl"}) {
        const json& m = *findByName(doc["methods"], builtin);
        LOGOS_ASSERT_EQ(m["x-logos-derived"], json(true));
        LOGOS_ASSERT_CONTAINS(m["description"].get<std::string>(), "always callable");
        LOGOS_ASSERT_EQ(m["result"]["schema"]["anyOf"][0], j(R"({"type":"string"})"));
    }
    const json& lidl = *findByName(doc["methods"], "sample.lidl");
    LOGOS_ASSERT_EQ(lidl["summary"], json("The module's canonical LIDL interface document."));
    LOGOS_ASSERT_CONTAINS(lidl["description"].get<std::string>(), "The module's canonical LIDL interface document.");
    LOGOS_ASSERT_CONTAINS(lidl["description"].get<std::string>(), "full canonical LIDL contract");
}

// The runtime always puts the built-ins in exposure; a policy cannot remove them.
LOGOS_TEST(lidl_is_listed_for_every_ok_module_even_under_a_deny_all_policy) {
    ModuleDoc m = typedDoc("sample", sampleAst(), {"move", "reset", "blob", "name", "version", "lidl"}, {"moved"});
    LOGOS_ASSERT_TRUE(m.exposedMethods == (std::vector<std::string>{"name", "version", "lidl"}));
    const DocContext ctx = sampleContext(m);
    LOGOS_ASSERT_TRUE(findByName(buildOpenRpc(ctx)["methods"], "sample.lidl") != nullptr);
    LOGOS_ASSERT_TRUE(buildOpenApi(ctx)["paths"].contains("/modules/sample/lidl"));
    LOGOS_ASSERT_TRUE(buildAsyncApi(ctx)["operations"].contains("sample.lidl"));
}

LOGOS_TEST(openrpc_events_carry_a_draft07_data_tuple) {
    const json doc = buildOpenRpc(sample());
    LOGOS_ASSERT_EQ(doc["x-logos-events"].size(), static_cast<size_t>(1));
    const json& e = doc["x-logos-events"][0];
    LOGOS_ASSERT_EQ(e["name"], json("sample.moved"));
    LOGOS_ASSERT_EQ(e["module"], json("sample"));
    LOGOS_ASSERT_EQ(e["event"], json("moved"));
    LOGOS_ASSERT_EQ(e["subscribe"], json("rpc.subscribe"));
    LOGOS_ASSERT_EQ(e["notification"], json("rpc.event"));
    LOGOS_ASSERT_EQ(e["params"], j(R"(["at","n"])"));
    LOGOS_ASSERT_EQ(e["summary"], json("Emitted after a move."));
    LOGOS_ASSERT_EQ(e["dataSchema"]["items"][0], j(R"({"$ref":"#/components/schemas/sample.Point"})"));
    LOGOS_ASSERT_EQ(e["dataSchema"]["additionalItems"], json(false));
    LOGOS_ASSERT_EQ(e["dataSchema"]["minItems"], json(2));
}

LOGOS_TEST(components_cover_shared_schemas_and_every_record) {
    const json rpc = buildOpenRpc(sample())["components"]["schemas"];
    for (const char* k : {"Bytes", "LogosResult", "ProviderRejection", "sample.Point", "sample.Unused"})
        LOGOS_ASSERT_TRUE(rpc.contains(k));
    LOGOS_ASSERT_EQ(rpc.size(), static_cast<size_t>(5));
    const json api = buildOpenApi(sample())["components"]["schemas"];
    for (const char* k : {"Bytes", "LogosResult", "ProviderRejection", "JsonRpcId", "JsonRpcError",
                          "JsonRpcErrorResponse", "ErrorBody", "sample.Point", "sample.Unused"})
        LOGOS_ASSERT_TRUE(api.contains(k));
    LOGOS_ASSERT_EQ(api.size(), static_cast<size_t>(9));
    const json async = buildAsyncApi(sample())["components"]["schemas"];
    for (const char* k : {"Bytes", "LogosResult", "ProviderRejection", "JsonRpcError", "JsonRpcId",
                          "SubscriptionId", "SubscriptionState", "NoParams", "EventGeneration",
                          "EventTimestamp", "sample.Point", "sample.Unused"})
        LOGOS_ASSERT_TRUE(async.contains(k));
    LOGOS_ASSERT_EQ(async.size(), static_cast<size_t>(12));
}

// ── OpenAPI ─────────────────────────────────────────────────────────────────

LOGOS_TEST(openapi_module_paths_have_the_documented_request_and_responses) {
    const json doc = buildOpenApi(sample());
    LOGOS_ASSERT_EQ(doc["openapi"], json("3.1.1"));
    LOGOS_ASSERT_EQ(doc["jsonSchemaDialect"], json("https://spec.openapis.org/oas/3.1/dialect/base"));
    LOGOS_ASSERT_EQ(doc["servers"][0]["url"], json("http://127.0.0.1:8645"));
    LOGOS_ASSERT_FALSE(doc.contains("security"));
    LOGOS_ASSERT_FALSE(doc["components"].contains("securitySchemes"));

    const json& op = doc["paths"]["/modules/sample/move"]["post"];
    LOGOS_ASSERT_EQ(op["operationId"], json("sample.move"));
    LOGOS_ASSERT_EQ(op["tags"], j(R"(["sample"])"));
    LOGOS_ASSERT_EQ(op["x-logos-module"], json("sample"));
    LOGOS_ASSERT_EQ(op["x-logos-method"], json("move"));
    LOGOS_ASSERT_EQ(op["requestBody"]["required"], json(true));
    const json& body = op["requestBody"]["content"]["application/json"]["schema"]["oneOf"];
    LOGOS_ASSERT_EQ(body[0]["type"], json("object"));
    LOGOS_ASSERT_EQ(body[0]["required"], j(R"(["to"])"));
    LOGOS_ASSERT_EQ(body[0]["additionalProperties"], json(false));
    LOGOS_ASSERT_EQ(body[1]["prefixItems"].size(), static_cast<size_t>(2));
    LOGOS_ASSERT_EQ(body[1]["items"], json(false));
    LOGOS_ASSERT_EQ(body[1]["minItems"], json(2));
    LOGOS_ASSERT_EQ(body[1]["maxItems"], json(2));

    const json& responses = op["responses"];
    LOGOS_ASSERT_EQ(responses.size(), static_cast<size_t>(5));   // 200, 401, 403, 411, 415
    const json& ok = responses["200"]["content"]["application/json"]["schema"]["oneOf"];
    LOGOS_ASSERT_EQ(ok[0], j(R"({"type":"object","required":["result"],"additionalProperties":false,
        "properties":{"result":{"anyOf":[{"$ref":"#/components/schemas/sample.Point"},
                                         {"$ref":"#/components/schemas/ProviderRejection"}]}}})"));
    LOGOS_ASSERT_EQ(ok[1], j(R"({"$ref":"#/components/schemas/ErrorBody"})"));
    LOGOS_ASSERT_EQ(ok[2], j(R"({"$ref":"#/components/schemas/JsonRpcErrorResponse"})"));
    LOGOS_ASSERT_EQ(doc["components"]["schemas"]["ErrorBody"]["properties"],
                    j(R"({"error":{"$ref":"#/components/schemas/JsonRpcError"}})"));
    LOGOS_ASSERT_EQ(doc["components"]["schemas"]["ErrorBody"]["required"], j(R"(["error"])"));
    LOGOS_ASSERT_EQ(doc["components"]["schemas"]["ErrorBody"]["additionalProperties"], json(false));
    LOGOS_ASSERT_EQ(doc["components"]["schemas"]["JsonRpcErrorResponse"]["properties"]["id"],
                    j(R"({"$ref":"#/components/schemas/JsonRpcId"})"));

    // lws writes these refusals as HTML pages; every path references the same components.
    LOGOS_ASSERT_EQ(responses["401"], j(R"({"$ref":"#/components/responses/Unauthorized"})"));
    LOGOS_ASSERT_EQ(responses["403"], j(R"({"$ref":"#/components/responses/Forbidden"})"));
    LOGOS_ASSERT_EQ(responses["411"], j(R"({"$ref":"#/components/responses/LengthRequired"})"));
    LOGOS_ASSERT_EQ(responses["415"], j(R"({"$ref":"#/components/responses/UnsupportedMediaType"})"));
    for (const char* name : {"Unauthorized", "Forbidden", "LengthRequired", "UnsupportedMediaType"}) {
        const json& r = doc["components"]["responses"][name];
        LOGOS_ASSERT_TRUE(r.contains("description"));
        LOGOS_ASSERT_EQ(r["content"], j(R"({"text/html":{"schema":{"type":"string"}}})"));
    }
    for (const auto& item : doc["paths"])
        for (auto verb = item.begin(); verb != item.end(); ++verb) {
            const json& rs = verb.value()["responses"];
            LOGOS_ASSERT_EQ(rs["401"], responses["401"]);
            LOGOS_ASSERT_EQ(rs["403"], responses["403"]);
            LOGOS_ASSERT_EQ(rs.contains("411"), verb.key() == "post");
            LOGOS_ASSERT_EQ(rs.contains("415"), verb.key() == "post");
        }
}

LOGOS_TEST(openapi_bodies_are_required_only_with_a_required_param) {
    DocContext ctx = sampleContext(typedDoc("opt", moduleAst("opt", json::array(), json::array({
        method("maybe", json::array({param("x", optionalOf(prim("tstr")))}), prim("bool")),
    }), json::array())));
    const json doc = buildOpenApi(ctx);
    const json& maybe = doc["paths"]["/modules/opt/maybe"]["post"]["requestBody"];
    LOGOS_ASSERT_EQ(maybe["required"], json(false));
    LOGOS_ASSERT_FALSE(maybe["content"]["application/json"]["schema"]["oneOf"][0].contains("required"));
    // Over the alias an absent params is [], which fits only zero arity.
    const json async = buildAsyncApi(ctx);
    LOGOS_ASSERT_EQ(async["components"]["messages"]["opt.maybe.request"]["payload"]["required"],
                    j(R"(["jsonrpc","id","method","params"])"));
    LOGOS_ASSERT_EQ(async["components"]["messages"]["opt.name.request"]["payload"]["required"],
                    j(R"(["jsonrpc","id","method"])"));

    // Every zero-arity method shares one request body.
    const json sampleDoc = buildOpenApi(sample());
    LOGOS_ASSERT_EQ(sampleDoc["paths"]["/modules/sample/reset"]["post"]["requestBody"],
                    j(R"({"$ref":"#/components/requestBodies/NoParams"})"));
    LOGOS_ASSERT_EQ(sampleDoc["paths"]["/modules/sample/lidl"]["post"]["requestBody"],
                    j(R"({"$ref":"#/components/requestBodies/NoParams"})"));
    const json& noParams = sampleDoc["components"]["requestBodies"]["NoParams"];
    LOGOS_ASSERT_EQ(noParams["required"], json(false));
    LOGOS_ASSERT_EQ(noParams["content"]["application/json"]["schema"]["oneOf"],
                    j(R"([{"type":"object","maxProperties":0},{"type":"array","maxItems":0}])"));
    LOGOS_ASSERT_EQ(async["components"]["messages"]["opt.name.request"]["payload"]["properties"]["params"],
                    j(R"({"$ref":"#/components/schemas/NoParams"})"));
}

LOGOS_TEST(openapi_has_the_fixed_routes) {
    const json doc = buildOpenApi(sample());
    const json& paths = doc["paths"];
    for (const char* p : {"/healthz", "/modules", "/modules/{module}", "/openapi.json", "/asyncapi.json"}) {
        LOGOS_ASSERT_TRUE(paths[p].contains("get"));
        LOGOS_ASSERT_EQ(paths[p]["get"]["x-logos-bridge-op"], json(true));
    }
    LOGOS_ASSERT_EQ(paths["/healthz"]["get"]["responses"]["200"]["content"]["application/json"]["schema"]["properties"]["status"]["enum"],
                    j(R"(["ok","draining"])"));
    const json& module = paths["/modules/{module}"]["get"];
    LOGOS_ASSERT_EQ(module["parameters"][0]["in"], json("path"));
    LOGOS_ASSERT_EQ(module["responses"]["404"]["content"]["application/json"]["schema"],
                    j(R"({"$ref":"#/components/schemas/JsonRpcError"})"));
    const json& rpc = paths["/rpc"]["post"];
    LOGOS_ASSERT_EQ(rpc["requestBody"]["required"], json(true));
    LOGOS_ASSERT_EQ(rpc["responses"]["411"], j(R"({"$ref":"#/components/responses/LengthRequired"})"));
    LOGOS_ASSERT_EQ(rpc["responses"]["415"], j(R"({"$ref":"#/components/responses/UnsupportedMediaType"})"));
    LOGOS_ASSERT_EQ(rpc["responses"]["200"]["content"]["application/json"]["schema"]["oneOf"][1]["type"], json("array"));
}

// ── AsyncAPI ────────────────────────────────────────────────────────────────

LOGOS_TEST(asyncapi_receives_requests_with_a_reply_and_sends_events) {
    const json doc = buildAsyncApi(sample());
    LOGOS_ASSERT_EQ(doc["asyncapi"], json("3.0.0"));
    LOGOS_ASSERT_EQ(doc["defaultContentType"], json("application/json"));
    LOGOS_ASSERT_EQ(doc["servers"]["bridge"]["host"], json("127.0.0.1:8645"));
    LOGOS_ASSERT_EQ(doc["servers"]["bridge"]["protocol"], json("ws"));
    LOGOS_ASSERT_EQ(doc["channels"]["ws"]["address"], json("/ws"));

    const json& ops = doc["operations"];
    const json& move = ops["sample.move"];
    LOGOS_ASSERT_EQ(move["action"], json("receive"));
    LOGOS_ASSERT_EQ(move["channel"], j(R"({"$ref":"#/channels/ws"})"));
    LOGOS_ASSERT_EQ(move["messages"], j(R"([{"$ref":"#/channels/ws/messages/sample.move.request"}])"));
    LOGOS_ASSERT_EQ(move["reply"], j(R"({"channel":{"$ref":"#/channels/ws"},"messages":[
        {"$ref":"#/channels/ws/messages/sample.move.result"},{"$ref":"#/channels/ws/messages/rpc.error"}]})"));

    LOGOS_ASSERT_EQ(ops["sample.moved.subscribe"]["action"], json("receive"));
    LOGOS_ASSERT_EQ(ops["sample.moved.subscribe"]["reply"]["messages"][0],
                    j(R"({"$ref":"#/channels/ws/messages/sample.moved.subscribed"})"));
    LOGOS_ASSERT_EQ(ops["sample.moved.event"]["action"], json("send"));
    LOGOS_ASSERT_FALSE(ops["sample.moved.event"].contains("reply"));
    LOGOS_ASSERT_EQ(ops["rpc.subscription_terminated"]["action"], json("send"));
    LOGOS_ASSERT_EQ(ops["rpc.unsubscribe"]["action"], json("receive"));
    LOGOS_ASSERT_EQ(ops["rpc.unsubscribe"]["reply"]["messages"][0],
                    j(R"({"$ref":"#/channels/ws/messages/rpc.unsubscribed"})"));
}

LOGOS_TEST(asyncapi_messages_have_the_wire_shapes_and_correlate_by_id) {
    const json doc = buildAsyncApi(sample());
    const json& msgs = doc["components"]["messages"];
    for (const char* key : {"sample.move.request", "sample.move.result", "rpc.error", "sample.moved.subscribe",
                            "sample.moved.subscribed", "rpc.unsubscribe", "rpc.unsubscribed"})
        LOGOS_ASSERT_EQ(msgs[key]["correlationId"], j(R"({"$ref":"#/components/correlationIds/requestId"})"));
    for (const char* key : {"sample.moved.event", "rpc.subscription_terminated"})
        LOGOS_ASSERT_EQ(msgs[key]["correlationId"], j(R"({"$ref":"#/components/correlationIds/subscriptionId"})"));
    const json& ids = doc["components"]["correlationIds"];
    LOGOS_ASSERT_EQ(ids.size(), static_cast<size_t>(2));
    LOGOS_ASSERT_EQ(ids["requestId"]["location"], json("$message.payload#/id"));
    LOGOS_ASSERT_EQ(ids["subscriptionId"]["location"], json("$message.payload#/params/subscription"));

    const json& request = msgs["sample.move.request"]["payload"];
    LOGOS_ASSERT_EQ(request["properties"]["method"], j(R"({"const":"sample.move"})"));
    LOGOS_ASSERT_EQ(request["properties"]["id"], j(R"({"$ref":"#/components/schemas/JsonRpcId"})"));
    // The operation carries the description; its request message only the summary.
    LOGOS_ASSERT_EQ(msgs["sample.move.request"]["summary"], json("Move to a point."));
    LOGOS_ASSERT_FALSE(msgs["sample.move.request"].contains("description"));
    LOGOS_ASSERT_CONTAINS(doc["operations"]["sample.move"]["description"].get<std::string>(), "```json");
    LOGOS_ASSERT_EQ(request["required"], j(R"(["jsonrpc","id","method","params"])"));
    const json& params = request["properties"]["params"]["oneOf"];
    LOGOS_ASSERT_EQ(params[0]["type"], json("object"));
    LOGOS_ASSERT_EQ(params[1]["additionalItems"], json(false));

    const json& result = msgs["sample.move.result"]["payload"];
    LOGOS_ASSERT_EQ(result["required"], j(R"(["jsonrpc","id","result"])"));
    LOGOS_ASSERT_EQ(result["properties"]["result"]["anyOf"][1],
                    j(R"({"$ref":"#/components/schemas/ProviderRejection"})"));

    const json& ack = msgs["sample.moved.subscribed"]["payload"]["properties"]["result"];
    LOGOS_ASSERT_EQ(ack["properties"]["module"], j(R"({"const":"sample"})"));
    LOGOS_ASSERT_EQ(ack["properties"]["event"], j(R"({"const":"moved"})"));
    LOGOS_ASSERT_EQ(ack["properties"]["state"], j(R"({"$ref":"#/components/schemas/SubscriptionState"})"));
    LOGOS_ASSERT_EQ(ack["properties"]["subscription"], j(R"({"$ref":"#/components/schemas/SubscriptionId"})"));
    LOGOS_ASSERT_EQ(doc["components"]["schemas"]["SubscriptionState"]["enum"], j(R"(["registered","active"])"));

    const json& event = msgs["sample.moved.event"]["payload"];
    LOGOS_ASSERT_FALSE(event["properties"].contains("id"));
    LOGOS_ASSERT_EQ(event["properties"]["method"], j(R"({"const":"rpc.event"})"));
    const json& ep = event["properties"]["params"];
    LOGOS_ASSERT_EQ(ep["required"], j(R"(["subscription","module","event","data","generation","ts"])"));
    LOGOS_ASSERT_EQ(ep["properties"]["event"], j(R"({"const":"moved"})"));
    LOGOS_ASSERT_EQ(ep["properties"]["data"]["items"][1]["format"], json("uint64"));
    LOGOS_ASSERT_EQ(ep["properties"]["generation"], j(R"({"$ref":"#/components/schemas/EventGeneration"})"));
    LOGOS_ASSERT_EQ(ep["properties"]["ts"], j(R"({"$ref":"#/components/schemas/EventTimestamp"})"));
    LOGOS_ASSERT_EQ(doc["components"]["schemas"]["EventTimestamp"]["type"], json("integer"));

    const json& terminated = msgs["rpc.subscription_terminated"]["payload"]["properties"]["params"];
    LOGOS_ASSERT_TRUE(terminated["properties"].contains("reason"));
}

LOGOS_TEST(asyncapi_channel_lists_every_message_and_declares_the_subprotocol) {
    DocContext ctx = sample();
    ctx.subprotocol = "jsonrpc-bridge.v1";
    const json doc = buildAsyncApi(ctx);
    const json& channelMessages = doc["channels"]["ws"]["messages"];
    const json& messages = doc["components"]["messages"];
    LOGOS_ASSERT_EQ(channelMessages.size(), messages.size());
    for (auto it = messages.begin(); it != messages.end(); ++it)
        LOGOS_ASSERT_EQ(channelMessages[it.key()]["$ref"], json("#/components/messages/" + it.key()));
    // Every operation message is one of the channel's.
    for (const auto& op : doc["operations"]) {
        std::vector<json> refs(op["messages"].begin(), op["messages"].end());
        if (op.contains("reply")) refs.insert(refs.end(), op["reply"]["messages"].begin(), op["reply"]["messages"].end());
        for (const json& r : refs) {
            const std::string ref = r["$ref"].get<std::string>();
            const std::string prefix = "#/channels/ws/messages/";
            LOGOS_ASSERT_EQ(ref.rfind(prefix, 0), static_cast<size_t>(0));
            LOGOS_ASSERT_TRUE(channelMessages.contains(ref.substr(prefix.size())));
        }
    }
    const json& ws = doc["channels"]["ws"]["bindings"]["ws"];
    LOGOS_ASSERT_EQ(ws["bindingVersion"], json("0.1.0"));
    LOGOS_ASSERT_EQ(ws["method"], json("GET"));
    LOGOS_ASSERT_EQ(ws["headers"]["type"], json("object"));
    LOGOS_ASSERT_EQ(ws["headers"]["properties"]["Sec-WebSocket-Protocol"]["const"], json("jsonrpc-bridge.v1"));
    const json& codes = doc["x-logos-ws-close-codes"];
    std::vector<int> seen;
    for (const auto& c : codes) seen.push_back(c["code"].get<int>());
    LOGOS_ASSERT_TRUE(seen == (std::vector<int>{1003, 1006, 1008, 1009}));
    LOGOS_ASSERT_CONTAINS(codes[3]["description"].get<std::string>(), "max_frame_bytes (1048576");
}

// ── across documents ────────────────────────────────────────────────────────

LOGOS_TEST(an_ipv6_host_is_bracketed_everywhere) {
    for (const char* host : {"::1", "[::1]"}) {
        DocContext ctx = sample();
        ctx.host = host;
        ctx.port = 9000;
        const json rpc = buildOpenRpc(ctx);
        LOGOS_ASSERT_EQ(rpc["servers"][0]["url"], json("http://[::1]:9000/rpc"));
        LOGOS_ASSERT_EQ(rpc["servers"][1]["url"], json("ws://[::1]:9000/ws"));
        LOGOS_ASSERT_EQ(buildOpenApi(ctx)["servers"][0]["url"], json("http://[::1]:9000"));
        LOGOS_ASSERT_EQ(buildAsyncApi(ctx)["servers"]["bridge"]["host"], json("[::1]:9000"));
    }
    LOGOS_ASSERT_EQ(urlHost("localhost"), std::string("localhost"));
    LOGOS_ASSERT_EQ(urlHost("127.0.0.1"), std::string("127.0.0.1"));
}

LOGOS_TEST(the_context_names_the_version_and_paths) {
    DocContext ctx = sample();
    ctx.bridgeVersion = "9.8.7";
    ctx.title = "Node bridge";
    ctx.rpcPath = "/jsonrpc";
    ctx.wsPath = "/socket";
    const json rpc = buildOpenRpc(ctx);
    LOGOS_ASSERT_EQ(rpc["info"]["version"], json("9.8.7"));
    LOGOS_ASSERT_EQ(rpc["info"]["title"], json("Node bridge"));
    LOGOS_ASSERT_EQ(rpc["servers"][0]["url"], json("http://127.0.0.1:8645/jsonrpc"));
    LOGOS_ASSERT_EQ(buildOpenApi(ctx)["info"]["version"], json("9.8.7"));
    const json async = buildAsyncApi(ctx);
    LOGOS_ASSERT_EQ(async["info"]["version"], json("9.8.7"));
    LOGOS_ASSERT_EQ(async["channels"]["ws"]["address"], json("/socket"));
}

LOGOS_TEST(documents_are_deterministic) {
    for (const std::string& kind : kDocKinds) {
        const std::string a = buildDoc(kind, sample()).dump();
        const std::string b = buildDoc(kind, sample()).dump();
        LOGOS_ASSERT_EQ(a, b);
    }
}

LOGOS_TEST(each_document_uses_its_own_dialects_tuple_keywords) {
    DocContext ctx = sample();
    const std::set<std::string> rpc = keywords(buildOpenRpc(ctx));
    const std::set<std::string> api = keywords(buildOpenApi(ctx));
    const std::set<std::string> async = keywords(buildAsyncApi(ctx));
    LOGOS_ASSERT_FALSE(rpc.count("prefixItems"));
    LOGOS_ASSERT_FALSE(async.count("prefixItems"));
    LOGOS_ASSERT_FALSE(api.count("additionalItems"));
    LOGOS_ASSERT_TRUE(rpc.count("additionalItems"));
    LOGOS_ASSERT_TRUE(async.count("additionalItems"));
    LOGOS_ASSERT_TRUE(api.count("prefixItems"));
    for (const std::string& kind : kDocKinds)
        walk(buildDoc(kind, ctx), [&](const json& n, const std::string& key) {
            if (n.is_object() && n.contains("$ref")) LOGOS_ASSERT_EQ(n.size(), static_cast<size_t>(1));
            if (kind == "openapi" && key == "items") LOGOS_ASSERT_FALSE(n.is_array());
        });
}

// Policy restricts calls, not knowledge: a denied method loses its operation,
// while the record only it uses stays in the components.
LOGOS_TEST(denied_members_get_no_operation_but_their_records_stay) {
    const DocContext ctx = vaultContext();
    for (const std::string& kind : kDocKinds) {
        const json doc = buildDoc(kind, ctx);
        LOGOS_ASSERT_TRUE(doc["components"]["schemas"].contains("vault.Secret"));
        for (const auto& t : targets(doc)) {
            LOGOS_ASSERT_NE(t.second, std::string("reveal"));
            LOGOS_ASSERT_NE(t.second, std::string("leaked"));
        }
        LOGOS_ASSERT_TRUE(doc.dump().find("vault.reveal") == std::string::npos);
        LOGOS_ASSERT_TRUE(doc.dump().find("vault.leaked") == std::string::npos);
        LOGOS_ASSERT_EQ(doc["x-logos-modules"][0]["exposure"]["methods"],
                        j(R"(["open","name","version","lidl"])"));
        LOGOS_ASSERT_EQ(doc["x-logos-modules"][0]["exposure"]["events"], j(R"(["opened"])"));
    }
    LOGOS_ASSERT_TRUE(findByName(buildOpenRpc(ctx)["methods"], "vault.open") != nullptr);
    LOGOS_ASSERT_TRUE(buildAsyncApi(ctx)["operations"].contains("vault.opened.event"));
    LOGOS_ASSERT_EQ(buildOpenRpc(ctx)["x-logos-events"].size(), static_cast<size_t>(1));
}

LOGOS_TEST(every_ref_resolves_inside_its_own_document) {
    DocContext ipv6 = sample();
    ipv6.host = "::1";
    DocContext empty;
    const std::vector<DocContext> contexts = {sample(), vaultContext(), namesOnlyContext(), ipv6, empty};
    for (const DocContext& ctx : contexts)
        for (const std::string& kind : kDocKinds) {
            const std::vector<std::string> bad = unresolvedRefs(buildDoc(kind, ctx));
            if (!bad.empty()) throw LogosTestFailure(kind + ": " + bad.front());
        }
    // The check itself: dangling, external and malformed references are all reported.
    const json probe = j(R"({"a":{"b":1},"ok":{"$ref":"#/a/b"},"gone":{"$ref":"#/a/c"},
        "far":{"$ref":"https://example.com/x"},"odd":{"$ref":"#a"},"num":{"$ref":7}})");
    LOGOS_ASSERT_EQ(unresolvedRefs(probe).size(), static_cast<size_t>(4));
}

LOGOS_TEST(pending_modules_appear_only_in_x_logos_modules) {
    DocContext ctx = sample();
    ModuleDoc pending;
    pending.name = "later";
    pending.status = kStatusPending;
    pending.exposedMethods = {"shouldNotAppear"};
    ctx.modules.push_back(pending);
    for (const std::string& kind : kDocKinds) {
        const json doc = buildDoc(kind, ctx);
        LOGOS_ASSERT_EQ(doc["x-logos-modules"][1], j(R"({"name":"later","status":"pending",
            "exposure":{"methods":["shouldNotAppear"],"events":[]}})"));
        std::string rest = doc.dump();
        const std::string once = "\"shouldNotAppear\"";
        const std::size_t first = rest.find(once);
        LOGOS_ASSERT_TRUE(first != std::string::npos);
        LOGOS_ASSERT_TRUE(rest.find(once, first + 1) == std::string::npos);
        for (const auto& t : targets(doc)) LOGOS_ASSERT_NE(t.first, std::string("later"));
    }
    const json api = buildOpenApi(ctx);
    for (const auto& tag : api["tags"]) LOGOS_ASSERT_NE(tag["name"], json("later"));
}

LOGOS_TEST(untyped_and_invalid_modules_are_names_only) {
    const DocContext ctx = namesOnlyContext();

    const json rpc = buildOpenRpc(ctx);
    const json& add = *findByName(rpc["methods"], "legacy.add");
    LOGOS_ASSERT_EQ(add["x-logos-untyped"], json(true));
    LOGOS_ASSERT_EQ(add["params"], j(R"([{"name":"a","schema":{}},{"name":"b","schema":{}}])"));
    LOGOS_ASSERT_EQ(add["paramStructure"], json("either"));
    LOGOS_ASSERT_EQ(add["result"]["schema"], json::object());
    LOGOS_ASSERT_CONTAINS(add["summary"].get<std::string>(), "Untyped");
    LOGOS_ASSERT_FALSE(add.contains("description"));  // it would only repeat the summary
    const json& odd = *findByName(rpc["methods"], "legacy.odd");
    LOGOS_ASSERT_TRUE(names(odd["params"]) == (std::vector<std::string>{"arg0", "x"}));
    LOGOS_ASSERT_EQ(odd["paramStructure"], json("by-position"));
    LOGOS_ASSERT_EQ((*findByName(rpc["methods"], "legacy.unlisted"))["params"], json::array());
    const json& lidl = *findByName(rpc["methods"], "broken.lidl");
    LOGOS_ASSERT_EQ(lidl["x-logos-untyped"], json(true));
    LOGOS_ASSERT_FALSE(lidl.contains("x-logos-derived"));
    LOGOS_ASSERT_CONTAINS(lidl["description"].get<std::string>(), "interface_error");
    LOGOS_ASSERT_EQ(rpc["x-logos-events"][0]["dataSchema"], json::object());
    LOGOS_ASSERT_EQ(rpc["x-logos-events"][0]["x-logos-untyped"], json(true));
    LOGOS_ASSERT_EQ(rpc["x-logos-modules"][1]["interface_error"], json("3:1: expected 'module'"));
    LOGOS_ASSERT_FALSE(rpc["x-logos-modules"][1].contains("interface_sha256"));

    const json api = buildOpenApi(ctx);
    const json& op = api["paths"]["/modules/legacy/add"]["post"];
    LOGOS_ASSERT_EQ(op["x-logos-untyped"], json(true));
    LOGOS_ASSERT_EQ(op["requestBody"], j(R"({"$ref":"#/components/requestBodies/UntypedParams"})"));
    LOGOS_ASSERT_EQ(op["responses"]["200"], j(R"({"$ref":"#/components/responses/UntypedCallResult"})"));
    const json& untypedBody = api["components"]["requestBodies"]["UntypedParams"];
    LOGOS_ASSERT_EQ(untypedBody["content"]["application/json"]["schema"], json::object());
    LOGOS_ASSERT_EQ(untypedBody["required"], json(false));
    LOGOS_ASSERT_EQ(api["components"]["responses"]["UntypedCallResult"]["content"]["application/json"]
                       ["schema"]["oneOf"][0]["properties"]["result"], json::object());

    const json async = buildAsyncApi(ctx);
    LOGOS_ASSERT_EQ(async["operations"]["legacy.add"]["x-logos-untyped"], json(true));
    LOGOS_ASSERT_EQ(async["components"]["messages"]["legacy.add.request"]["payload"]["properties"]["params"],
                    json::object());
    LOGOS_ASSERT_EQ(async["components"]["messages"]["legacy.tick.event"]["payload"]["properties"]["params"]
                         ["properties"]["data"], j(R"({"description":"The event's parameters, by position."})"));
    // Only the shared schemas: an untyped module contributes no records.
    LOGOS_ASSERT_EQ(rpc["components"]["schemas"].size(), static_cast<size_t>(3));
}

LOGOS_TEST(an_ok_view_without_an_interface_degrades_to_names_only) {
    ModuleDoc m;
    m.name = "odd";
    m.status = kStatusOk;
    m.exposedMethods = {"x"};
    const json rpc = buildOpenRpc(sampleContext(m));
    LOGOS_ASSERT_EQ((*findByName(rpc["methods"], "odd.x"))["x-logos-untyped"], json(true));

    // A typed module whose exposure names something its contract lacks.
    ModuleDoc typed = typedDoc("sample", sampleAst());
    typed.exposedMethods.push_back("ghost");
    const json doc = buildOpenRpc(sampleContext(typed));
    const json& ghost = *findByName(doc["methods"], "sample.ghost");
    LOGOS_ASSERT_EQ(ghost["x-logos-untyped"], json(true));
    LOGOS_ASSERT_CONTAINS(ghost["summary"].get<std::string>(), "not declared");
}

LOGOS_TEST(x_logos_modules_carry_status_digests_and_exposure) {
    const ModuleDoc m = typedDoc("sample", sampleAst(), {"blob"});
    const json v = buildOpenApi(sampleContext(m))["x-logos-modules"][0];
    LOGOS_ASSERT_EQ(v["name"], json("sample"));
    LOGOS_ASSERT_EQ(v["status"], json("ok"));
    LOGOS_ASSERT_EQ(v["interface_sha256"], json(m.interfaceSha256));
    LOGOS_ASSERT_EQ(v["contract_sha256"], json(m.contractSha256));
    LOGOS_ASSERT_FALSE(v.contains("interface_error"));
    LOGOS_ASSERT_EQ(v["exposure"]["methods"], j(R"(["move","reset","name","version","lidl"])"));
    LOGOS_ASSERT_EQ(v["exposure"]["events"], j(R"(["moved"])"));
}

// ── text ────────────────────────────────────────────────────────────────────

LOGOS_TEST(summaries_take_the_first_sentence_within_120_bytes) {
    LOGOS_ASSERT_EQ(summary("Get the peer ID.\n\nMore."), std::string("Get the peer ID."));
    LOGOS_ASSERT_EQ(summary("  Starts late. Then more"), std::string("Starts late."));
    LOGOS_ASSERT_EQ(summary("Use it (e.g. a file) now. Later."), std::string("Use it (e.g. a file) now."));
    LOGOS_ASSERT_EQ(summary("Version 1.5 is fine\nnext"), std::string("Version 1.5 is fine"));
    LOGOS_ASSERT_EQ(summary(""), std::string());
    LOGOS_ASSERT_EQ(summary("\n \n"), std::string());

    // 118 ASCII bytes then a 3-byte character: the cut backs off to a boundary.
    const std::string long1 = std::string(118, 'a') + "\xE2\x80\x93" + "tail";
    const std::string s1 = summary(long1);
    LOGOS_ASSERT_LE(s1.size(), static_cast<size_t>(120));
    LOGOS_ASSERT_EQ(s1, std::string(117, 'a') + "\xE2\x80\xA6");
    LOGOS_ASSERT_TRUE(utf8Valid(s1));

    const std::string dashes = [] { std::string s; for (int i = 0; i < 60; ++i) s += "\xE2\x80\x93"; return s; }();
    const std::string s2 = summary(dashes);
    LOGOS_ASSERT_LE(s2.size(), static_cast<size_t>(120));
    LOGOS_ASSERT_TRUE(utf8Valid(s2));
    LOGOS_ASSERT_EQ(s2.substr(s2.size() - 3), std::string("\xE2\x80\xA6"));
    LOGOS_ASSERT_EQ(s2.size(), static_cast<size_t>(120));  // 39 dashes and the ellipsis

    const std::string exact(120, 'b');
    LOGOS_ASSERT_EQ(summary(exact), exact);
}

LOGOS_TEST(doxygen_code_blocks_become_fences) {
    LOGOS_ASSERT_EQ(description("Config:\n@code{.json}\n{\"a\": 1}\n@endcode\n\nDone.\n@param x kept"),
                    std::string("Config:\n```json\n{\"a\": 1}\n```\n\nDone.\n@param x kept"));
    LOGOS_ASSERT_EQ(description("Plain:\n@code\nx = 1\n@endcode"), std::string("Plain:\n```\nx = 1\n```"));
    LOGOS_ASSERT_EQ(description("Inline @code{.json} {} @endcode after"),
                    std::string("Inline\n```json\n{}\n```\nafter"));
    LOGOS_ASSERT_EQ(description("Open:\n@code{.json}\n[1]"), std::string("Open:\n```json\n[1]\n```"));
    LOGOS_ASSERT_EQ(description("mail me@code.org or @codex"), std::string("mail me@code.org or @codex"));
    LOGOS_ASSERT_EQ(description("@return nothing"), std::string("@return nothing"));

    const json doc = buildOpenRpc(sample());
    const std::string d = (*findByName(doc["methods"], "sample.move"))["description"].get<std::string>();
    LOGOS_ASSERT_CONTAINS(d, "```json\n{\"x\": 1}\n```");
    LOGOS_ASSERT_TRUE(d.find("@code") == std::string::npos);
    LOGOS_ASSERT_CONTAINS(d, "@param to where");
}

LOGOS_TEST(invalid_utf8_never_reaches_a_document) {
    LOGOS_ASSERT_TRUE(utf8Valid("plain \xE2\x80\x93 dash"));
    LOGOS_ASSERT_FALSE(utf8Valid("\xC0\xAF"));          // overlong
    LOGOS_ASSERT_FALSE(utf8Valid("\xED\xA0\x80"));      // surrogate
    LOGOS_ASSERT_FALSE(utf8Valid("\xF4\x90\x80\x80"));  // > U+10FFFF
    LOGOS_ASSERT_FALSE(utf8Valid("\xE2\x80"));          // truncated
    LOGOS_ASSERT_EQ(utf8Sanitize("a\xFF" "b"), std::string("a\xEF\xBF\xBD" "b"));

    ModuleDoc m = typedDoc("sample", sampleAst());
    m.interface["methods"][0]["description"] = "bad \xFF byte";
    m.interfaceError = "\xFE";
    m.exposedEvents.push_back("\xC3");
    for (const std::string& kind : kDocKinds) {
        const json doc = buildDoc(kind, sampleContext(m));
        const std::string dumped = doc.dump();  // strict: throws on invalid UTF-8
        LOGOS_ASSERT_TRUE(utf8Valid(dumped));
    }
}
