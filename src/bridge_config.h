#pragma once

// Config parsing, validation, and exposure resolution.
//
// Header-only and deliberately PURE: no sockets, no threads, no IPC, no
// logos_sdk.h. Everything here is a function of the config string, which is
// what lets the unit tests cover the whole policy surface — the part most
// likely to be wrong in a way that silently exposes something — without
// standing up a server or a module host.

#include <cctype>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace bridge {

// ---------------------------------------------------------------------------
// Allow/deny resolution
// ---------------------------------------------------------------------------

// One axis of per-module exposure (methods, or events).
//
// Semantics follow the spec's own convention, not the more obvious one: an
// ABSENT allow list means UNCONSTRAINED, an EMPTY one permits NOTHING. Those
// are different, and collapsing them is how an operator writing `"allow": []`
// to mean "I haven't decided yet" ends up exposing everything.
//
// `deny` always subtracts, and is applied after `allow` — so a name in both is
// denied. Stated rather than left to the reader because the opposite precedence
// is just as defensible and silently more permissive.
struct NamePolicy {
    bool allowUnconstrained = true;   // no allow list was given
    std::set<std::string> allow;
    std::set<std::string> deny;

    bool permits(const std::string& name) const {
        if (deny.count(name)) return false;
        if (allowUnconstrained) return true;
        return allow.count(name) > 0;
    }

    // For getInfo()/README output: what the operator actually configured.
    nlohmann::json describe() const {
        nlohmann::json j;
        j["allow"] = allowUnconstrained ? nlohmann::json()
                                        : nlohmann::json(std::vector<std::string>(
                                              allow.begin(), allow.end()));
        j["deny"] = std::vector<std::string>(deny.begin(), deny.end());
        return j;
    }
};

struct ExposedModule {
    std::string name;
    NamePolicy methods;
    NamePolicy events;
};

struct Limits {
    int maxConnections = 128;
    int maxConnectionsPerPeer = 8;
    int maxBodyBytes = 1048576;
    int maxFrameBytes = 1048576;
    int maxInFlightPerConnection = 32;
    int maxInFlightTotal = 256;
    int maxSubscriptionsPerConnection = 256;
    int maxQueuedFramesPerConnection = 256;
    int callTimeoutMs = 30000;
};

enum class AuthMode { None, Bearer };

struct BridgeConfig {
    std::string host = "127.0.0.1";
    int port = 8645;
    // Empty means: refuse any request that carries an Origin header at all.
    // That is the safe default for a loopback service — a browser will happily
    // connect to 127.0.0.1 from any page, and does not preflight WebSockets.
    std::vector<std::string> allowedOrigins;
    AuthMode authMode = AuthMode::None;
    std::vector<ExposedModule> modules;
    Limits limits;

    const ExposedModule* find(const std::string& moduleName) const {
        for (const auto& m : modules)
            if (m.name == moduleName) return &m;
        return nullptr;
    }
};

struct ConfigParseResult {
    bool ok = false;
    std::string error;
    BridgeConfig config;
};

// ---------------------------------------------------------------------------

namespace detail {

// Loopback literals only. Deliberately NOT a DNS lookup: resolving a name here
// would let "localhost" point wherever a hosts file says, which is exactly the
// rebinding class this check exists to close. `localhost` itself is accepted as
// a literal because every client spells it that way and the bind call resolves
// it locally.
inline bool isLoopbackHost(const std::string& h) {
    if (h == "localhost" || h == "::1" || h == "[::1]") return true;
    // 127.0.0.0/8
    if (h.rfind("127.", 0) == 0) {
        for (char c : h)
            if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.') return false;
        return true;
    }
    return false;
}

inline bool readPolicy(const nlohmann::json& j, NamePolicy* out, std::string* err,
                       const std::string& where) {
    if (!j.is_object()) { *err = where + " must be an object"; return false; }
    if (j.contains("allow")) {
        if (!j["allow"].is_array()) { *err = where + ".allow must be an array"; return false; }
        out->allowUnconstrained = false;
        for (const auto& e : j["allow"]) {
            if (!e.is_string()) { *err = where + ".allow entries must be strings"; return false; }
            if (!out->allow.insert(e.get<std::string>()).second) {
                // A duplicate is a mistake in a hand-written allowlist, and
                // silently deduplicating it hides the fact that the operator
                // wrote the same rule twice — usually meaning they meant two
                // different ones.
                *err = where + ".allow contains a duplicate: " + e.get<std::string>();
                return false;
            }
        }
    }
    if (j.contains("deny")) {
        if (!j["deny"].is_array()) { *err = where + ".deny must be an array"; return false; }
        for (const auto& e : j["deny"]) {
            if (!e.is_string()) { *err = where + ".deny entries must be strings"; return false; }
            if (!out->deny.insert(e.get<std::string>()).second) {
                *err = where + ".deny contains a duplicate: " + e.get<std::string>();
                return false;
            }
        }
    }
    return true;
}

inline int readInt(const nlohmann::json& j, const char* key, int fallback) {
    if (!j.is_object() || !j.contains(key)) return fallback;
    const auto& v = j[key];
    return v.is_number_integer() ? v.get<int>() : fallback;
}

} // namespace detail

