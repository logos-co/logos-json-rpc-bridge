#include "json_rpc_bridge_impl.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "bridge_config.h"
#include "discovery.h"
#include "error_map.h"
#include "rpc_dispatcher.h"
#include "upstream.h"
#include "ws_server.h"

#include <logos_protocol.h>   // LOGOS_PROTOCOL_VERSION_*

// Generated at build time. Included only here so the impl header the generator
// parses stays free of codegen types.
#include "logos_sdk.h"

namespace {

// Subscription continuity needs the per-module status callback, which arrived
// at protocol 0.9.
// Below that the bridge still serves; it just cannot tell a client that its
// subscription was lost, so a provider restart resumes the stream with a gap.
#if defined(LOGOS_PROTOCOL_VERSION_MINOR) && \
    (LOGOS_PROTOCOL_VERSION_MAJOR > 0 ||     \
     (LOGOS_PROTOCOL_VERSION_MAJOR == 0 && LOGOS_PROTOCOL_VERSION_MINOR >= 9))
constexpr bool kHasSubscriptionContinuity = true;
#else
constexpr bool kHasSubscriptionContinuity = false;
#endif

} // namespace

using namespace bridge;

// ---------------------------------------------------------------------------
// BridgeCore
// ---------------------------------------------------------------------------

class BridgeCore {
public:
    BridgeCore(BridgeConfig cfg, std::string origin)
        : m_cfg(std::move(cfg)),
          m_clients(std::move(origin)),
          m_discovery(&m_clients, &m_cfg),
          m_hub(&m_clients,
                [this](const Delivery& d) { onEvent(d); },
                [this](std::uint64_t id, const std::string& m, const std::string& e) {
                    onSubscriptionLost(id, m, e);
                }) {}

    bool start(std::string* error);
    void shutdown();

    nlohmann::json info() const;
    const BridgeConfig& config() const { return m_cfg; }

private:
    void onBody(std::shared_ptr<Conn> conn, std::string body, bool isWebsocket);
    void handleOne(const std::shared_ptr<Conn>& conn, const nlohmann::json& raw,
                   const std::shared_ptr<class Batch>& batch, std::size_t slot);
    void onEvent(const Delivery& d);
    void onSubscriptionLost(std::uint64_t subscriberId, const std::string& module,
                            const std::string& event);
    std::string onGet(const std::string& path, int* status);

    BridgeConfig m_cfg;
    ClientRegistry m_clients;
    Discovery m_discovery;
    CallPump m_pump;
    SubscriptionHub m_hub;
    std::unique_ptr<WsServer> m_server;

    std::atomic<bool> m_draining{false};
    std::chrono::steady_clock::time_point m_startedAt;

    // subscriberId -> the connection and the client's own id for it.
    struct Subscriber {
        std::weak_ptr<Conn> conn;
        nlohmann::json clientId;
        std::string module, event;
    };
    mutable std::mutex m_subMu;
    std::map<std::uint64_t, Subscriber> m_subscribers;
    std::atomic<std::uint64_t> m_nextSubscriberId{1};
};

// Collects a batch's responses out of order and flushes once they are all in.
// HTTP needs this: a batch is one response body, and the upstream calls that
// fill it complete on unrelated threads at unrelated times.
class Batch : public std::enable_shared_from_this<Batch> {
public:
    // restShape: emit the bare {"result":...} / {"error":...} the REST
    // projection promises, rather than the full JSON-RPC envelope. The
    // dispatcher is shared, so only the final serialisation differs.
    Batch(std::shared_ptr<Conn> conn, WsServer* server, std::size_t expected, bool single,
          bool restShape = false)
        : m_conn(std::move(conn)), m_server(server), m_slots(expected),
          m_remaining(expected), m_single(single), m_restShape(restShape) {}

