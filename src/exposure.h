#pragma once

// What this bridge lets a client call or subscribe to. Pure, over plain name
// lists (contract or live report). Denied names are not secret, only unexposed.

#include <algorithm>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "bridge_config.h"

namespace bridge {

// ModuleProxy answers these ahead of its gate; lidl() and rpc.schema replace them.
inline bool isReservedIntrospection(const std::string& method) {
    return method == "getPluginInterface" || method == "getPluginMethods" ||
           method == "getPluginEvents";
}

struct Exposure {
    std::vector<std::string> methods;
    std::vector<std::string> events;

    nlohmann::json toJson() const {
        return nlohmann::json{{"methods", methods}, {"events", events}};
    }
};

namespace detail {
inline void addOnce(std::vector<std::string>* out, const std::string& name) {
    if (std::find(out->begin(), out->end(), name) == out->end()) out->push_back(name);
}
} // namespace detail

// Typed: the declared members the policy permits, in declaration order. The
// built-ins are always declared, and always permitted.
inline Exposure exposeDeclared(const ExposedModule& em,
                               const std::vector<std::string>& declaredMethods,
                               const std::vector<std::string>& declaredEvents) {
    Exposure x;
    for (const auto& m : declaredMethods)
        if (!isReservedIntrospection(m) && em.methodAllowed(m)) detail::addOnce(&x.methods, m);
    for (const auto& e : declaredEvents)
        if (em.events.permits(e)) detail::addOnce(&x.events, e);
    return x;
}

// Untyped or invalid: live names the policy permits, in live order. Untagged
// live events cannot be listed, so an explicit events.allow stands in for them.
inline Exposure exposeLive(const ExposedModule& em, const std::vector<std::string>& liveMethods,
                           const std::vector<std::string>& liveEvents, bool eventsDeclared) {
    Exposure x = exposeDeclared(em, liveMethods, liveEvents);
    if (!eventsDeclared && !em.events.allowUnconstrained) {
        for (const auto& e : em.events.allow)
            if (em.events.permits(e)) detail::addOnce(&x.events, e);
    }
    return x;
}

} // namespace bridge