// Parse and validate. `selfModuleName` is this module's own registry name; it
// is refused as an exposure target because bridging the bridge is an
// unbounded recursion with a socket at each level.
inline ConfigParseResult parseBridgeConfig(const std::string& configJson,
                                           const std::string& selfModuleName) {
    ConfigParseResult r;
    auto j = nlohmann::json::parse(configJson, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) { r.error = "config is not valid JSON"; return r; }
    if (!j.is_object())   { r.error = "config must be a JSON object"; return r; }

    BridgeConfig c;

    // ---- http -------------------------------------------------------------
    if (j.contains("http")) {
        const auto& h = j["http"];
        if (!h.is_object()) { r.error = "http must be an object"; return r; }
        if (h.contains("host")) {
            if (!h["host"].is_string()) { r.error = "http.host must be a string"; return r; }
            c.host = h["host"].get<std::string>();
        }
        c.port = detail::readInt(h, "port", c.port);
        if (h.contains("allowed_origins")) {
            if (!h["allowed_origins"].is_array()) {
                r.error = "http.allowed_origins must be an array"; return r;
            }
            for (const auto& o : h["allowed_origins"]) {
                if (!o.is_string()) { r.error = "http.allowed_origins entries must be strings"; return r; }
                c.allowedOrigins.push_back(o.get<std::string>());
            }
        }
    }

    // Refused, not warned. This module hands whoever reaches it the ability to
    // call every exposed module, and it authenticates as itself — so a
    // routable bind is not a configuration choice, it is a different product.
    if (!detail::isLoopbackHost(c.host)) {
        r.error = "http.host must be a loopback address (got '" + c.host +
                  "'). This module binds loopback only; expose it beyond the "
                  "host with an SSH tunnel or a reverse proxy that terminates "
                  "its own authentication.";
        return r;
    }
    if (c.port <= 0 || c.port > 65535) {
        r.error = "http.port must be in 1..65535"; return r;
    }

    // ---- auth -------------------------------------------------------------
    if (j.contains("auth")) {
        const auto& a = j["auth"];
        if (!a.is_object()) { r.error = "auth must be an object"; return r; }
        if (a.contains("mode")) {
            if (!a["mode"].is_string()) { r.error = "auth.mode must be a string"; return r; }
            const std::string m = a["mode"].get<std::string>();
            if      (m == "none")   c.authMode = AuthMode::None;
            else if (m == "bearer") c.authMode = AuthMode::Bearer;
            else { r.error = "auth.mode must be \"none\" or \"bearer\" (got '" + m + "')"; return r; }
        }
        // Deliberately NO token/token_file key. A secret does not belong in a
        // config document that gets logged, echoed by getInfo(), and pasted
        // into issues; the bearer secret is read from the module's own
        // per-instance persistence directory instead.
        if (a.contains("token") || a.contains("token_file")) {
            r.error = "auth.token / auth.token_file are not accepted -- the bearer "
                      "secret is read from the module's instance persistence "
                      "directory, so it never travels through config";
            return r;
        }
    }

    // ---- expose -----------------------------------------------------------
    if (!j.contains("expose") || !j["expose"].is_object()) {
        r.error = "expose is required and must be an object"; return r;
    }
    const auto& ex = j["expose"];
    if (!ex.contains("modules") || !ex["modules"].is_array()) {
        r.error = "expose.modules is required and must be an array"; return r;
    }
    for (const auto& m : ex["modules"]) {
        ExposedModule em;
        if (m.is_string()) {
            em.name = m.get<std::string>();
        } else if (m.is_object()) {
            if (!m.contains("name") || !m["name"].is_string()) {
                r.error = "each expose.modules entry needs a string \"name\""; return r;
            }
            em.name = m["name"].get<std::string>();
            if (m.contains("methods") &&
                !detail::readPolicy(m["methods"], &em.methods, &r.error,
                                    "expose.modules[" + em.name + "].methods"))
                return r;
            if (m.contains("events") &&
                !detail::readPolicy(m["events"], &em.events, &r.error,
                                    "expose.modules[" + em.name + "].events"))
                return r;
        } else {
            r.error = "expose.modules entries must be a string or an object"; return r;
        }

        if (em.name.empty()) { r.error = "expose.modules contains an empty name"; return r; }
        if (!selfModuleName.empty() && em.name == selfModuleName) {
            r.error = "expose.modules must not contain this module itself (\"" +
                      selfModuleName + "\") -- bridging the bridge recurses";
            return r;
        }
        for (const auto& seen : c.modules) {
            if (seen.name != em.name) continue;
            r.error = "expose.modules lists '" + em.name + "' twice";
            return r;
        }
        c.modules.push_back(std::move(em));
    }
    if (c.modules.empty()) {
        r.error = "expose.modules is empty -- the bridge would serve nothing. "
                  "There is no wildcard: name every module you intend to expose.";
        return r;
    }

    // ---- limits -----------------------------------------------------------
    if (j.contains("limits")) {
        const auto& L = j["limits"];
        if (!L.is_object()) { r.error = "limits must be an object"; return r; }
        Limits d;
        c.limits.maxConnections              = detail::readInt(L, "max_connections", d.maxConnections);
        c.limits.maxConnectionsPerPeer       = detail::readInt(L, "max_connections_per_peer", d.maxConnectionsPerPeer);
        c.limits.maxBodyBytes                = detail::readInt(L, "max_body_bytes", d.maxBodyBytes);
        c.limits.maxFrameBytes               = detail::readInt(L, "max_frame_bytes", d.maxFrameBytes);
        c.limits.maxInFlightPerConnection    = detail::readInt(L, "max_in_flight_per_connection", d.maxInFlightPerConnection);
        c.limits.maxInFlightTotal            = detail::readInt(L, "max_in_flight_total", d.maxInFlightTotal);
        c.limits.maxSubscriptionsPerConnection = detail::readInt(L, "max_subscriptions_per_connection", d.maxSubscriptionsPerConnection);
        c.limits.maxQueuedFramesPerConnection  = detail::readInt(L, "max_queued_frames_per_connection", d.maxQueuedFramesPerConnection);
        c.limits.callTimeoutMs               = detail::readInt(L, "call_timeout_ms", d.callTimeoutMs);

        const struct { const char* name; int v; } positive[] = {
            {"max_connections", c.limits.maxConnections},
            {"max_connections_per_peer", c.limits.maxConnectionsPerPeer},
            {"max_body_bytes", c.limits.maxBodyBytes},
            {"max_frame_bytes", c.limits.maxFrameBytes},
            {"max_in_flight_per_connection", c.limits.maxInFlightPerConnection},
            {"max_in_flight_total", c.limits.maxInFlightTotal},
            {"max_subscriptions_per_connection", c.limits.maxSubscriptionsPerConnection},
            {"max_queued_frames_per_connection", c.limits.maxQueuedFramesPerConnection},
            {"call_timeout_ms", c.limits.callTimeoutMs},
        };
        for (const auto& p : positive) {
            if (p.v <= 0) {
                r.error = std::string("limits.") + p.name + " must be positive";
                return r;
            }
        }
    }

    r.ok = true;
    r.config = std::move(c);
    return r;
}

} // namespace bridge