    void fill(std::size_t slot, nlohmann::json response) {
        {
            std::lock_guard<std::mutex> lock(m_mu);
            if (slot < m_slots.size()) m_slots[slot] = std::move(response);
        }
        if (m_remaining.fetch_sub(1) == 1) flush();
    }

    // A notification produces no response but still consumes its slot.
    void skip(std::size_t slot) {
        {
            std::lock_guard<std::mutex> lock(m_mu);
            if (slot < m_slots.size()) m_slots[slot] = nlohmann::json();
        }
        if (m_remaining.fetch_sub(1) == 1) flush();
    }

private:
    void flush() {
        std::vector<nlohmann::json> slots;
        {
            std::lock_guard<std::mutex> lock(m_mu);
            slots = std::move(m_slots);
        }
        nlohmann::json out = nlohmann::json::array();
        for (auto& s : slots)
            if (!s.is_null()) out.push_back(std::move(s));
        if (out.empty()) {
            // Every request was a notification: nothing to answer with.
            m_server->send(m_conn, "");
            return;
        }
        if (m_restShape) {
            nlohmann::json r = nlohmann::json::object();
            if (out[0].contains("error"))       r["error"]  = out[0]["error"];
            else if (out[0].contains("result")) r["result"] = out[0]["result"];
            m_server->send(m_conn, r.dump());
            return;
        }
        m_server->send(m_conn, (m_single && out.size() == 1) ? out[0].dump() : out.dump());
    }

    std::shared_ptr<Conn> m_conn;
    WsServer* m_server;
    std::mutex m_mu;
    std::vector<nlohmann::json> m_slots;
    std::atomic<std::size_t> m_remaining;
    bool m_single;
    bool m_restShape;
};

// ---------------------------------------------------------------------------

bool BridgeCore::start(std::string* error) {
    m_pump.start(2);

    ServerHooks hooks;
    hooks.onBody = [this](std::shared_ptr<Conn> c, std::string b, bool ws) {
        onBody(std::move(c), std::move(b), ws);
    };
    hooks.onClosed = [this](std::shared_ptr<Conn> c) {
        std::vector<std::uint64_t> mine;
        {
            std::lock_guard<std::mutex> lock(m_subMu);
            for (auto it = m_subscribers.begin(); it != m_subscribers.end();) {
                if (it->second.conn.lock() == c) { mine.push_back(it->first); it = m_subscribers.erase(it); }
                else ++it;
            }
        }
        for (std::uint64_t id : mine) m_hub.removeSubscriberEverywhere(id);
    };
    hooks.onGet = [this](const std::string& p, int* s) { return onGet(p, s); };
    hooks.checkBearer = [](const std::string&) { return false; };

    m_server = std::make_unique<WsServer>(m_cfg, std::move(hooks));
    if (!m_server->start(error)) { m_pump.stop(); return false; }

    m_startedAt = std::chrono::steady_clock::now();

    // Warm every exposed target off the request path. This is also what
    // populates the interface cache, because the call that fetches it is the
    // same call that triggers LpClient's lazy (and blocking) create.
    for (const auto& em : m_cfg.modules) {
        const std::string name = em.name;
        m_pump.submit([this, name] { m_discovery.refresh(name); });
    }
    return true;
}

// Ordered so that the thing which STOPS callbacks comes before the thing they
// touch. Synchronous on purpose: every step here is bounded in tens of
// milliseconds, and doing it inline avoids a detached thread that could outlive
// the module and call back into freed state — the failure mode the host's
// unload grace period makes easy to hit.
void BridgeCore::shutdown() {
    m_draining.store(true);
    m_hub.clear();        // no further event deliveries
    m_clients.close();    // no further completions
    m_pump.stop();        // no further submissions
    if (m_server) { m_server->stop(); m_server.reset(); }  // joins, then destroys the ctx
    std::lock_guard<std::mutex> lock(m_subMu);
    m_subscribers.clear();
}

