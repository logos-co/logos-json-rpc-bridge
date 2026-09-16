#pragma once

// Discovery IO: getPluginInterface, then lidl() and version() when listed, with
// retry and rediscovery. Decisions live in discovery_model.h.

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "bridge_config.h"
#include "discovery_model.h"
#include "lidl_contract.h"
#include "sha256.h"

namespace bridge {

struct CallOutcome {
    nlohmann::json value;
    std::string error;   // CallError code; empty on success
    bool ok() const { return error.empty(); }
};

struct DiscoveryIo {
    using Done = std::function<void(CallOutcome)>;
    using Job = std::function<void()>;
    // Start a zero-argument upstream call; `done` runs at most once, on any thread.
    std::function<void(const std::string& module, const std::string& method,
                       int timeoutMs, Done done)> invoke;
    // Queue work on the pump; false once it is stopping.
    std::function<bool(Job)> post;
    // Run `job` after `delay`, replacing any timer already set for `key`.
    std::function<bool(const std::string& key, std::chrono::milliseconds delay, Job job)> schedule;
};

class Discovery {
public:
    static constexpr int kCallTimeoutMs = 5000;

    Discovery(const BridgeConfig* config, DiscoveryIo io)
        : m_config(config), m_io(std::move(io)) {
        for (const auto& em : m_config->modules)
            m_slots[em.name].view = std::make_shared<const ModuleView>(pendingView(em.name));
    }

    // Warm-up: one refresh per exposed module, off the request path. The first
    // call is also what triggers LpClient's lazy (blocking) create.
    void start() {
        for (const auto& em : m_config->modules) {
            const std::string name = em.name;
            m_io.post([this, name] { refresh(name); });
        }
    }

    // Start a discovery of `module`. Call from the pump, never the socket thread.
    void refresh(const std::string& module) {
        std::uint64_t attempt = 0;
        {
            std::lock_guard<std::mutex> lock(m_mu);
            Slot* s = slot(module);
            if (!s) return;
            attempt = ++s->attempt;
            s->inFlight = true;
            s->retryArmed = false;
        }
        // ModuleProxy answers this ahead of its gate; LpClient::getMethods() is a qt_remote stub.
        call(module, attempt, "getPluginInterface",
             [this, module, attempt](CallOutcome r) { onInterface(module, attempt, std::move(r)); });
    }

    // The provider went away (the subscription hub's per-module watcher).
    void onProviderLost(const std::string& module) {
        std::chrono::milliseconds delay{};
        {
            std::lock_guard<std::mutex> lock(m_mu);
            Slot* s = slot(module);
            if (!s) return;
            ++s->attempt;   // anything in flight answered for the provider that just left
            s->inFlight = false;
            s->view = std::make_shared<const ModuleView>(staleView(*s->view));
            s->failures = 1;
            s->retryArmed = true;
            delay = retryDelay(s->failures);
        }
        scheduleRefresh(module, delay);
    }

    // A (re)establishment of the module's subscriptions. A higher generation
    // than the last one seen means the provider came back: rediscover now.
    void onProviderArmed(const std::string& module, std::uint64_t generation) {
        {
            std::lock_guard<std::mutex> lock(m_mu);
            Slot* s = slot(module);
            if (!s) return;
            const bool restarted = s->armedGeneration != 0 && generation > s->armedGeneration;
            if (generation > s->armedGeneration) s->armedGeneration = generation;
            if (!restarted && !s->view->stale) return;
            ++s->attempt;
            s->inFlight = false;
            s->retryArmed = true;
            s->view = std::make_shared<const ModuleView>(staleView(*s->view));
        }
        scheduleRefresh(module, std::chrono::milliseconds(0));
    }

    // A bridged call found the module unavailable. The loss watcher only runs
    // for modules with subscriptions, so this is how the rest go stale.
    void onCallUnavailable(const std::string& module) {
        std::chrono::milliseconds delay{};
        {
            std::lock_guard<std::mutex> lock(m_mu);
            Slot* s = slot(module);
            if (!s || s->inFlight || s->retryArmed) return;
            s->view = std::make_shared<const ModuleView>(staleView(*s->view));
            s->failures = 1;
            s->retryArmed = true;
            delay = retryDelay(s->failures);
        }
        scheduleRefresh(module, delay);
    }

    // The current snapshot; a pending view for a module that is not exposed.
    std::shared_ptr<const ModuleView> view(const std::string& module) const {
        std::lock_guard<std::mutex> lock(m_mu);
        auto it = m_slots.find(module);
        if (it == m_slots.end()) return std::make_shared<const ModuleView>(pendingView(module));
        return it->second.view;
    }

    bool methodPermitted(const std::string& module, const std::string& method) const {
        return bridge::methodPermitted(*m_config, *view(module), method);
    }

    bool eventPermitted(const std::string& module, const std::string& event) const {
        return bridge::eventPermitted(*m_config, *view(module), event);
    }

    bool toPositional(const std::string& module, const std::string& method,
                      const nlohmann::json& byName, nlohmann::json* out,
                      std::string* badPath) const {
        return bridge::toPositional(*view(module), method, byName, out, badPath);
    }

