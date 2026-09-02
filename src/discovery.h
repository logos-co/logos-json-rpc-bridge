#pragma once

// Per-module interface cache and exposure resolution.
//
// Discovery does NOT use LpClient::getMethods(): on the default (qt_remote)
// transport that is a stub returning an empty array, so a bridge built on it
// would report every module as having no methods and no events. Instead it
// makes an ordinary by-name call to `getPluginInterface`, which ModuleProxy
// answers ahead of its authorization gate and which returns methods AND events
// in one array, events tagged "type":"event".
//
// That answer is the target module's own unvalidated self-report, not something
// the runtime checked. So everything derived from it is presented to clients as
// a bridge-derived VIEW, never as the module's contract.

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "bridge_config.h"
#include "upstream.h"

namespace bridge {

struct ModuleInterface {
    bool resolved = false;
    // False when the module reported no "type":"event" entries at all. Legacy
    // hand-written Qt plugins never tag events, so this is "we cannot know",
    // not "it has none" — the distinction the per-module events.allow list
    // exists to let an operator override.
    bool eventsDeclared = false;
    std::vector<std::string> methods;
    std::vector<std::string> events;
    // Parameter names per method, in declaration order, for translating a
    // by-name params object into the positional array the ABI takes.
    std::map<std::string, std::vector<std::string>> methodParams;
};

class Discovery {
public:
    Discovery(ClientRegistry* clients, const BridgeConfig* config)
        : m_clients(clients), m_config(config) {}

    // Fetch and cache one module's interface. Blocking; call from the pump or
    // the warm thread, never from the socket thread. Doubles as the client
    // warm-up: it is the call that triggers LpClient's lazy create.
    void refresh(const std::string& module) {
        auto client = m_clients->get(module);
        if (!client) return;
        logos::CallError err;
        nlohmann::json iface = client->invoke("getPluginInterface",
                                              nlohmann::json::array(), &err,
                                              /*timeout_ms=*/5000);
        if (!err.ok() || !iface.is_array()) return;

        ModuleInterface mi;
        mi.resolved = true;
        for (const auto& entry : iface) {
            if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string())
                continue;
            const std::string name = entry["name"].get<std::string>();
            const bool isEvent = entry.value("type", std::string()) == "event";
            if (isEvent) {
                mi.eventsDeclared = true;
                mi.events.push_back(name);
                continue;
            }
            mi.methods.push_back(name);
            std::vector<std::string> params;
            if (entry.contains("parameters") && entry["parameters"].is_array()) {
                for (const auto& p : entry["parameters"]) {
                    if (p.is_object() && p.contains("name") && p["name"].is_string())
                        params.push_back(p["name"].get<std::string>());
                    else
                        params.emplace_back();
                }
            }
            mi.methodParams.emplace(name, std::move(params));
        }
        std::lock_guard<std::mutex> lock(m_mu);
        m_cache[module] = std::move(mi);
    }

    ModuleInterface get(const std::string& module) const {
        std::lock_guard<std::mutex> lock(m_mu);
        auto it = m_cache.find(module);
        return it == m_cache.end() ? ModuleInterface{} : it->second;
    }

    // Is this (module, method) callable by an external client?
    //
    // Returns a single boolean on purpose. Not-exposed, denied, unknown module
    // and unknown method must be indistinguishable from outside, so the caller
    // has exactly one answer to turn into exactly one error.
    bool methodPermitted(const std::string& module, const std::string& method) const {
        const ExposedModule* em = m_config->find(module);
        if (!em) return false;
        if (!em->methods.permits(method)) return false;
        // An unresolved interface is not a reason to refuse: the module may
        // simply not be up yet, and refusing would make "starting" look like
        // "forbidden". The upstream call answers authoritatively.
        const ModuleInterface mi = get(module);
        if (!mi.resolved || mi.methods.empty()) return true;
        for (const auto& m : mi.methods)
            if (m == method) return true;
        return false;
    }

    // Same, for events. Stricter than methods, because subscribing to an event
    // name the module does not declare is a SILENT no-op upstream: the
    // subscription arms, reports healthy, and never fires. Pre-validating here
    // is the only place a client's typo can be turned into an error.
    bool eventPermitted(const std::string& module, const std::string& event) const {
        const ExposedModule* em = m_config->find(module);
        if (!em) return false;
        if (!em->events.permits(event)) return false;
        const ModuleInterface mi = get(module);
        // Not declared (a legacy Qt plugin) or not yet resolved: fall back to
        // the operator's explicit allow list, which is exactly the override
        // that case exists for. With no allow list there is nothing to check
        // against and we accept.
        if (!mi.resolved || !mi.eventsDeclared) return true;
        for (const auto& e : mi.events)
            if (e == event) return true;
        return false;
    }

    // Translate a by-name params object into the positional array the ABI
    // requires. Returns false when a name is not in the method's signature, or
    // when the signature is unknown and so cannot be ordered.
    bool toPositional(const std::string& module, const std::string& method,
                      const nlohmann::json& byName, nlohmann::json* out,
                      std::string* badPath) const {
        const ModuleInterface mi = get(module);
        auto it = mi.methodParams.find(method);
        if (it == mi.methodParams.end()) {
            *badPath = method;
            return false;
        }
        const auto& order = it->second;
        for (auto kv = byName.begin(); kv != byName.end(); ++kv) {
            bool known = false;
            for (const auto& n : order) if (n == kv.key()) { known = true; break; }
            if (!known) { *badPath = kv.key(); return false; }
        }
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& n : order)
            arr.push_back(byName.contains(n) ? byName.at(n) : nlohmann::json());
        *out = std::move(arr);
        return true;
    }

    // The bridge-derived view of one module, filtered to what is exposed.
    nlohmann::json describe(const std::string& module) const {
        const ExposedModule* em = m_config->find(module);
        if (!em) return nlohmann::json();
        const ModuleInterface mi = get(module);
        nlohmann::json methods = nlohmann::json::array();
        for (const auto& m : mi.methods)
            if (em->methods.permits(m)) methods.push_back(m);
        nlohmann::json events = nlohmann::json::array();
        for (const auto& e : mi.events)
            if (em->events.permits(e)) events.push_back(e);
        return nlohmann::json{
            {"module", module},
            {"resolved", mi.resolved},
            {"events_declared", mi.eventsDeclared},
            {"methods", std::move(methods)},
            {"events", std::move(events)},
            {"source", "getPluginInterface"},
            {"authoritative", false},
        };
    }

    nlohmann::json listModules() const {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& em : m_config->modules) out.push_back(describe(em.name));
        return out;
    }

private:
    ClientRegistry* m_clients;
    const BridgeConfig* m_config;
    mutable std::mutex m_mu;
    std::map<std::string, ModuleInterface> m_cache;
};

} // namespace bridge
