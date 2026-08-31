#pragma once

// The libwebsockets context, its single service thread, and the connection
// table. The ONLY file that touches lws.
//
// THE RULE, and it is absolute: lws is single-threaded (LWS_MAX_SMP == 1) and
// no lws function except lws_cancel_service is safe off the service thread.
// So every cross-thread action — a response, an event, a close — is recorded in
// per-connection state and then announced with lws_cancel_service(); the
// service thread performs the actual lws call when it wakes. A close requested
// from a pump thread is a flag, never an lws call.
//
// The service thread itself must never block: it parses frames, moves work to
// the pump, and writes. It never touches an LpClient.

#include <atomic>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <libwebsockets.h>

#include "bridge_config.h"

namespace bridge {

enum class CloseReason { None, Policy, TooBig, Unsupported, Shutdown };

// Per-connection state. Deliberately NOT stored in lws per-session user data:
// lws zero-fills that memory and never runs a constructor, so a std::mutex,
// std::deque or std::string living there would be undefined behaviour. A side
// table keyed by the wsi pointer costs one lookup and is correct.
struct Conn {
    std::uint64_t id = 0;
    bool isWebsocket = false;
    bool authenticated = false;

    std::mutex mu;                       // guards outbound + partial
    std::deque<std::string> outbound;    // complete frames, oldest first
    std::string partial;                 // remainder of a frame lws took only part of
    std::size_t partialOffset = 0;

    std::atomic<CloseReason> wantClose{CloseReason::None};
    std::atomic<int> inFlight{0};

    // Subscriptions this connection holds: client-assigned id -> (module,event).
    std::mutex subMu;
    std::map<std::string, std::pair<std::string, std::string>> subs;

    std::string httpBody;                // accumulating POST body
    std::string httpRoute;               // path, captured at header time
    int httpStatus = 200;                // status for the queued HTTP response
    std::string peer;
};

// Everything the transport needs from the rest of the bridge, injected so this
// file stays free of upstream types and the dispatcher stays testable without a
// socket.
struct ServerHooks {
    // Handle one JSON-RPC body. `respond` may be called from ANY thread and
    // fires once per response; `expected` is how many responses will come (0 =
    // no reply at all, i.e. all notifications).
    std::function<void(std::shared_ptr<Conn>, std::string body, bool isWebsocket)> onBody;
    // A websocket connection went away: drop its subscriptions.
    std::function<void(std::shared_ptr<Conn>)> onClosed;
    // GET routes, answered inline on the service thread (no upstream calls).
    std::function<std::string(const std::string& path, int* status)> onGet;
    // Bearer check; only consulted when auth.mode is bearer.
    std::function<bool(const std::string& presented)> checkBearer;
};

class WsServer {
public:
    WsServer(const BridgeConfig& cfg, ServerHooks hooks)
        : m_cfg(cfg), m_hooks(std::move(hooks)) {}

    ~WsServer() { stop(); }

    bool start(std::string* error);
    void stop();

    // Queue a frame for one connection. Safe from any thread.
    void send(const std::shared_ptr<Conn>& c, std::string frame);
    // Request a close. Safe from any thread; the service thread performs it.
    void requestClose(const std::shared_ptr<Conn>& c, CloseReason why);

    std::size_t connectionCount() const {
        std::lock_guard<std::mutex> lock(m_connMu);
        return m_conns.size();
    }

    int boundPort() const { return m_cfg.port; }

private:
    static int callback(struct lws* wsi, enum lws_callback_reasons reason,
                        void* user, void* in, size_t len);
    int dispatch(struct lws* wsi, enum lws_callback_reasons reason,
                 void* user, void* in, size_t len);

    std::shared_ptr<Conn> lookup(struct lws* wsi) const {
        std::lock_guard<std::mutex> lock(m_connMu);
        auto it = m_conns.find(wsi);
        return it == m_conns.end() ? nullptr : it->second;
    }
    std::shared_ptr<Conn> attach(struct lws* wsi, bool isWebsocket);
    void detach(struct lws* wsi);

    // Origin/Host gating. Loopback binding is NOT a boundary against a browser:
    // browsers do not apply same-origin policy to WebSockets and send no
    // preflight, so without this any page the operator visits can drive the
    // bridge. An empty allowed_origins refuses any request that carries an
    // Origin at all, which is the right default for a non-browser service.
    bool originAllowed(struct lws* wsi) const;
    bool hostAllowed(struct lws* wsi) const;
    bool authorized(struct lws* wsi) const;

    void serviceLoop();
    int writeQueued(struct lws* wsi, const std::shared_ptr<Conn>& c);

    BridgeConfig m_cfg;
    ServerHooks m_hooks;
    struct lws_context* m_ctx = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};

    mutable std::mutex m_connMu;
    std::map<struct lws*, std::shared_ptr<Conn>> m_conns;
    std::map<std::string, int> m_perPeer;
    std::atomic<std::uint64_t> m_nextConnId{1};

    // Connections with something to write or a pending close, so the service
    // thread knows who to poke after a cross-thread wake.
    std::mutex m_wakeMu;
    std::set<struct lws*> m_pending;
};

} // namespace bridge