void BridgeCore::onEvent(const Delivery& d) {
    std::shared_ptr<Conn> conn;
    nlohmann::json clientId;
    {
        std::lock_guard<std::mutex> lock(m_subMu);
        auto it = m_subscribers.find(d.subscriberId);
        if (it == m_subscribers.end()) return;
        conn = it->second.conn.lock();
        clientId = it->second.clientId;
    }
    if (!conn) return;
    // Only build a frame and enqueue: this runs on the publishing side's
    // thread, and the payload buffer is borrowed for this call only.
    m_server->send(conn, makeNotification(op::kEvent, nlohmann::json{
        {"subscription", clientId},
        {"module", d.module},
        {"event", d.event},
        {"data", d.data},
        {"generation", d.generation},
        {"ts", std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count()},
    }).dump());
}

void BridgeCore::onSubscriptionLost(std::uint64_t subscriberId, const std::string& module,
                                    const std::string& event) {
    std::shared_ptr<Conn> conn;
    nlohmann::json clientId;
    {
        std::lock_guard<std::mutex> lock(m_subMu);
        auto it = m_subscribers.find(subscriberId);
        if (it == m_subscribers.end()) return;
        conn = it->second.conn.lock();
        clientId = it->second.clientId;
        m_subscribers.erase(it);
    }
    if (!conn) return;
    // Terminate rather than silently resume: a re-established upstream
    // subscription is a NEW one, and the events in between are unrecoverable.
    // The client decides whether to re-subscribe and refetch state.
    m_server->send(conn, makeNotification(op::kTerminated, nlohmann::json{
        {"subscription", clientId},
        {"module", module},
        {"event", event},
        {"reason", "provider_unavailable"},
    }).dump());
    if (conn) {
        std::lock_guard<std::mutex> lock(conn->subMu);
        for (auto it = conn->subs.begin(); it != conn->subs.end(); ++it) {
            if (it->second.first == module && it->second.second == event) {
                conn->subs.erase(it);
                break;
            }
        }
    }
}

std::string BridgeCore::onGet(const std::string& path, int* status) {
    *status = 200;
    if (path == "/healthz") {
        return nlohmann::json{
            {"status", m_draining.load() ? "draining" : "ok"},
            {"uptime_seconds", std::chrono::duration_cast<std::chrono::seconds>(
                                   std::chrono::steady_clock::now() - m_startedAt).count()},
            {"protocol", "json-rpc-2.0"},
        }.dump();
    }
    if (path == "/modules") return m_discovery.listModules().dump();
    const std::string prefix = "/modules/";
    if (path.rfind(prefix, 0) == 0) {
        const std::string name = path.substr(prefix.size());
        nlohmann::json d = m_discovery.describe(name);
        if (d.is_null()) { *status = 404; return notFound().toJson().dump(); }
        return d.dump();
    }
    *status = 404;
    return notFound().toJson().dump();
}

