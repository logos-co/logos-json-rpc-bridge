#pragma once

// What the bridge knows about each exposed module, and every decision that
// follows from it. Pure: no IO, no threads, no locks. discovery.h does the IO.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "bridge_config.h"

namespace bridge {

enum class InterfaceStatus { Pending, Ok, Untyped, Invalid };

inline const char* interfaceStatusName(InterfaceStatus s) {
    switch (s) {
        case InterfaceStatus::Pending: return "pending";
        case InterfaceStatus::Ok:      return "ok";
        case InterfaceStatus::Untyped: return "untyped";
        case InterfaceStatus::Invalid: return "invalid";
    }
    return "pending";
}

// ---------------------------------------------------------------------------
// Live report: the module's own getPluginInterface answer, names only
// ---------------------------------------------------------------------------

struct LiveMember {
    std::string name;
    std::vector<std::string> params;   // "" where the host gave no name
};

struct LiveReport {
    // False when no entry was tagged "type":"event". Legacy Qt plugins never
    // tag events, so this means "cannot know", not "has none".
    bool eventsDeclared = false;
    std::vector<LiveMember> methods;   // host order; Qt default-argument clones repeat a name
    std::vector<LiveMember> events;

    const LiveMember* method(const std::string& name) const {
        for (const auto& m : methods)
            if (m.name == name) return &m;
        return nullptr;
    }
    bool hasMethod(const std::string& name) const { return method(name) != nullptr; }
    bool hasEvent(const std::string& name) const {
        for (const auto& e : events)
            if (e.name == name) return true;
        return false;
    }
};

// False when `iface` is not an array. Entries without a string name are skipped.
inline bool parseLiveReport(const nlohmann::json& iface, LiveReport* out) {
    if (!iface.is_array()) return false;
    LiveReport r;
    for (const auto& entry : iface) {
        if (!entry.is_object()) continue;
        const auto name = entry.find("name");
        if (name == entry.end() || !name->is_string()) continue;
        LiveMember m;
        m.name = name->get<std::string>();
        const auto params = entry.find("parameters");
        if (params != entry.end() && params->is_array()) {
            for (const auto& p : *params) {
                const bool named = p.is_object() && p.contains("name") && p["name"].is_string();
                m.params.push_back(named ? p["name"].get<std::string>() : std::string());
            }
        }
        const auto type = entry.find("type");
        if (type != entry.end() && type->is_string() && type->get<std::string>() == "event") {
            r.eventsDeclared = true;
            r.events.push_back(std::move(m));
        } else {
            r.methods.push_back(std::move(m));
        }
    }
    *out = std::move(r);
    return true;
}

// ---------------------------------------------------------------------------
// Module view and its status machine
// ---------------------------------------------------------------------------

// One immutable snapshot per module; discovery swaps whole snapshots.
struct ModuleView {
    std::string module;
    InterfaceStatus status = InterfaceStatus::Pending;
    LiveReport live;                 // empty while pending
    std::uint64_t generation = 0;    // completed discoveries of this module
    bool stale = false;              // the provider went away since; a refresh is due
    std::string interfaceError;      // invalid only

