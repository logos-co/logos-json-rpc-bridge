#pragma once

// GET /openapi.json: the bridge's HTTP surface as an OpenAPI 3.1.1 document, with
// JSON Schema 2020-12 schemas. Anything identical across paths is a referenced component.

#include <string>

#include <nlohmann/json.hpp>

#include "doc_common.h"

namespace bridge {
namespace docs {

constexpr const char* kOpenApiVersion = "3.1.1";
constexpr const char* kOasDialect = "https://spec.openapis.org/oas/3.1/dialect/base";

namespace openapi {

using nlohmann::json;

constexpr const char* kJson = "application/json";

inline json jsonContent(json schema) {
    return json{{kJson, {{"schema", std::move(schema)}}}};
}

inline json responseRef(const char* name) {
    return json{{"$ref", std::string("#/components/responses/") + name}};
}

inline json requestBodyRef(const char* name) {
    return json{{"$ref", std::string("#/components/requestBodies/") + name}};
}

// lws answers these itself, with no body.
inline json gateResponses(bool post) {
    json r{{"401", responseRef("Unauthorized")}, {"403", responseRef("Forbidden")}};
    if (post) r["415"] = responseRef("UnsupportedMediaType");
    return r;
}

// The REST projection answers 200 with {result}, {error}, or a full error envelope.
inline json callResponse(json result) {
    json resultObject{{"type", "object"},
                      {"properties", {{"result", std::move(result)}}},
                      {"required", {"result"}},
                      {"additionalProperties", false}};
    return json{
        {"description", "The call's outcome: the method's value (or a ProviderRejection), or "
                        "a JSON-RPC error."},
        {"content", jsonContent(json{{"oneOf", json::array({std::move(resultObject),
                                                            schemaRef("ErrorBody"),
                                                            schemaRef("JsonRpcErrorResponse")})}})}};
}

inline json moduleOperation(const MethodEntry& e) {
    const std::string module = safe(e.module->name);
    json op{{"operationId", qualified(module, e.method)}, {"tags", json::array({module})}};
    addText(&op, methodText(e));
    json responses = gateResponses(true);
    if (!e.typed()) {
        op["requestBody"] = requestBodyRef("UntypedParams");
        responses["200"] = responseRef("UntypedCallResult");
        op["x-logos-untyped"] = true;
    } else {
        const json& decl = *e.decl;
        if (paramsOf(decl).empty()) {
            op["requestBody"] = requestBodyRef("NoParams");
        } else {
            const bool required = anyRequiredParam(decl);
            std::string text = "The parameters, by name (an object) or by position (an array of "
                               "exactly the declared arity, null for an empty optional slot).";
            if (!required) text += " With no required parameter the body may be empty.";
            json body{{"oneOf", json::array({paramsObject(decl, e.scope, Dialect::Draft2020_12),
                                             paramsTuple(decl, e.scope, Dialect::Draft2020_12)})}};
            op["requestBody"] = {{"description", text},
                                 {"required", required},
                                 {"content", jsonContent(std::move(body))}};
        }
        responses["200"] = callResponse(methodResultSchema(decl, e.scope, Dialect::Draft2020_12));
        if (detail::boolField(decl, "derived")) op["x-logos-derived"] = true;
    }
    op["responses"] = std::move(responses);
    op["x-logos-module"] = module;
    op["x-logos-method"] = safe(e.method);
    return op;
}

inline json getOperation(const char* id, const char* summary, const char* description,
                         json okSchema, const char* okDescription) {
    json responses = gateResponses(false);
    responses["200"] = {{"description", okDescription}, {"content", jsonContent(std::move(okSchema))}};
    return json{{"operationId", id},
                {"tags", json::array({"bridge"})},
                {"summary", summary},
                {"description", description},
                {"responses", std::move(responses)},
                {"x-logos-bridge-op", true}};
}

inline json fixedPaths() {
    json paths = json::object();
    paths["/healthz"]["get"] = getOperation(
        "getHealth", "Bridge health.",
        "Liveness and draining state. Gated like every other route.",
        json{{"type", "object"},
             {"properties",
              {{"status", {{"type", "string"}, {"enum", {"ok", "draining"}}}},
               {"uptime_seconds", {{"type", "integer"}, {"minimum", 0}}},
               {"protocol", {{"const", "json-rpc-2.0"}}}}},
             {"required", {"status", "uptime_seconds", "protocol"}}},
        "The bridge is serving.");
    paths["/modules"]["get"] = getOperation(
        "listModules", "Describe every exposed module.",
        "The rpc.list_modules views, in configuration order, without `interface`.",
        json{{"type", "array"}, {"items", moduleViewSchema(false)}}, "The module views.");

    json one = getOperation(
        "getModule", "Describe one exposed module.",
        "The rpc.schema view: interface_status, the complete LIDL contract as `interface` when "
        "ok, this bridge's exposure, the digests and the live report.",
        moduleViewSchema(true), "The module view.");
    one["parameters"] = json::array({json{{"name", "module"},
                                          {"in", "path"},
                                          {"required", true},
                                          {"schema", {{"type", "string"}}}}});
    one["responses"]["404"] = {
        {"description", "Not exposed or unknown: the same -32601 error object a denied call "
                        "gets."},
        {"content", jsonContent(schemaRef("JsonRpcError"))}};
    paths["/modules/{module}"]["get"] = std::move(one);

    paths["/openapi.json"]["get"] = getOperation(
        "getOpenApi", "This document.", "The OpenAPI 3.1.1 description of this bridge.",
        json{{"type", "object"}}, "This OpenAPI document.");
    paths["/asyncapi.json"]["get"] = getOperation(
        "getAsyncApi", "The WebSocket description.",
        "The AsyncAPI 3.0.0 description of this bridge's WebSocket endpoint.",
        json{{"type", "object"}}, "The AsyncAPI document.");

    json request{{"type", "object"},
                 {"properties", {{"jsonrpc", {{"const", "2.0"}}},
                                 {"id", schemaRef("JsonRpcId")},
                                 {"method", {{"type", "string"}}},
                                 {"params", {{"type", {"object", "array"}}}}}},
                 {"required", {"jsonrpc", "method"}}};
    json success{{"type", "object"},
                 {"properties", {{"jsonrpc", {{"const", "2.0"}}},
                                 {"id", schemaRef("JsonRpcId")},
                                 {"result", json::object()}}},
                 {"required", {"jsonrpc", "id", "result"}},
                 {"additionalProperties", false}};
    json response{{"oneOf", json::array({std::move(success), schemaRef("JsonRpcErrorResponse")})}};
    const auto batchOf = [](const json& item) {
        return json{{"oneOf", json::array({item, json{{"type", "array"},
                                                      {"minItems", 1},
                                                      {"items", item}}})}};
    };
    json rpc{{"operationId", "postRpc"},
             {"tags", json::array({"bridge"})},
             {"summary", "JSON-RPC 2.0 endpoint."},
             {"description", "One request or a batch. Every rpc.* operation and every "
                             "`<module>.<method>` alias (see rpc.discover) is accepted here; "
                             "events need the WebSocket endpoint (see /asyncapi.json). A request "
                             "without an id is still answered, with id null: a module call is "
                             "refused with -32600, and any other operation runs."},
             {"requestBody", {{"required", true}, {"content", jsonContent(batchOf(request))}}},
             {"x-logos-bridge-op", true}};
    json responses = gateResponses(true);
    responses["200"] = {{"description", "The response, or the batch of responses. JSON-RPC "
                                        "failures ride in the body at 200."},
                        {"content", jsonContent(batchOf(response))}};
    rpc["responses"] = std::move(responses);
    paths["/rpc"]["post"] = std::move(rpc);
    return paths;
}

inline json components() {
    json schemas{
        {"Bytes", bytesSchema()},
        {"LogosResult", logosResultSchema()},
        {"ProviderRejection", providerRejectionSchema()},
        {"JsonRpcId", jsonRpcIdSchema()},
        {"JsonRpcError", jsonRpcErrorSchema()},
        {"JsonRpcErrorResponse", jsonRpcErrorResponseSchema()},
        {"ErrorBody", {{"title", "ErrorBody"},
                       {"description", "A module route's refusal: the JSON-RPC error alone. "
                                       "Denied, unexposed and unknown targets all get the same "
                                       "-32601."},
                       {"type", "object"},
                       {"properties", {{"error", schemaRef("JsonRpcError")}}},
                       {"required", {"error"}},
                       {"additionalProperties", false}}},
    };
    json responses{
        {"Unauthorized", {{"description", "auth.mode is bearer and the request carries no valid "
                                          "`Authorization: Bearer` token. No body."}}},
        {"Forbidden", {{"description", "Refused before routing: the Host header is not a "
                                       "loopback literal this bridge bound, or the Origin is not "
                                       "in http.allowed_origins. No body."}}},
        {"UnsupportedMediaType", {{"description", "The Content-Type is not application/json, "
                                                  "which is required even for an empty body. No "
                                                  "body."}}},
        {"UntypedCallResult", callResponse(json::object())},
    };
    json requestBodies{
        {"NoParams", {{"description", "No parameters: send no body, {} or []."},
                      {"required", false},
                      {"content", jsonContent(noParamsSchema())}}},
        {"UntypedParams", {{"description", "Untyped: the parameters by name (an object) or by "
                                           "position (an array)."},
                           {"required", false},
                           {"content", jsonContent(json::object())}}},
    };
    return json{{"schemas", std::move(schemas)},
                {"responses", std::move(responses)},
                {"requestBodies", std::move(requestBodies)}};
}

constexpr const char* kInfoDescription =
    "The HTTP surface of the Logos JSON-RPC bridge. `POST /modules/<module>/<method>` calls an "
    "exposed module method with its parameters as the body, typed from the module's canonical "
    "LIDL contract when it has one (x-logos-untyped marks names-only modules). `POST /rpc` "
    "speaks JSON-RPC 2.0 (see rpc.discover). Every route checks Host and Origin and sends no "
    "CORS headers. Operations cover what this bridge lets you call; component schemas cover "
    "every record in each contract, and x-logos-modules gives each module's status, digests "
    "and exposure.";

} // namespace openapi

inline nlohmann::json buildOpenApi(const DocContext& ctx) {
    using nlohmann::json;
    json doc = json::object();
    doc["openapi"] = kOpenApiVersion;
    doc["jsonSchemaDialect"] = kOasDialect;
    doc["info"] = {{"title", safe(ctx.title)},
                   {"version", safe(ctx.bridgeVersion)},
                   {"description", openapi::kInfoDescription}};
    doc["servers"] = json::array({json{{"url", httpOrigin(ctx)},
                                       {"description", "Loopback only."}}});

    json tags = json::array({json{{"name", "bridge"},
                                  {"description", "The bridge's own routes."}}});
    for (const ModuleDoc& m : ctx.modules) {
        if (!hasOperations(m)) continue;
        json tag{{"name", safe(m.name)}};
        const std::string text = isTyped(m) ? detail::strField(m.interface, "description")
                                            : untypedNote(m);
        if (!text.empty()) tag["description"] = description(text);
        tags.push_back(std::move(tag));
    }
    doc["tags"] = std::move(tags);

    json paths = openapi::fixedPaths();
    for (const MethodEntry& e : methodEntries(ctx)) {
        const std::string path = "/modules/" + safe(e.module->name) + "/" + safe(e.method);
        paths[path]["post"] = openapi::moduleOperation(e);
    }
    doc["paths"] = std::move(paths);

    json comps = openapi::components();
    for (const ModuleDoc& m : ctx.modules)
        if (isTyped(m))
            addRecordComponents(&comps["schemas"], m.name, m.interface, Dialect::Draft2020_12);
    doc["components"] = std::move(comps);
    doc["x-logos-modules"] = xLogosModules(ctx);
    return doc;
}

} // namespace docs
} // namespace bridge