void BridgeCore::onBody(std::shared_ptr<Conn> conn, std::string body, bool /*isWebsocket*/) {
    if (m_draining.load()) {
        m_server->send(conn, makeError(nlohmann::json(),
            {kShuttingDown, LogosErrorCode::NotReady, "shutting down"}).dump());
        return;
    }
    // REST projection: POST /modules/{module}/{method}. The same two-field
    // addressing as rpc.call, expressed in the path, so the body is the inner
    // params directly. Synthesised into a normal request and dispatched through
    // the shared path — the two routes agree by construction rather than by
    // being kept in sync.
    const std::string prefix = "/modules/";
    if (conn->httpRoute.rfind(prefix, 0) == 0) {
        const std::string rest = conn->httpRoute.substr(prefix.size());
        const std::size_t slash = rest.find('/');
        if (slash == std::string::npos || slash == 0 || slash + 1 >= rest.size()) {
            m_server->send(conn, makeError(nlohmann::json(), notFound()).dump());
            return;
        }
        nlohmann::json inner = nlohmann::json::object();
        if (!body.empty()) {
            auto parsed = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
            if (parsed.is_discarded()) {
                m_server->send(conn, makeError(nlohmann::json(),
                    {kParseError, LogosErrorCode::InvalidParams, "parse error"}).dump());
                return;
            }
            inner = std::move(parsed);
        }
        nlohmann::json req{
            {"jsonrpc", "2.0"}, {"id", 1}, {"method", op::kCall},
            {"params", {{"module", rest.substr(0, slash)},
                        {"method", rest.substr(slash + 1)},
                        {"params", std::move(inner)}}}};
        auto batch = std::make_shared<Batch>(conn, m_server.get(), 1, true, /*restShape=*/true);
        handleOne(conn, req, batch, 0);
        return;
    }

    std::vector<nlohmann::json> requests;
    bool single = true;
    nlohmann::json protoErr;
    if (!splitBody(body, &requests, &single, &protoErr)) {
        m_server->send(conn, protoErr.dump());
        return;
    }
    if (static_cast<int>(requests.size()) > m_cfg.limits.maxInFlightPerConnection) {
        m_server->send(conn, makeError(nlohmann::json(),
            {kOverloaded, LogosErrorCode::NotReady, "batch too large"}).dump());
        return;
    }
    auto batch = std::make_shared<Batch>(conn, m_server.get(), requests.size(), single);
    for (std::size_t i = 0; i < requests.size(); ++i)
        handleOne(conn, requests[i], batch, i);
}