    bool resolved() const { return status != InterfaceStatus::Pending; }
};

inline ModuleView pendingView(const std::string& module) {
    ModuleView v;
    v.module = module;
    return v;
}

// Every completed discovery goes through here: a fresh snapshot, never stale.
inline ModuleView nextView(const ModuleView& prev, InterfaceStatus status, LiveReport live) {
    ModuleView v;
    v.module = prev.module;
    v.status = status;
    v.live = std::move(live);
    v.generation = prev.generation + 1;
    return v;
}

// pending|ok|untyped|invalid -> untyped: a live report and nothing more.
inline ModuleView untypedView(const ModuleView& prev, LiveReport live) {
    return nextView(prev, InterfaceStatus::Untyped, std::move(live));
}

// A lost provider keeps its last view for gating (the upstream call answers
// authoritatively anyway) until a refresh replaces it. Pending stays pending.
inline ModuleView staleView(const ModuleView& prev) {
    ModuleView v = prev;
    v.stale = prev.resolved();
    return v;
}

// ---------------------------------------------------------------------------
// Retry backoff
// ---------------------------------------------------------------------------

constexpr std::chrono::milliseconds kRetryBase{500};
constexpr std::chrono::milliseconds kRetryCap{30000};

// Delay before the next attempt after `failures` consecutive misses: 0.5 s doubling to 30 s.
inline std::chrono::milliseconds retryDelay(int failures) {
    std::chrono::milliseconds d = kRetryBase;
    for (int i = 1; i < failures && d < kRetryCap; ++i) d *= 2;
    return std::min(d, kRetryCap);
}

// ---------------------------------------------------------------------------
// Gating
// ---------------------------------------------------------------------------

// Is (module, method) callable by an external client? One boolean on purpose:
// not exposed, denied and unknown must be indistinguishable from outside.
inline bool methodPermitted(const BridgeConfig& cfg, const ModuleView& view,
                            const std::string& method) {
    const ExposedModule* em = cfg.find(view.module);
    if (!em || !em->methods.permits(method)) return false;
    // Unresolved is not a refusal: "starting" must not look like "forbidden",
    // and the upstream call answers authoritatively.
    if (!view.resolved() || view.live.methods.empty()) return true;
    return view.live.hasMethod(method);
}

// Stricter than methods: subscribing to an undeclared event is a SILENT no-op
// upstream, so this is the only place a client's typo becomes an error.
inline bool eventPermitted(const BridgeConfig& cfg, const ModuleView& view,
                           const std::string& event) {
    const ExposedModule* em = cfg.find(view.module);
    if (!em || !em->events.permits(event)) return false;
    // Undeclared (a legacy Qt plugin) or unresolved: the operator's allow list
    // is the override that case exists for.
    if (!view.resolved() || !view.live.eventsDeclared) return true;
    return view.live.hasEvent(event);
}

// By-name params -> the positional array the ABI takes. False when a name is
// not in the signature, or the signature is unknown; *badPath names which.
inline bool toPositional(const ModuleView& view, const std::string& method,
                         const nlohmann::json& byName, nlohmann::json* out,
                         std::string* badPath) {
    const LiveMember* m = view.resolved() ? view.live.method(method) : nullptr;
    if (!m) {
        *badPath = method;
        return false;
    }
    for (auto kv = byName.begin(); kv != byName.end(); ++kv) {
        if (std::find(m->params.begin(), m->params.end(), kv.key()) == m->params.end()) {
            *badPath = kv.key();
            return false;
        }
    }
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& n : m->params)
        arr.push_back(byName.contains(n) ? byName.at(n) : nlohmann::json());
    *out = std::move(arr);
    return true;
}

// ---------------------------------------------------------------------------
// Views as JSON
// ---------------------------------------------------------------------------

// rpc.schema / GET /modules/{m}. A bridge-derived view, never the module's word.
inline nlohmann::json describeView(const ExposedModule& em, const ModuleView& view) {
    nlohmann::json methods = nlohmann::json::array();
    for (const auto& m : view.live.methods)
        if (em.methods.permits(m.name)) methods.push_back(m.name);
    nlohmann::json events = nlohmann::json::array();
    for (const auto& e : view.live.events)
        if (em.events.permits(e.name)) events.push_back(e.name);
    return nlohmann::json{
        {"module", view.module},
        {"resolved", view.resolved()},
        {"events_declared", view.live.eventsDeclared},
        {"methods", std::move(methods)},
        {"events", std::move(events)},
        {"source", "getPluginInterface"},
        {"authoritative", false},
        {"interface_status", interfaceStatusName(view.status)},
        {"stale", view.stale},
    };
}

// rpc.list_modules / GET /modules entries: the same view, minus the contract.
inline nlohmann::json listView(const ExposedModule& em, const ModuleView& view) {
    return describeView(em, view);
}

} // namespace bridge
