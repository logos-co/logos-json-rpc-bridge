#pragma once

// JSON-RPC 2.0 framing, shared verbatim by the HTTP and WebSocket transports.
// Pure: parsing and envelope construction only, no sockets and no upstream
// calls, so the unit tests cover the whole framing surface directly.

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "error_map.h"

namespace bridge {

// Bridge operations. Module and method are addressed as separate fields inside
// `params`, never dot-joined into the JSON-RPC method name: that mirrors the
// transport spec's Request (which keeps target and method distinct), keeps
// methods symmetric with events, and leaves module names and bridge operations
// in separate namespaces.
namespace op {
constexpr const char* kCall        = "rpc.call";
constexpr const char* kSubscribe   = "rpc.subscribe";
constexpr const char* kUnsubscribe = "rpc.unsubscribe";
constexpr const char* kSchema      = "rpc.schema";
constexpr const char* kListModules = "rpc.list_modules";
constexpr const char* kCancel      = "rpc.cancel";
constexpr const char* kPing        = "rpc.ping";
constexpr const char* kAuth        = "rpc.authenticate";
// Server-initiated notifications.
constexpr const char* kEvent       = "rpc.event";
constexpr const char* kTerminated  = "rpc.subscription_terminated";
} // namespace op

struct RpcRequest {
    nlohmann::json id;
    bool isNotification = true;   // the `id` KEY was absent
    std::string method;
    nlohmann::json params;
};

// A module call, after rpc.call's params have been validated.
struct CallTarget {
    std::string module;
    std::string method;
    nlohmann::json params;        // object (by name) or array (positional)
};

inline nlohmann::json makeError(const nlohmann::json& id, const MappedError& e) {
    return nlohmann::json{{"jsonrpc", "2.0"}, {"id", id}, {"error", e.toJson()}};
}

inline nlohmann::json makeErrorRaw(const nlohmann::json& id, nlohmann::json errObj) {
    return nlohmann::json{{"jsonrpc", "2.0"}, {"id", id}, {"error", std::move(errObj)}};
}

inline nlohmann::json makeResult(const nlohmann::json& id, nlohmann::json result) {
    return nlohmann::json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

inline nlohmann::json makeNotification(const char* method, nlohmann::json params) {
    return nlohmann::json{{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(params)}};
}

// A notification is the `id` key being ABSENT (JSON-RPC 2.0 section 4).
// `id: null` is a valid id and gets a response echoing null — the two are
// distinct, and conflating them leaves a WebSocket client's request unanswered
// forever with no error and no timeout.
inline bool parseRequest(const nlohmann::json& j, RpcRequest* out, MappedError* err) {
    if (!j.is_object()) {
        *err = {kInvalidRequest, LogosErrorCode::InvalidParams, "request must be an object"};
        return false;
    }
    if (j.value("jsonrpc", std::string()) != "2.0") {
        *err = {kInvalidRequest, LogosErrorCode::InvalidParams, "jsonrpc must be \"2.0\""};
        return false;
    }
    if (!j.contains("method") || !j["method"].is_string()) {
        *err = {kInvalidRequest, LogosErrorCode::InvalidParams, "method must be a string"};
        return false;
    }
    out->method = j["method"].get<std::string>();
    out->isNotification = !j.contains("id");
    out->id = out->isNotification ? nlohmann::json() : j["id"];
    if (!out->isNotification && !(out->id.is_string() || out->id.is_number() || out->id.is_null())) {
        *err = {kInvalidRequest, LogosErrorCode::InvalidParams, "id must be a string, number or null"};
        return false;
    }
    out->params = j.contains("params") ? j["params"] : nlohmann::json::object();
    if (!out->params.is_object() && !out->params.is_array()) {
        *err = {kInvalidParams, LogosErrorCode::InvalidParams, "params must be an object or array"};
        return false;
    }
    return true;
}

// rpc.call params: {"module": s, "method": s, "params": object|array|absent}.
inline bool parseCallTarget(const nlohmann::json& p, CallTarget* out, MappedError* err) {
    if (!p.is_object()) {
        *err = {kInvalidParams, LogosErrorCode::InvalidParams,
                "rpc.call params must be an object with \"module\" and \"method\""};
        return false;
    }
    if (!p.contains("module") || !p["module"].is_string() ||
        !p.contains("method") || !p["method"].is_string()) {
        *err = {kInvalidParams, LogosErrorCode::InvalidParams,
                "rpc.call requires string \"module\" and \"method\""};
        return false;
    }
    out->module = p["module"].get<std::string>();
    out->method = p["method"].get<std::string>();
    if (out->module.empty() || out->method.empty()) {
        *err = {kInvalidParams, LogosErrorCode::InvalidParams,
                "\"module\" and \"method\" must be non-empty"};
        return false;
    }
    out->params = p.contains("params") ? p["params"] : nlohmann::json::array();
    if (out->params.is_null()) out->params = nlohmann::json::array();
    if (!out->params.is_object() && !out->params.is_array()) {
        *err = {kInvalidParams, LogosErrorCode::InvalidParams,
                "rpc.call inner params must be an object or array"};
        return false;
    }
    return true;
}

// {"module": s, "event": s, "subscription": <client-assigned id>}.
// The subscription id is the CALLER's, per the transport spec — so a client can
// correlate without waiting for the ack.
struct SubscribeTarget {
    std::string module;
    std::string event;
    nlohmann::json subscription;
};

inline bool parseSubscribeTarget(const nlohmann::json& p, SubscribeTarget* out,
                                 MappedError* err, bool requireEvent) {
    if (!p.is_object()) {
        *err = {kInvalidParams, LogosErrorCode::InvalidParams, "params must be an object"};
        return false;
    }
    if (!p.contains("subscription") ||
        !(p["subscription"].is_string() || p["subscription"].is_number())) {
        *err = {kInvalidParams, LogosErrorCode::InvalidParams,
                "\"subscription\" (a caller-assigned id) is required"};
        return false;
    }
    out->subscription = p["subscription"];
    if (requireEvent) {
        if (!p.contains("module") || !p["module"].is_string() ||
            !p.contains("event") || !p["event"].is_string()) {
            *err = {kInvalidParams, LogosErrorCode::InvalidParams,
                    "rpc.subscribe requires string \"module\" and \"event\""};
            return false;
        }
        out->module = p["module"].get<std::string>();
        out->event  = p["event"].get<std::string>();
        if (out->module.empty() || out->event.empty()) {
            *err = {kInvalidParams, LogosErrorCode::InvalidParams,
                    "\"module\" and \"event\" must be non-empty"};
            return false;
        }
    }
    return true;
}

// Split a request body into individual requests. Returns false for a body that
// is not parseable or is an empty batch; `single` reports whether the response
// should be one object rather than an array.
inline bool splitBody(const std::string& body, std::vector<nlohmann::json>* out,
                      bool* single, nlohmann::json* protocolError) {
    auto j = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) {
        *protocolError = makeError(nlohmann::json(),
                                   {kParseError, LogosErrorCode::InvalidParams, "parse error"});
        return false;
    }
    if (j.is_array()) {
        *single = false;
        if (j.empty()) {
            *protocolError = makeError(nlohmann::json(),
                                       {kInvalidRequest, LogosErrorCode::InvalidParams, "empty batch"});
            return false;
        }
        for (auto& e : j) out->push_back(e);
        return true;
    }
    *single = true;
    out->push_back(std::move(j));
    return true;
}

} // namespace bridge