void BridgeCore::handleOne(const std::shared_ptr<Conn>& conn, const nlohmann::json& raw,
                           const std::shared_ptr<Batch>& batch, std::size_t slot) {
    RpcRequest req;
    MappedError err;
    if (!parseRequest(raw, &req, &err)) { batch->fill(slot, makeError(nlohmann::json(), err)); return; }

    // No notification concept exists upstream -- every call gets exactly one
    // reply -- so a fire-and-forget module call would silently discard errors.
    if (req.isNotification && req.method == op::kCall) {
        batch->fill(slot, makeError(nlohmann::json(),
            {kInvalidRequest, LogosErrorCode::InvalidParams,
             "notifications are not accepted for rpc.call: every module call has a reply"}));
        return;
    }
    const nlohmann::json id = req.id;

    if (req.method == op::kPing)        { batch->fill(slot, makeResult(id, "pong")); return; }
    if (req.method == op::kListModules) { batch->fill(slot, makeResult(id, m_discovery.listModules())); return; }
    if (req.method == op::kSchema) {
        SubscribeTarget t;
        if (!req.params.is_object() || !req.params.contains("module") ||
            !req.params["module"].is_string()) {
            batch->fill(slot, makeError(id, {kInvalidParams, LogosErrorCode::InvalidParams,
                                             "rpc.schema requires a string \"module\""}));
            return;
        }
        nlohmann::json d = m_discovery.describe(req.params["module"].get<std::string>());
        batch->fill(slot, d.is_null() ? makeError(id, notFound()) : makeResult(id, d));
        return;
    }

    if (req.method == op::kCall) {
        CallTarget t;
        if (!parseCallTarget(req.params, &t, &err)) { batch->fill(slot, makeError(id, err)); return; }
        if (!m_discovery.methodPermitted(t.module, t.method)) {
            batch->fill(slot, makeError(id, notFound()));
            return;
        }
        nlohmann::json args = t.params;
        if (args.is_object()) {
            std::string badPath;
            nlohmann::json positional;
            if (!m_discovery.toPositional(t.module, t.method, args, &positional, &badPath)) {
                batch->fill(slot, makeErrorRaw(id, invalidParamsJson("schema-mismatch", badPath)));
                return;
            }
            args = std::move(positional);
        }
        const int timeout = m_cfg.limits.callTimeoutMs;
        auto client = m_clients.get(t.module);
        if (!client) { batch->fill(slot, makeError(id, notFound())); return; }
        const bool queued = m_pump.submit([client, t, args, id, batch, slot, timeout] {
            client->invokeAsyncResult(t.method, args,
                [batch, slot, id](nlohmann::json value, const logos::CallError& e) {
                    if (!e.ok()) {
                        batch->fill(slot, makeError(id, mapCallError(e.code, e.message)));
                        return;
                    }
                    // An application-level failure is a SUCCESSFUL call: the
                    // payload goes in `result` untouched, never promoted to an
                    // error. See error_map.h.
                    batch->fill(slot, makeResult(id, std::move(value)));
                }, timeout);
        });
        if (!queued)
            batch->fill(slot, makeError(id, {kShuttingDown, LogosErrorCode::NotReady, "shutting down"}));
        return;
    }

    if (req.method == op::kSubscribe) {
        SubscribeTarget t;
        if (!parseSubscribeTarget(req.params, &t, &err, /*requireEvent=*/true)) {
            batch->fill(slot, makeError(id, err)); return;
        }
        if (!m_discovery.eventPermitted(t.module, t.event)) {
            batch->fill(slot, makeError(id, notFound()));
            return;
        }
        const std::string key = t.subscription.dump();
        {
            std::lock_guard<std::mutex> lock(conn->subMu);
            if (static_cast<int>(conn->subs.size()) >= m_cfg.limits.maxSubscriptionsPerConnection) {
                batch->fill(slot, makeError(id, {kOverloaded, LogosErrorCode::NotReady,
                                                 "too many subscriptions"}));
                return;
            }
            // Re-subscribing the same id is idempotent and must not create a
            // second upstream subscriber or double-deliver.
            auto existing = conn->subs.find(key);
            if (existing != conn->subs.end()) {
                batch->fill(slot, makeResult(id, nlohmann::json{
                    {"subscription", t.subscription}, {"operation", "subscribe"},
                    {"module", t.module}, {"event", t.event}, {"state", "active"}}));
                return;
            }
            conn->subs.emplace(key, std::make_pair(t.module, t.event));
        }
        const std::uint64_t sid = m_nextSubscriberId.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock(m_subMu);
            m_subscribers[sid] = Subscriber{conn, t.subscription, t.module, t.event};
        }
        const bool queued = m_pump.submit([this, t, sid, id, batch, slot] {
            const bool ok = m_hub.add(t.module, t.event, sid);
            if (!ok) {
                std::lock_guard<std::mutex> lock(m_subMu);
                m_subscribers.erase(sid);
                batch->fill(slot, makeError(id, notFound()));
                return;
            }
            // "registered", not "active": the upstream subscription is held and
            // arms when the provider appears, so claiming it is live here would
            // be a promise this layer cannot keep.
            batch->fill(slot, makeResult(id, nlohmann::json{
                {"subscription", t.subscription}, {"operation", "subscribe"},
                {"module", t.module}, {"event", t.event}, {"state", "registered"}}));
        });
        if (!queued)
            batch->fill(slot, makeError(id, {kShuttingDown, LogosErrorCode::NotReady, "shutting down"}));
        return;
    }

    if (req.method == op::kUnsubscribe) {
        SubscribeTarget t;
        if (!parseSubscribeTarget(req.params, &t, &err, /*requireEvent=*/false)) {
            batch->fill(slot, makeError(id, err)); return;
        }
        const std::string key = t.subscription.dump();
        std::string module, event;
        {
            std::lock_guard<std::mutex> lock(conn->subMu);
            auto it = conn->subs.find(key);
            if (it != conn->subs.end()) { module = it->second.first; event = it->second.second; conn->subs.erase(it); }
        }
        std::uint64_t sid = 0;
        {
            std::lock_guard<std::mutex> lock(m_subMu);
            for (auto it = m_subscribers.begin(); it != m_subscribers.end(); ++it) {
                if (it->second.conn.lock() == conn && it->second.clientId == t.subscription) {
                    sid = it->first; m_subscribers.erase(it); break;
                }
            }
        }
        if (sid) m_hub.remove(module, event, sid);
        // Idempotent: an unknown id is acked, not an error.
        batch->fill(slot, makeResult(id, nlohmann::json{
            {"subscription", t.subscription}, {"operation", "unsubscribe"}}));
        return;
    }

    if (req.method == op::kCancel) {
        // Bridge-local bookkeeping only: the ABI has no cancel primitive, so
        // this stops the RESPONSE, not the work. Documented as such.
        batch->fill(slot, makeResult(id, nlohmann::json{{"cancelled", false},
                                                        {"reason", "not_supported_upstream"}}));
        return;
    }

    batch->fill(slot, makeError(id, notFound()));
}

