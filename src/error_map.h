#pragma once

// The single place an outcome becomes a JSON-RPC error object.
//
// Header-only and pure, so the unit tests can prove the mapping is TOTAL over
// the shipped CallError vocabulary. That totality is the point: an unmapped
// code silently becoming "internal error" is how a timeout ends up
// indistinguishable from a crash on the client side.
//
// TWO VOCABULARIES, deliberately, because neither alone is enough:
//
//   * the JSON-RPC `code`, so a stock client library behaves sensibly (its
//     range is coarse and mostly reserved, and it cannot express "the provider
//     restarted");
//   * the Logos `error_code` name + number in `error.data`, carried
//     losslessly, so a Logos-aware client gets the real taxonomy.
//
// WHAT IS NOT AN ERROR HERE, and this is the rule most likely to be broken by a
// later edit: an application-level failure is a SUCCESSFUL call. Only a
// populated logos::CallError — transport, timeout, authorization, dispatch —
// becomes a JSON-RPC error. A provider that ran and returned
// StdLogosResult{success:false} answered the call; its whole payload belongs in
// `result`. LOGOS-MODULE-TRANSPORT section 4.2 says so directly ("An expected
// domain or application failure defined by the selected response schema MUST be
// returned in response-ok.result and MUST NOT be converted to
// response-err"), and StdLogosResult having `success` and `error` members makes
// the mistake an easy one to reintroduce. Do not inspect a result payload here.

#include <string>

#include <nlohmann/json.hpp>

namespace bridge {

// The Logos error taxonomy (LOGOS-MODULE-INTERFACE). Names and numbers are the
// spec's, including the British spelling of NOT_AUTHORISED.
enum class LogosErrorCode {
    MethodNotFound  = 1,
    InvalidParams   = 2,
    ModuleError     = 3,
    NotAuthorised   = 4,
    TransportError  = 5,
    Timeout         = 6,
    VersionMismatch = 7,
    NotReady        = 8,
    Cancelled       = 9,
};

inline const char* logosErrorName(LogosErrorCode c) {
    switch (c) {
        case LogosErrorCode::MethodNotFound:  return "METHOD_NOT_FOUND";
        case LogosErrorCode::InvalidParams:   return "INVALID_PARAMS";
        case LogosErrorCode::ModuleError:     return "MODULE_ERROR";
        case LogosErrorCode::NotAuthorised:   return "NOT_AUTHORISED";
        case LogosErrorCode::TransportError:  return "TRANSPORT_ERROR";
        case LogosErrorCode::Timeout:         return "TIMEOUT";
        case LogosErrorCode::VersionMismatch: return "VERSION_MISMATCH";
        case LogosErrorCode::NotReady:        return "NOT_READY";
        case LogosErrorCode::Cancelled:       return "CANCELLED";
    }
    return "MODULE_ERROR";
}

// JSON-RPC codes. -32000..-32099 is the implementation-defined server range.
enum JsonRpcCode {
    kParseError     = -32700,
    kInvalidRequest = -32600,
    kMethodNotFound = -32601,
    kInvalidParams  = -32602,
    kInternalError  = -32603,
    kModuleError    = -32000,
    kUnavailable    = -32001,
    kTimeout        = -32002,
    kTransportError = -32003,
    kUnauthorized   = -32004,
    kCancelled      = -32005,
    kShuttingDown   = -32006,
    kOverloaded     = -32029,
};

struct MappedError {
    int jsonRpcCode = kInternalError;
    LogosErrorCode logosCode = LogosErrorCode::ModuleError;
    std::string message;

    nlohmann::json toJson() const {
        nlohmann::json e;
        e["code"] = jsonRpcCode;
        e["message"] = message;
        e["data"] = nlohmann::json{
            {"logos_error_code", static_cast<int>(logosCode)},
            {"logos_error_name", logosErrorName(logosCode)},
        };
        return e;
    }
};

// The one message every "you may not have this" condition answers with.
//
// A module that is not loaded, a module that is loaded but not in
// expose.modules, a method excluded by methods.deny, and a method that simply
// does not exist MUST be indistinguishable — otherwise the bridge is an oracle
// for what a node is running and what it has been told to hide. Same JSON-RPC
// code, same Logos code, same string, and (on the REST route) the same HTTP
// status.
inline MappedError notFound() {
    return {kMethodNotFound, LogosErrorCode::MethodNotFound, "method not found"};
}

// Map a shipped CallError code. `codes` here is the vocabulary produced by
// logos-protocol (logos_call_error.h documents the first five;
// lp_invoke's argument parsing adds the invalid_arg pair).
//
// UNKNOWN codes deliberately land on MODULE_ERROR/-32603 rather than being
// asserted away: logos_call_error.h says outright that the set is strings
// rather than an enum "so the set can grow", so a protocol newer than this
// bridge WILL hand it something not listed here, and refusing to answer would
// be worse than answering coarsely. The unit test pins the known set so the
// fallback stays a fallback.
// `message` is accepted so a caller can LOG the upstream detail, and is
// deliberately never read into the response. Every string below is a constant.
// The first version of this function passed the upstream message through for
// timeouts, which the "never echoed" unit test caught: a module's own error text
// routinely carries nix store paths, socket paths and instance ids.
inline MappedError mapCallError(const std::string& code, const std::string& /*message*/) {
    if (code == "timeout")
        return {kTimeout, LogosErrorCode::Timeout, "upstream call timed out"};
    if (code == "object_unavailable")
        return {kUnavailable, LogosErrorCode::NotReady, "module unavailable"};
    if (code == "transport_error")
        return {kTransportError, LogosErrorCode::TransportError, "transport error"};
    if (code == "unauthorized")
        return {kUnauthorized, LogosErrorCode::NotAuthorised, "not authorised"};
    if (code == "call_failed")
        return {kModuleError, LogosErrorCode::ModuleError, "call could not be dispatched"};
    if (code == "invalid_args" || code == "invalid_arg")
        return {kInvalidParams, LogosErrorCode::InvalidParams, "invalid params"};
    return {kInternalError, LogosErrorCode::ModuleError, "upstream call failed"};
}

// Bad params, in the spec's shape: logos.invalid_params_detail = {reason,
// ? path}. `reason` is one of the three the spec allows and `detail` is
// permitted ONLY alongside INVALID_PARAMS, which is why it lives here rather
// than being attachable to any error.
inline MappedError invalidParams(const std::string& reason, const std::string& path = {}) {
    MappedError e{kInvalidParams, LogosErrorCode::InvalidParams, "invalid params"};
    (void)reason; (void)path;
    return e;
}

inline nlohmann::json invalidParamsJson(const std::string& reason, const std::string& path) {
    nlohmann::json e = invalidParams(reason, path).toJson();
    nlohmann::json detail;
    detail["reason"] = reason;             // schema-mismatch | malformed-cbor | non-deterministic-cbor
    if (!path.empty()) detail["path"] = path;
    e["data"]["invalid_params_detail"] = std::move(detail);
    return e;
}

// NOTE on messages: none of the strings above interpolate anything from
// upstream. A module's own error text routinely carries nix store paths, socket
// paths and instance ids, and forwarding it verbatim to an external client
// leaks the node's filesystem layout. Upstream detail is logged locally, never
// serialised into a response.

} // namespace bridge
