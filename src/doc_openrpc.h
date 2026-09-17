#pragma once

// rpc.discover: the bridge described as an OpenRPC 1.3.2 document, with
// JSON Schema draft-07 schemas. Pure function of the DocContext.

#include <algorithm>
#include <initializer_list>
#include <string>

#include <nlohmann/json.hpp>

#include "doc_common.h"

namespace bridge {
namespace docs {

constexpr const char* kOpenRpcVersion = "1.3.2";
constexpr const char* kErrorsPrefix = "#/components/errors/";

namespace openrpc {

using nlohmann::json;

// components.errors: the catalog, keyed by its stable names.
inline json errorComponents() {
    json out = json::object();
    for (const CatalogError& e : errorCatalog()) out[e.key] = e.error.toJson();
    return out;
}

// References to the catalog entries for these codes, in catalog order.
inline json errorRefs(std::initializer_list<int> codes) {
    json out = json::array();
    for (const CatalogError& e : errorCatalog())
        if (std::find(codes.begin(), codes.end(), e.error.jsonRpcCode) != codes.end())
            out.push_back(json{{"$ref", std::string(kErrorsPrefix) + e.key}});
    return out;
}

inline json allErrorRefs() {
    json out = json::array();
    for (const CatalogError& e : errorCatalog())
        out.push_back(json{{"$ref", std::string(kErrorsPrefix) + e.key}});
    return out;
}

inline json param(const std::string& name, json schema, bool required,
                  const std::string& description = std::string()) {
    json p{{"name", name}, {"schema", std::move(schema)}, {"required", required}};
    if (!description.empty()) p["description"] = description;
    return p;
}

inline json result(const std::string& name, json schema) {
    return json{{"name", name}, {"schema", std::move(schema)}};
}

inline json stringSchema() { return json{{"type", "string"}}; }

inline json bridgeMethod(const char* name, json params, const char* structure, json resultDesc,
                         json errors) {
    const BridgeOpText* text = bridgeOp(name);
    json m{{"name", name},
           {"params", std::move(params)},
           {"paramStructure", structure},
           {"result", std::move(resultDesc)},
           {"errors", std::move(errors)},
           {"tags", json::array({json{{"name", "bridge"}}})},
           {"x-logos-bridge-op", true}};
    if (text) {
        m["summary"] = text->summary;
        m["description"] = text->description;
    }
    return m;
}

inline json bridgeMethods() {
    const std::initializer_list<int> small = {kParseError, kInvalidRequest, kShuttingDown,
                                              kOverloaded};
    json subscribeAck{
        {"type", "object"},
        {"properties",
         {{"subscription", subscriptionIdSchema()},
          {"operation", {{"const", "subscribe"}}},
          {"module", stringSchema()},
          {"event", stringSchema()},
          {"state", {{"type", "string"},
                     {"enum", {"registered", "active"}},
                     {"description", "registered: held, arms when the provider appears; "
                                     "active: this id was already subscribed."}}}}},
        {"required", {"subscription", "operation", "module", "event", "state"}},
        {"additionalProperties", false}};
    json unsubscribeAck{
        {"type", "object"},
        {"properties", {{"subscription", subscriptionIdSchema()},
                        {"operation", {{"const", "unsubscribe"}}}}},
        {"required", {"subscription", "operation"}},
        {"additionalProperties", false}};
    json cancelAnswer{
        {"type", "object"},
        {"properties", {{"cancelled", {{"const", false}}},
                        {"reason", {{"const", "not_supported_upstream"}}}}},
        {"required", {"cancelled", "reason"}},
        {"additionalProperties", false}};

    json out = json::array();
    out.push_back(bridgeMethod(
        "rpc.call",
        json::array({param("module", stringSchema(), true),
                     param("method", stringSchema(), true),
                     param("params", json{{"type", {"object", "array"}}}, false,
                           "By name (object) or by position (array); absent means [].")}),
        "by-name",
        result("result", describe(json::object(),
                                  "The method's return value (see the <module>.<method> "
                                  "methods for typed results), or a ProviderRejection.")),
        allErrorRefs()));
    out.push_back(bridgeMethod(
        "rpc.subscribe",
        json::array({param("subscription", subscriptionIdSchema(), true),
                     param("module", stringSchema(), true),
                     param("event", stringSchema(), true)}),
        "by-name", result("subscribed", std::move(subscribeAck)),
        errorRefs({kParseError, kInvalidRequest, kMethodNotFound, kInvalidParams,
                   kShuttingDown, kOverloaded})));
    out.push_back(bridgeMethod(
        "rpc.unsubscribe",
        json::array({param("subscription", subscriptionIdSchema(), true)}),
        "by-name", result("unsubscribed", std::move(unsubscribeAck)),
        errorRefs({kParseError, kInvalidRequest, kInvalidParams, kShuttingDown,
                   kOverloaded})));
    out.push_back(bridgeMethod(
        "rpc.schema", json::array({param("module", stringSchema(), true)}), "by-name",
        result("view", moduleViewSchema(true)),
        errorRefs({kParseError, kInvalidRequest, kMethodNotFound, kInvalidParams,
                   kShuttingDown, kOverloaded})));
    out.push_back(bridgeMethod(
        "rpc.list_modules", json::array(), "either",
        result("views", json{{"type", "array"}, {"items", moduleViewSchema(false)}}),
        errorRefs(small)));
    out.push_back(bridgeMethod("rpc.ping", json::array(), "either",
                               result("pong", json{{"const", "pong"}}), errorRefs(small)));
    out.push_back(bridgeMethod(
        "rpc.cancel",
        json::array({param("id", jsonRpcIdSchema(), false,
                           "The id of the request to cancel.")}),
        "by-name", result("cancellation", std::move(cancelAnswer)), errorRefs(small)));
    // No external $ref: every reference in these documents resolves inside them.
    out.push_back(bridgeMethod(
        "rpc.discover", json::array(), "either",
        result("OpenRPC Schema",
               json{{"type", "object"},
                    {"description", "An OpenRPC 1.3.2 document: this one. Its meta-schema is "
                                    "https://meta.open-rpc.org/."}}),
        errorRefs(small)));
    return out;
}

inline std::string paramName(const json& p, std::size_t index) {
    const std::string name = safe(detail::strField(p, "name"));
    return name.empty() ? "arg" + std::to_string(index) : name;
}

inline json moduleMethod(const MethodEntry& e) {
    const std::string module = safe(e.module->name);
    json m{{"name", qualified(module, e.method)}};
    addText(&m, methodText(e));
    m["tags"] = json::array({json{{"name", module}}});
    json params = json::array();
    std::string structure = "either";
    if (e.typed()) {
        const json& decl = *e.decl;
        std::size_t i = 0;
        for (const auto& p : paramsOf(decl)) {
            params.push_back(param(paramName(p, i++), slotSchema(p, e.scope, Dialect::Draft07),
                                   !slotIsOptional(p)));
        }
        m["result"] = result("result", methodResultSchema(decl, e.scope, Dialect::Draft07));
        if (detail::boolField(decl, "derived")) m["x-logos-derived"] = true;
    } else {
        bool allNamed = true;
        for (const std::string& name : liveParamNames(e, &allNamed))
            params.push_back(json{{"name", name}, {"schema", json::object()}});
        if (!allNamed) structure = "by-position";
        m["result"] = result("result", json::object());
        m["x-logos-untyped"] = true;
    }
    m["params"] = std::move(params);
    m["paramStructure"] = structure;
    m["errors"] = allErrorRefs();
    m["x-logos-module"] = module;
    m["x-logos-method"] = safe(e.method);
    return m;
}

inline json eventDescriptor(const EventEntry& e) {
    const std::string module = safe(e.module->name);
    json v{{"name", qualified(module, e.event)},
           {"module", module},
           {"event", safe(e.event)},
           {"subscribe", "rpc.subscribe"},
           {"notification", "rpc.event"}};
    addText(&v, eventText(e));
    if (e.typed()) {
        v["params"] = declParamNames(*e.decl);
        v["dataSchema"] = paramsTuple(*e.decl, e.scope, Dialect::Draft07);
    } else {
        v["dataSchema"] = json::object();
        v["x-logos-untyped"] = true;
    }
    return v;
}

constexpr const char* kInfoDescription =
    "JSON-RPC 2.0 access to Logos modules. The `rpc.*` methods are the bridge's own "
    "operations (x-logos-bridge-op). Every exposed module method is listed as "
    "`<module>.<method>`, an exact alias for `rpc.call`, typed from the module's canonical "
    "LIDL contract when it has one; x-logos-untyped marks names-only modules. A result is "
    "the method's value or a ProviderRejection, and bytes travel as "
    "`{\"_bytes\": \"<base64url>\"}`. x-logos-events lists the subscribable events, delivered "
    "over WebSocket as `rpc.event`; x-logos-modules gives every module's status, digests and "
    "exposure. Policy restricts calls, not knowledge: methods cover what this bridge lets you "
    "call, while component schemas cover every record in each contract.";

} // namespace openrpc

inline nlohmann::json buildOpenRpc(const DocContext& ctx) {
    using nlohmann::json;
    json doc = json::object();
    doc["openrpc"] = kOpenRpcVersion;
    doc["info"] = {{"title", safe(ctx.title)},
                   {"version", safe(ctx.bridgeVersion)},
                   {"description", openrpc::kInfoDescription}};
    doc["servers"] = json::array({
        json{{"name", "http"},
             {"url", rpcUrl(ctx)},
             {"description", "JSON-RPC 2.0 over HTTP POST with Content-Type: "
                             "application/json. Events are not delivered over HTTP."}},
        json{{"name", "ws"},
             {"url", wsUrl(ctx)},
             {"description", "JSON-RPC 2.0 over WebSocket, subprotocol " +
                                 safe(ctx.subprotocol) +
                                 ": calls, subscriptions and rpc.event notifications."}},
    });

    json methods = openrpc::bridgeMethods();
    for (const MethodEntry& e : methodEntries(ctx)) methods.push_back(openrpc::moduleMethod(e));
    doc["methods"] = std::move(methods);

    json schemas{{"Bytes", bytesSchema()},
                 {"LogosResult", logosResultSchema()},
                 {"ProviderRejection", providerRejectionSchema()}};
    for (const ModuleDoc& m : ctx.modules)
        if (isTyped(m)) addRecordComponents(&schemas, m.name, m.interface, Dialect::Draft07);
    doc["components"] = {{"schemas", std::move(schemas)}, {"errors", openrpc::errorComponents()}};

    json events = json::array();
    for (const EventEntry& e : eventEntries(ctx)) events.push_back(openrpc::eventDescriptor(e));
    doc["x-logos-events"] = std::move(events);
    doc["x-logos-modules"] = xLogosModules(ctx);
    return doc;
}

} // namespace docs
} // namespace bridge