nlohmann::json BridgeCore::info() const {
    nlohmann::json mods = nlohmann::json::array();
    for (const auto& em : m_cfg.modules) {
        mods.push_back(nlohmann::json{
            {"name", em.name},
            {"methods", em.methods.describe()},
            {"events", em.events.describe()},
        });
    }
    std::size_t subs = 0;
    { std::lock_guard<std::mutex> lock(m_subMu); subs = m_subscribers.size(); }
    return nlohmann::json{
        {"running", m_server != nullptr},
        {"http", "http://" + m_cfg.host + ":" + std::to_string(m_cfg.port)},
        {"ws", "ws://" + m_cfg.host + ":" + std::to_string(m_cfg.port) + "/ws"},
        {"auth", m_cfg.authMode == AuthMode::Bearer ? "bearer" : "none"},
        {"modules", std::move(mods)},
        {"connections", m_server ? m_server->connectionCount() : 0},
        {"subscriptions", subs},
        {"upstream_subscriptions", m_hub.upstreamCount()},
        {"protocol_version", LOGOS_PROTOCOL_VERSION_STRING},
        {"subscription_continuity", kHasSubscriptionContinuity},
    };
}

// ---------------------------------------------------------------------------
// JsonRpcBridgeImpl
// ---------------------------------------------------------------------------

JsonRpcBridgeImpl::JsonRpcBridgeImpl() = default;

JsonRpcBridgeImpl::~JsonRpcBridgeImpl() {
    if (m_core) m_core->shutdown();
}

StdLogosResult JsonRpcBridgeImpl::start(const std::string& configJson) {
    if (m_core) return {false, {}, "already running; call stop() first"};

    ConfigParseResult parsed = parseBridgeConfig(configJson, moduleName());
    if (!parsed.ok) return {false, {}, parsed.error};

    auto core = std::make_shared<BridgeCore>(std::move(parsed.config), moduleName());
    std::string error;
    if (!core->start(&error)) return {false, {}, error};

    m_core = std::move(core);
    return {true, m_core->info()};
}

StdLogosResult JsonRpcBridgeImpl::stop() {
    if (!m_core) return {false, {}, "not running"};
    m_core->shutdown();
    m_core.reset();
    return {true, nlohmann::json{{"stopped", true}}};
}

std::string JsonRpcBridgeImpl::getInfo() {
    if (!m_core) {
        return nlohmann::json{
            {"running", false},
            {"protocol_version", LOGOS_PROTOCOL_VERSION_STRING},
            {"subscription_continuity", kHasSubscriptionContinuity},
        }.dump();
    }
    return m_core->info().dump();
}

LogosShutdown JsonRpcBridgeImpl::aboutToUnload() {
    // SYNCHRONOUS, deliberately. Every step of shutdown() is bounded in tens of
    // milliseconds (the lws service loop wakes on a 50 ms timeout and the pump
    // drains rather than waiting), so it fits inside the host's grace period
    // with room to spare. Returning Asynchronous would mean a detached thread
    // that can outlive the plugin object and call unloadFinished() through a
    // freed pointer — a real hazard here, and one that buys nothing.
    if (m_core) { m_core->shutdown(); m_core.reset(); }
    return LogosShutdown::Synchronous;
}