    // Null when the module is not exposed.
    nlohmann::json describe(const std::string& module) const {
        const ExposedModule* em = m_config->find(module);
        if (!em) return nlohmann::json();
        return describeView(*em, *view(module));
    }

    nlohmann::json listModules() const {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& em : m_config->modules) out.push_back(listView(em, *view(em.name)));
        return out;
    }

private:
    struct Slot {
        std::shared_ptr<const ModuleView> view;
        std::uint64_t attempt = 0;          // bumps per refresh; older completions are dropped
        bool inFlight = false;
        bool retryArmed = false;
        int failures = 0;                   // consecutive attempts without a live report
        std::uint64_t armedGeneration = 0;  // highest subscription generation seen
    };

    Slot* slot(const std::string& module) {
        auto it = m_slots.find(module);
        return it == m_slots.end() ? nullptr : &it->second;
    }

    bool current(const std::string& module, std::uint64_t attempt) const {
        std::lock_guard<std::mutex> lock(m_mu);
        auto it = m_slots.find(module);
        return it != m_slots.end() && it->second.attempt == attempt;
    }

    // Async, resumed on the pump, so a down module never holds a pump thread.
    // A superseded attempt is not resumed; a refused post means we are stopping.
    void call(const std::string& module, std::uint64_t attempt, const std::string& method,
              std::function<void(CallOutcome)> next) {
        m_io.invoke(module, method, kCallTimeoutMs,
            [this, module, attempt, next = std::move(next)](CallOutcome r) {
                m_io.post([this, module, attempt, next, r = std::move(r)]() mutable {
                    if (current(module, attempt)) next(std::move(r));
                });
            });
    }

    void onInterface(const std::string& module, std::uint64_t attempt, CallOutcome r) {
        LiveReport live;
        if (!r.ok() || !parseLiveReport(r.value, &live)) {
            missed(module, attempt);
            return;
        }
        if (!lidlPresent(live)) {
            publish(module, attempt, untypedView(*view(module), std::move(live)));
            return;
        }
        call(module, attempt, "lidl", [this, module, attempt, live](CallOutcome a) mutable {
            onContract(module, attempt, std::move(live), std::move(a));
        });
    }

    void onContract(const std::string& module, std::uint64_t attempt, LiveReport live,
                    CallOutcome answer) {
        const LidlText text = lidlText(answer.ok(), answer.value);
        if (!text.ok) {
            publish(module, attempt, invalidView(*view(module), std::move(live), text.error),
                    /*retry=*/text.retryable);
            return;
        }
        const std::string contractSha256 = sha256Hex(text.text);
        const ContractResult contract = readContract(text.text, module);
        std::shared_ptr<const TypedContract> typed;
        std::string error;
        if (!contract.ok) error = contractErrorText(contract, lidlReaderRev());
        else if (!(typed = typedContract(contract))) error = "the contract could not be served";
        if (!typed) {
            ModuleView v = invalidView(*view(module), std::move(live), error);
            v.contractSha256 = contractSha256;
            publish(module, attempt, std::move(v));
            return;
        }
        // One version() call for the cross-check; a failure is only an info finding.
        call(module, attempt, "version",
             [this, module, attempt, live, typed, contractSha256](CallOutcome v) mutable {
                 publish(module, attempt,
                         checkedView(*view(module), std::move(live), std::move(typed),
                                     runtimeVersion(v.ok(), v.value), contractSha256));
             });
    }

    // No live report: keep the view (pending, or stale) and retry with backoff.
    void missed(const std::string& module, std::uint64_t attempt) {
        std::chrono::milliseconds delay{};
        {
            std::lock_guard<std::mutex> lock(m_mu);
            Slot* s = slot(module);
            if (!s || s->attempt != attempt) return;
            s->inFlight = false;
            ++s->failures;
            s->retryArmed = true;
            delay = retryDelay(s->failures);
        }
        scheduleRefresh(module, delay);
    }

    // `retry`: the view stands, but the failure was transient; try again with backoff.
    void publish(const std::string& module, std::uint64_t attempt, ModuleView next,
                 bool retry = false) {
        std::chrono::milliseconds delay{};
        if (const ExposedModule* em = m_config->find(module)) next = withExposure(*em, std::move(next));
        {
            std::lock_guard<std::mutex> lock(m_mu);
            Slot* s = slot(module);
            if (!s || s->attempt != attempt) return;
            next.generation = s->view->generation + 1;   // counted here, not from a snapshot
            next.stale = false;
            s->view = std::make_shared<const ModuleView>(std::move(next));
            s->inFlight = false;
            s->failures = retry ? s->failures + 1 : 0;
            s->retryArmed = retry;
            delay = retryDelay(s->failures);
        }
        if (retry) scheduleRefresh(module, delay);
    }

    void scheduleRefresh(const std::string& module, std::chrono::milliseconds delay) {
        m_io.schedule(module, delay, [this, module] {
            m_io.post([this, module] { refresh(module); });
        });
    }

    const BridgeConfig* m_config;
    DiscoveryIo m_io;
    mutable std::mutex m_mu;
    std::map<std::string, Slot> m_slots;
};

} // namespace bridge
