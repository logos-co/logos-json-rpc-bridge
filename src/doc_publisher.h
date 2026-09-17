#pragma once

// The served documents: discovery's views as the builders' input, and whole
// snapshots rebuilt off the socket thread whenever a view changes.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "bridge_config.h"
#include "discovery_model.h"
#include "doc_asyncapi.h"
#include "doc_common.h"
#include "doc_openapi.h"
#include "doc_openrpc.h"

namespace bridge {

// One view as the builders see it. Only a typed view carries an interface.
inline docs::ModuleDoc toModuleDoc(const ModuleView& view) {
    docs::ModuleDoc m;
    m.name = view.module;
    m.status = interfaceStatusName(view.status);
    m.exposedMethods = view.exposure.methods;
    m.exposedEvents = view.exposure.events;
    if (view.typed()) {
        m.interface = view.contract->interface;
        m.interfaceSha256 = view.interfaceSha256;
    } else if (view.resolved()) {
        for (const auto& lm : view.live.methods) m.liveMethods.push_back({lm.name, lm.params});
    }
    m.contractSha256 = view.contractSha256;
    m.interfaceError = view.interfaceError;
    return m;
}

// Everything the builders read: this bridge's endpoint, limits and version, and the views in config order.
inline docs::DocContext docContext(const BridgeConfig& cfg, const std::string& bridgeVersion,
                                   const std::vector<std::shared_ptr<const ModuleView>>& views) {
    docs::DocContext ctx;
    ctx.bridgeVersion = bridgeVersion;
    ctx.host = cfg.host;
    ctx.port = cfg.port;
    ctx.limits = cfg.limits;
    for (const auto& v : views)
        if (v) ctx.modules.push_back(toModuleDoc(*v));
    return ctx;
}

// Never throws; documents are UTF-8 clean, but a build must not take the pump down.
inline std::string compactJson(const nlohmann::json& doc) {
    return doc.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// A context's identity for rebuild decisions. An interface is stood for by its digest.
inline std::string docInputsKey(const docs::DocContext& ctx) {
    using nlohmann::json;
    json modules = json::array();
    for (const auto& m : ctx.modules) {
        json live = json::array();
        for (const auto& lm : m.liveMethods) live.push_back(json::array({lm.name, lm.params}));
        modules.push_back(json::array({m.name, m.status, m.interface.is_object(), m.interfaceSha256,
                                       m.contractSha256, m.interfaceError, m.exposedMethods,
                                       m.exposedEvents, std::move(live)}));
    }
    const Limits& l = ctx.limits;
    return compactJson(json::array({ctx.bridgeVersion, ctx.title, ctx.host, ctx.port, ctx.wsPath,
                                    ctx.rpcPath, ctx.subprotocol, l.maxFrameBytes,
                                    l.maxQueuedFramesPerConnection, std::move(modules)}));
}

// One immutable set of the three documents.
struct DocSet {
    std::uint64_t generation = 0;   // builds so far; the one start() makes is 1
    std::int64_t builtAt = 0;       // ms since the Unix epoch
    double buildMs = 0;             // how long the three builds took
    std::string inputsKey;
    nlohmann::json openrpc;         // rpc.discover's result
    std::string openapi;            // GET /openapi.json
    std::string asyncapi;           // GET /asyncapi.json
    std::size_t openrpcBytes = 0;   // compact

    nlohmann::json info() const {
        return nlohmann::json{
            {"generation", generation},
            {"built_at", builtAt},
            {"build_ms", buildMs},
            {"sizes", {{"openrpc", openrpcBytes},
                       {"openapi", openapi.size()},
                       {"asyncapi", asyncapi.size()}}},
        };
    }
};

inline std::shared_ptr<const DocSet> buildDocSet(const docs::DocContext& ctx, std::string inputsKey,
                                                 std::uint64_t generation) {
    const auto started = std::chrono::steady_clock::now();
    auto set = std::make_shared<DocSet>();
    set->generation = generation;
    set->inputsKey = std::move(inputsKey);
    set->openrpc = docs::buildOpenRpc(ctx);
    set->openrpcBytes = compactJson(set->openrpc).size();
    set->openapi = compactJson(docs::buildOpenApi(ctx));
    set->asyncapi = compactJson(docs::buildAsyncApi(ctx));
    set->buildMs = std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - started).count();
    set->builtAt = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch()).count();
    return set;
}

// Rebuilds on request: requests coalesce into one pending build, builds run one at
// a time on the pump, and readers only ever hold a whole snapshot.
class DocPublisher {
public:
    using Job = std::function<void()>;
    struct Io {
        std::function<docs::DocContext()> inputs;   // the current views; any thread
        std::function<bool(const std::string& key, std::chrono::milliseconds delay, Job job)> schedule;
        std::function<bool(Job)> post;
    };
    // The scheduler key; module names cannot contain a space.
    static constexpr const char* kTimer = "publish documents";
    static constexpr std::chrono::milliseconds kCoalesce{25};

    explicit DocPublisher(Io io) : m_io(std::move(io)) {}
    DocPublisher(const DocPublisher&) = delete;
    DocPublisher& operator=(const DocPublisher&) = delete;

    // The first snapshot, on the caller's thread. False when the build failed.
    bool buildNow() {
        rebuild();
        return current() != nullptr;
    }

    // Some view changed.
    void request() {
        {
            std::lock_guard<std::mutex> lock(m_mu);
            if (m_pending) return;
            m_pending = true;
        }
        const bool armed = m_io.schedule(kTimer, kCoalesce, [this] {
            if (!m_io.post([this] { rebuild(); })) clearPending();
        });
        if (!armed) clearPending();
    }

    // Null only before the first build.
    std::shared_ptr<const DocSet> current() const { return std::atomic_load(&m_current); }

    std::uint64_t failures() const { return m_failures.load(); }

    nlohmann::json info() const {
        const auto docs = current();
        nlohmann::json j = docs ? docs->info() : nlohmann::json::object();
        j["build_failures"] = failures();
        return j;
    }

private:
    void clearPending() {
        std::lock_guard<std::mutex> lock(m_mu);
        m_pending = false;
    }

    void rebuild() {
        std::lock_guard<std::mutex> building(m_buildMu);
        clearPending();   // a change from here on needs a build of its own
        try {
            const docs::DocContext ctx = m_io.inputs();
            std::string key = docInputsKey(ctx);
            const auto prev = current();
            if (prev && prev->inputsKey == key) return;
            std::shared_ptr<const DocSet> next =
                buildDocSet(ctx, std::move(key), (prev ? prev->generation : 0) + 1);
            std::atomic_store(&m_current, std::move(next));
        } catch (const std::exception&) {
            m_failures.fetch_add(1);   // the previous snapshot stays served
        }
    }

    Io m_io;
    std::mutex m_mu;
    bool m_pending = false;
    std::mutex m_buildMu;
    std::shared_ptr<const DocSet> m_current;
    std::atomic<std::uint64_t> m_failures{0};
};

} // namespace bridge
