#pragma once

// The builders' input and the pieces the three documents share. Pure. Documents are
// nlohmann::json (sorted keys), so dump() is deterministic; arrays keep build order.

#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "bridge_config.h"
#include "doc_text.h"
#include "error_map.h"
#include "schema_ir.h"

namespace bridge {
namespace docs {

struct LiveMethod { std::string name; std::vector<std::string> params; };   // untyped: live names + param names
struct ModuleDoc {
    std::string name;
    std::string status;                          // "pending" | "ok" | "untyped" | "invalid"
    nlohmann::json interface;                    // ok: full LIDL JSON AST after identity injection (see below); else null
    std::vector<std::string> exposedMethods;     // exposure.methods, in declaration order (ok) or live order (untyped/invalid)
    std::vector<std::string> exposedEvents;      // exposure.events, same ordering rule
    std::vector<LiveMethod> liveMethods;         // untyped/invalid only: live methods with param names (may be empty)
    std::string interfaceSha256;                 // ok only
    std::string contractSha256;                  // ok only
    std::string interfaceError;                  // invalid only
};
struct DocContext {
    std::string bridgeVersion;                   // module version (metadata.json)
    std::string title = "Logos JSON-RPC bridge";
    std::string host = "127.0.0.1"; int port = 8645;
    std::string wsPath = "/ws", rpcPath = "/rpc", subprotocol = "jsonrpc-bridge.v1";
    Limits limits;                               // from bridge_config.h
    std::vector<ModuleDoc> modules;              // config order
};

constexpr const char* kStatusPending = "pending";
constexpr const char* kStatusOk = "ok";
constexpr const char* kStatusUntyped = "untyped";
constexpr const char* kStatusInvalid = "invalid";

// Typed: operations come from `interface`.
inline bool isTyped(const ModuleDoc& m) {
    return m.status == kStatusOk && m.interface.is_object();
}

// Names-only operations. An ok view without an interface degrades here too.
inline bool isNamesOnly(const ModuleDoc& m) {
    return m.status == kStatusUntyped || m.status == kStatusInvalid ||
           (m.status == kStatusOk && !m.interface.is_object());
}

// Pending (or unrecognised) modules appear only in x-logos-modules.
inline bool hasOperations(const ModuleDoc& m) { return isTyped(m) || isNamesOnly(m); }

inline std::string safe(const std::string& s) { return utf8Sanitize(s); }

constexpr const char* kLidlBuiltinText = "The module's canonical LIDL interface document.";
constexpr const char* kLidlContractNote =
    "Returns this module's full canonical LIDL contract: the exact `.lidl` text its build "
    "published, never filtered by this bridge's policy.";
constexpr const char* kBuiltinNote =
    "Built-in: always callable for an exposed module, whatever this bridge's method policy.";

// ── the exposed surface, flattened ─────────────────────────────────────────

struct MethodEntry {
    const ModuleDoc* module = nullptr;
    std::string method;
    const nlohmann::json* decl = nullptr;   // typed only
    const LiveMethod* live = nullptr;       // names-only, when the live report named it
    TypeScope scope;
    bool typed() const { return decl != nullptr; }
};

struct EventEntry {
    const ModuleDoc* module = nullptr;
    std::string event;
    const nlohmann::json* decl = nullptr;   // typed only
    TypeScope scope;
    bool typed() const { return decl != nullptr; }
};

inline const nlohmann::json* findDecl(const nlohmann::json& interface, const char* list,
                                      const std::string& name) {
    if (const nlohmann::json* items = detail::arrayField(interface, list))
        for (const auto& item : *items)
            if (detail::strField(item, "name") == name) return &item;
    return nullptr;
}

// Exposed methods with an operation: modules in config order, members in exposure order.
inline std::vector<MethodEntry> methodEntries(const DocContext& ctx) {
    std::vector<MethodEntry> out;
    for (const ModuleDoc& m : ctx.modules) {
        if (!hasOperations(m)) continue;
        const bool typed = isTyped(m);
        const TypeScope scope = typed ? TypeScope::of(m.name, m.interface) : TypeScope{m.name, {}};
        std::set<std::string> seen;
        for (const std::string& name : m.exposedMethods) {
            if (name.empty() || !seen.insert(name).second) continue;
            MethodEntry e;
            e.module = &m;
            e.method = name;
            e.scope = scope;
            if (typed) e.decl = findDecl(m.interface, "methods", name);
            if (!e.decl)
                for (const LiveMethod& lm : m.liveMethods)
                    if (lm.name == name) { e.live = &lm; break; }
            out.push_back(std::move(e));
        }
    }
    return out;
}

inline std::vector<EventEntry> eventEntries(const DocContext& ctx) {
    std::vector<EventEntry> out;
    for (const ModuleDoc& m : ctx.modules) {
        if (!hasOperations(m)) continue;
        const bool typed = isTyped(m);
        const TypeScope scope = typed ? TypeScope::of(m.name, m.interface) : TypeScope{m.name, {}};
        std::set<std::string> seen;
        for (const std::string& name : m.exposedEvents) {
            if (name.empty() || !seen.insert(name).second) continue;
            EventEntry e;
            e.module = &m;
            e.event = name;
            e.scope = scope;
            if (typed) e.decl = findDecl(m.interface, "events", name);
            out.push_back(std::move(e));
        }
    }
    return out;
}

inline std::string qualified(const std::string& module, const std::string& member) {
    return safe(module) + "." + safe(member);
}

inline void appendParagraph(std::string* text, const std::string& paragraph) {
    if (paragraph.empty()) return;
    if (!text->empty()) *text += "\n\n";
    *text += paragraph;
}

// Why a member is names-only. A typed module lacking it means the exposure outran its contract.
inline std::string untypedNote(const ModuleDoc& m) {
    if (isTyped(m))
        return "Untyped: this member is not declared in the module's contract.";
    if (m.status == kStatusInvalid)
        return "Untyped: the module's lidl() contract was refused (see interface_error in "
               "x-logos-modules), so only names are known.";
    return "Untyped: the module publishes no lidl() contract, so only names are known.";
}

// The raw (Doxygen) text for a method; callers pass it through summary()/description().
inline std::string methodText(const MethodEntry& e) {
    std::string text = e.typed() ? detail::strField(*e.decl, "description")
                                 : untypedNote(*e.module);
    if (e.method == "lidl" && e.typed()) {
        if (text.empty()) text = kLidlBuiltinText;
        appendParagraph(&text, kLidlContractNote);
    }
    if (isBuiltinMethod(e.method)) appendParagraph(&text, kBuiltinNote);
    return text;
}

inline std::string eventText(const EventEntry& e) {
    return e.typed() ? detail::strField(*e.decl, "description") : untypedNote(*e.module);
}

// Live parameter names for a names-only method; unnamed slots become argN.
inline std::vector<std::string> liveParamNames(const MethodEntry& e, bool* allNamed) {
    std::vector<std::string> names;
    *allNamed = true;
    if (!e.live) return names;
    for (std::size_t i = 0; i < e.live->params.size(); ++i) {
        std::string n = safe(e.live->params[i]);
        if (n.empty()) { n = "arg" + std::to_string(i); *allNamed = false; }
        names.push_back(n);
    }
    return names;
}

inline std::vector<std::string> declParamNames(const nlohmann::json& decl) {
    std::vector<std::string> names;
    for (const auto& p : paramsOf(decl)) names.push_back(safe(detail::strField(p, "name")));
    return names;
}

// Adds a summary, and a description only when it says more than the summary.
inline void addText(nlohmann::json* obj, const std::string& raw) {
    const std::string s = summary(raw);
    const std::string d = description(raw);
    if (!s.empty()) (*obj)["summary"] = s;
    if (!d.empty() && d != s) (*obj)["description"] = d;
}

// ── x-logos-modules ────────────────────────────────────────────────────────

inline nlohmann::json safeList(const std::vector<std::string>& in) {
    nlohmann::json out = nlohmann::json::array();
    for (const std::string& s : in) out.push_back(safe(s));
    return out;
}

inline nlohmann::json xLogosModules(const DocContext& ctx) {
    nlohmann::json out = nlohmann::json::array();
    for (const ModuleDoc& m : ctx.modules) {
        nlohmann::json v{{"name", safe(m.name)}, {"status", safe(m.status)}};
        if (m.status == kStatusOk && !m.interfaceSha256.empty())
            v["interface_sha256"] = safe(m.interfaceSha256);
        if (m.status == kStatusOk && !m.contractSha256.empty())
            v["contract_sha256"] = safe(m.contractSha256);
        if (m.status == kStatusInvalid && !m.interfaceError.empty())
            v["interface_error"] = safe(m.interfaceError);
        v["exposure"] = {{"methods", safeList(m.exposedMethods)},
                         {"events", safeList(m.exposedEvents)}};
        out.push_back(std::move(v));
    }
    return out;
}

// ── servers ────────────────────────────────────────────────────────────────

// An IPv6 literal is bracketed in a URL authority.
inline std::string urlHost(const std::string& host) {
    const bool bracketed = host.size() >= 2 && host.front() == '[' && host.back() == ']';
    if (!bracketed && host.find(':') != std::string::npos) return "[" + host + "]";
    return host;
}

inline std::string hostPort(const DocContext& ctx) {
    return safe(urlHost(ctx.host)) + ":" + std::to_string(ctx.port);
}

inline std::string httpOrigin(const DocContext& ctx) { return "http://" + hostPort(ctx); }
inline std::string rpcUrl(const DocContext& ctx) { return httpOrigin(ctx) + safe(ctx.rpcPath); }
inline std::string wsUrl(const DocContext& ctx) {
    return "ws://" + hostPort(ctx) + safe(ctx.wsPath);
}

// ── WebSocket close codes ──────────────────────────────────────────────────

inline nlohmann::json closeCodes(const Limits& limits) {
    const auto row = [](int code, const char* name, const std::string& text) {
        return nlohmann::json{{"code", code}, {"name", name}, {"description", text}};
    };
    return nlohmann::json::array({
        row(1003, "unsupported_data", "Binary frame refused: the bridge accepts text frames only."),
        row(1006, "abnormal_closure",
            "Bridge stopped: connections are dropped without a close frame. 1006 is what a "
            "client reports; it is never sent."),
        row(1008, "policy_violation",
            "Outbound queue overflow: more than max_queued_frames_per_connection (" +
                std::to_string(limits.maxQueuedFramesPerConnection) +
                ") frames were waiting. A slow reader is closed rather than silently missing "
                "events."),
        row(1009, "message_too_big",
            "Inbound message larger than max_frame_bytes (" +
                std::to_string(limits.maxFrameBytes) + " bytes)."),
    });
}

// ── bridge operations ──────────────────────────────────────────────────────

struct BridgeOpText {
    const char* name;
    const char* summary;
    const char* description;
};

inline const std::vector<BridgeOpText>& bridgeOps() {
    static const std::vector<BridgeOpText> ops = {
        {"rpc.call", "Call a method on an exposed module.",
         "Calls `method` on `module` with `params`: an object (by name) or an array (by "
         "position); absent means `[]`. The result is the method's return value, or a "
         "ProviderRejection when the provider refused the arguments. An application failure "
         "(a LogosResult with success=false) is a successful call. Denied, unexposed and "
         "unknown targets all get the same -32601. Every module method below is also callable "
         "as `<module>.<method>`, an exact alias for this operation; notifications are "
         "refused."},
        {"rpc.subscribe", "Subscribe to an exposed module event.",
         "Registers the caller-assigned `subscription` id for `event` on `module`. Events "
         "arrive as `rpc.event` notifications on the WebSocket connection that subscribed, so "
         "this is WebSocket-only in practice: an HTTP request has no channel to deliver them "
         "on. The ack says `registered` because the upstream subscription arms when the "
         "provider appears; re-subscribing an id this connection already holds answers "
         "`active` and never double-delivers. A lost provider ends the subscription with "
         "`rpc.subscription_terminated`. See x-logos-events for each event's data."},
        {"rpc.unsubscribe", "Drop a subscription.",
         "Stops the subscription with this caller-assigned id. Idempotent: an unknown id is "
         "acknowledged too."},
        {"rpc.schema", "Describe one exposed module.",
         "The bridge's view of `module`: interface_status (pending, ok, untyped or invalid), "
         "the complete LIDL contract as `interface` when ok, this bridge's `exposure` (what "
         "can be called or subscribed to), the interface_sha256 and contract_sha256 digests, "
         "and the live report. Policy restricts calls, not knowledge: `interface` is never "
         "filtered. Unexposed and unknown modules get -32601."},
        {"rpc.list_modules", "Describe every exposed module.",
         "The same views as rpc.schema, in configuration order, without `interface`."},
        {"rpc.ping", "Check that the bridge answers.", "Answers \"pong\"."},
        {"rpc.cancel", "Cancel a request (not supported upstream).",
         "The module ABI has no cancel primitive, so this always answers "
         "`{\"cancelled\": false, \"reason\": \"not_supported_upstream\"}`. A request is "
         "never known to have had no effect."},
        {"rpc.discover", "Describe this bridge (this document).",
         "Returns the OpenRPC 1.3.2 description of this bridge: its operations, every exposed "
         "module method (typed from the module's lidl() contract where it has one), "
         "x-logos-events and x-logos-modules."},
    };
    return ops;
}

inline const BridgeOpText* bridgeOp(const std::string& name) {
    for (const BridgeOpText& op : bridgeOps())
        if (name == op.name) return &op;
    return nullptr;
}

// ── JSON-RPC shapes ────────────────────────────────────────────────────────

inline nlohmann::json jsonRpcIdSchema() {
    return nlohmann::json{{"title", "JsonRpcId"},
                          {"description", "A JSON-RPC request id; a reply echoes it."},
                          {"type", {"string", "number", "null"}}};
}

inline nlohmann::json subscriptionIdSchema() {
    return nlohmann::json{{"title", "SubscriptionId"},
                          {"description", "The caller-assigned subscription id."},
                          {"type", {"string", "number"}}};
}

inline nlohmann::json jsonRpcErrorSchema() {
    return nlohmann::json{
        {"title", "JsonRpcError"},
        {"description", "A JSON-RPC 2.0 error object, with the Logos error taxonomy in data."},
        {"type", "object"},
        {"properties",
         {{"code", {{"type", "integer"}}},
          {"message", {{"type", "string"}}},
          {"data",
           {{"type", "object"},
            {"properties",
             {{"logos_error_code", {{"type", "integer"}}},
              {"logos_error_name", {{"type", "string"}}},
              {"invalid_params_detail",
               {{"type", "object"},
                {"properties",
                 {{"reason", {{"type", "string"},
                              {"description", "schema-mismatch, malformed-cbor or "
                                              "non-deterministic-cbor."}}},
                  {"path", {{"type", "string"}}}}},
                {"required", {"reason"}}}}}},
            {"required", {"logos_error_code", "logos_error_name"}}}}}},
        {"required", {"code", "message"}},
    };
}

inline nlohmann::json jsonRpcErrorResponseSchema() {
    return nlohmann::json{
        {"title", "JsonRpcErrorResponse"},
        {"description", "A complete JSON-RPC 2.0 error response."},
        {"type", "object"},
        {"properties",
         {{"jsonrpc", {{"const", "2.0"}}},
          {"id", schemaRef("JsonRpcId")},
          {"error", schemaRef("JsonRpcError")}}},
        {"required", {"jsonrpc", "id", "error"}},
        {"additionalProperties", false},
    };
}

// ── shared schemas, defined once per document and referenced ───────────────

inline nlohmann::json noParamsSchema() {
    return nlohmann::json{
        {"title", "NoParams"},
        {"description", "No parameters: an empty object or an empty array."},
        {"oneOf", nlohmann::json::array({nlohmann::json{{"type", "object"}, {"maxProperties", 0}},
                                         nlohmann::json{{"type", "array"}, {"maxItems", 0}}})}};
}

inline nlohmann::json subscriptionStateSchema() {
    return nlohmann::json{
        {"title", "SubscriptionState"},
        {"description", "registered: the upstream subscription is held and arms when the "
                        "provider appears. active: this connection already held the id, so "
                        "nothing new was registered and nothing is delivered twice."},
        {"type", "string"},
        {"enum", {"registered", "active"}}};
}

inline nlohmann::json eventGenerationSchema() {
    return nlohmann::json{
        {"title", "EventGeneration"},
        {"description", "The upstream subscription's incarnation; it grows each time the "
                        "provider is lost."},
        {"type", "integer"},
        {"minimum", 0}};
}

inline nlohmann::json eventTimestampSchema() {
    return nlohmann::json{
        {"title", "EventTimestamp"},
        {"description", "Delivery time, in milliseconds since the Unix epoch."},
        {"type", "integer"},
        {"minimum", 0}};
}

// A loose description of rpc.schema / GET /modules views; the runtime owns the full shape.
inline nlohmann::json moduleViewSchema(bool withInterface) {
    // Digests and cross_check are null until the view has them.
    const nlohmann::json digest{{"type", {"string", "null"}}, {"pattern", "^[0-9a-f]{64}$"}};
    const nlohmann::json names{{"type", "array"}, {"items", {{"type", "string"}}}};
    nlohmann::json properties{
        {"module", {{"type", "string"}}},
        {"interface_status", {{"type", "string"},
                              {"enum", {kStatusPending, kStatusOk, kStatusUntyped,
                                        kStatusInvalid}}}},
        {"exposure", {{"type", "object"},
                      {"properties", {{"methods", names}, {"events", names}}}}},
        {"interface_sha256", digest},
        {"contract_sha256", digest},
        {"interface_error", {{"type", "string"}}},
        {"cross_check", {{"type", {"object", "null"}}}},
        {"source", {{"type", "string"}}},
        {"authoritative", {{"type", "boolean"}}},
    };
    if (withInterface)
        properties["interface"] = {
            {"type", "object"},
            {"description", "The complete LIDL JSON AST (as `lidl json --identity` prints it); "
                            "present only when interface_status is ok."}};
    return nlohmann::json{
        {"type", "object"},
        {"description", withInterface
                            ? "The bridge's view of one module. Keys not listed here are the "
                              "live report."
                            : "The bridge's view of one module, without `interface`."},
        {"properties", std::move(properties)},
        {"required", {"module"}},
    };
}

} // namespace docs
} // namespace bridge
