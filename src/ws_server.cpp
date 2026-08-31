#include "ws_server.h"

#include <algorithm>
#include <cstdio>

namespace bridge {
namespace {

constexpr const char* kWsProtocol = "jsonrpc-bridge.v1";
constexpr const char* kJson = "application/json";

std::string hdr(struct lws* wsi, enum lws_token_indexes tok) {
    const int n = lws_hdr_total_length(wsi, tok);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n) + 1, '\0');
    if (lws_hdr_copy(wsi, out.data(), static_cast<int>(out.size()), tok) < 0) return {};
    out.resize(static_cast<std::size_t>(n));
    return out;
}

WsServer* serverOf(struct lws* wsi) {
    return static_cast<WsServer*>(lws_context_user(lws_get_context(wsi)));
}

} // namespace

// ---------------------------------------------------------------------------

bool WsServer::start(std::string* error) {
    static struct lws_protocols protocols[] = {
        {"http", &WsServer::callback, 0, 0, 0, nullptr, 0},
        {kWsProtocol, &WsServer::callback, 0, 65536, 0, nullptr, 0},
        LWS_PROTOCOL_LIST_TERM
    };

    struct lws_context_creation_info info;
    std::memset(&info, 0, sizeof(info));
    info.port = m_cfg.port;
    // Bind the loopback address EXPLICITLY. Leaving `iface` null binds
    // INADDR_ANY -- which is what openmetrics-module does, and means it is
    // listening on every interface rather than the loopback its config implies.
    info.iface = m_cfg.host.c_str();
    info.protocols = protocols;
    info.user = this;
    info.options = LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE;
    info.count_threads = 1;

    m_ctx = lws_create_context(&info);
    if (!m_ctx) {
        *error = "could not bind " + m_cfg.host + ":" + std::to_string(m_cfg.port);
        return false;
    }
    m_stop.store(false);
    m_thread = std::thread([this] { serviceLoop(); });
    return true;
}

void WsServer::serviceLoop() {
    while (!m_stop.load()) lws_service(m_ctx, 50);
}

void WsServer::stop() {
    if (!m_ctx) return;
    m_stop.store(true);
    lws_cancel_service(m_ctx);          // the one cross-thread-safe call
    if (m_thread.joinable()) m_thread.join();
    lws_context_destroy(m_ctx);         // only after the service thread is gone
    m_ctx = nullptr;
    std::lock_guard<std::mutex> lock(m_connMu);
    m_conns.clear();
    m_perPeer.clear();
}

void WsServer::send(const std::shared_ptr<Conn>& c, std::string frame) {
    if (!c || !m_ctx) return;
    struct lws* wsi = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_connMu);
        for (const auto& kv : m_conns)
            if (kv.second == c) { wsi = kv.first; break; }
    }
    if (!wsi) return;
    {
        std::lock_guard<std::mutex> lock(c->mu);
        if (static_cast<int>(c->outbound.size()) >= m_cfg.limits.maxQueuedFramesPerConnection) {
            // One upstream subscription feeds many clients, so the producer
            // cannot be back-pressured and dropping events would be a silent
            // gap. Closing the slow reader is the only honest option.
            c->wantClose.store(CloseReason::Policy);
        } else {
            c->outbound.push_back(std::move(frame));
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_wakeMu);
        m_pending.insert(wsi);
    }
    lws_cancel_service(m_ctx);
}

void WsServer::requestClose(const std::shared_ptr<Conn>& c, CloseReason why) {
    if (!c || !m_ctx) return;
    c->wantClose.store(why);
    struct lws* wsi = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_connMu);
        for (const auto& kv : m_conns)
            if (kv.second == c) { wsi = kv.first; break; }
    }
    if (!wsi) return;
    {
        std::lock_guard<std::mutex> lock(m_wakeMu);
        m_pending.insert(wsi);
    }
    lws_cancel_service(m_ctx);
}

std::shared_ptr<Conn> WsServer::attach(struct lws* wsi, bool isWebsocket) {
    auto c = std::make_shared<Conn>();
    c->id = m_nextConnId.fetch_add(1);
    c->isWebsocket = isWebsocket;
    c->authenticated = (m_cfg.authMode == AuthMode::None);
    char peer[64] = {0};
    lws_get_peer_simple(wsi, peer, sizeof(peer));
    c->peer = peer;
    std::lock_guard<std::mutex> lock(m_connMu);
    m_conns[wsi] = c;
    ++m_perPeer[c->peer];
    return c;
}

void WsServer::detach(struct lws* wsi) {
    std::shared_ptr<Conn> c;
    {
        std::lock_guard<std::mutex> lock(m_connMu);
        auto it = m_conns.find(wsi);
        if (it == m_conns.end()) return;
        c = it->second;
        auto p = m_perPeer.find(c->peer);
        if (p != m_perPeer.end() && --p->second <= 0) m_perPeer.erase(p);
        m_conns.erase(it);
    }
    {
        std::lock_guard<std::mutex> lock(m_wakeMu);
        m_pending.erase(wsi);
    }
    if (c && c->isWebsocket && m_hooks.onClosed) m_hooks.onClosed(c);
}

bool WsServer::originAllowed(struct lws* wsi) const {
    const std::string origin = hdr(wsi, WSI_TOKEN_ORIGIN);
    if (origin.empty()) return true;   // not a browser
    for (const auto& allowed : m_cfg.allowedOrigins)
        if (origin == allowed) return true;
    return false;
}

bool WsServer::hostAllowed(struct lws* wsi) const {
    const std::string host = hdr(wsi, WSI_TOKEN_HOST);
    if (host.empty()) return false;
    const std::string port = std::to_string(m_cfg.port);
    // Closes DNS rebinding: a name that resolves to 127.0.0.1 still arrives
    // with its own Host, and only the literals we bound are accepted.
    for (const char* h : {"127.0.0.1", "localhost", "[::1]"})
        if (host == std::string(h) + ":" + port || host == h) return true;
    return false;
}

bool WsServer::authorized(struct lws* wsi) const {
    if (m_cfg.authMode == AuthMode::None) return true;
    std::string auth = hdr(wsi, WSI_TOKEN_HTTP_AUTHORIZATION);
    const std::string prefix = "Bearer ";
    if (auth.rfind(prefix, 0) != 0) return false;
    return m_hooks.checkBearer && m_hooks.checkBearer(auth.substr(prefix.size()));
}

int WsServer::writeQueued(struct lws* wsi, const std::shared_ptr<Conn>& c) {
    std::string frame;
    std::size_t offset = 0;
    {
        std::lock_guard<std::mutex> lock(c->mu);
        if (!c->partial.empty()) {
            frame = c->partial;
            offset = c->partialOffset;
        } else if (!c->outbound.empty()) {
            frame = std::move(c->outbound.front());
            c->outbound.pop_front();
        } else {
            return 0;
        }
    }

    std::vector<unsigned char> buf(LWS_PRE + frame.size() - offset);
    std::memcpy(buf.data() + LWS_PRE, frame.data() + offset, frame.size() - offset);
    const int n = lws_write(wsi, buf.data() + LWS_PRE,
                            frame.size() - offset, LWS_WRITE_TEXT);
    if (n < 0) return -1;

    const std::size_t wrote = static_cast<std::size_t>(n);
    std::lock_guard<std::mutex> lock(c->mu);
    if (wrote < frame.size() - offset) {
        // Partial write. Resuming from the right offset is what keeps the frame
        // stream well-formed under a slow reader -- the exact condition the
        // queue bound exists for.
        c->partial = std::move(frame);
        c->partialOffset = offset + wrote;
    } else {
        c->partial.clear();
        c->partialOffset = 0;
    }
    if (!c->partial.empty() || !c->outbound.empty())
        lws_callback_on_writable(wsi);
    return 0;
}

int WsServer::callback(struct lws* wsi, enum lws_callback_reasons reason,
                       void* user, void* in, size_t len) {
    WsServer* self = serverOf(wsi);
    return self ? self->dispatch(wsi, reason, user, in, len) : 0;
}

int WsServer::dispatch(struct lws* wsi, enum lws_callback_reasons reason,
                       void* user, void* in, size_t len) {
    switch (reason) {

    // Cross-thread wake: the ONLY place work queued by another thread becomes
    // an lws call.
    case LWS_CALLBACK_EVENT_WAIT_CANCELLED: {
        std::set<struct lws*> pending;
        {
            std::lock_guard<std::mutex> lock(m_wakeMu);
            pending.swap(m_pending);
        }
        for (struct lws* w : pending) lws_callback_on_writable(w);
        return 0;
    }

    // ---- HTTP ----------------------------------------------------------
    case LWS_CALLBACK_HTTP: {
        const std::string path = in ? std::string(static_cast<const char*>(in), len)
                                    : std::string("/");
        if (!hostAllowed(wsi) || !originAllowed(wsi))
            return lws_return_http_status(wsi, HTTP_STATUS_FORBIDDEN, nullptr);
        if (!authorized(wsi))
            return lws_return_http_status(wsi, HTTP_STATUS_UNAUTHORIZED, nullptr);

        const std::string method = hdr(wsi, WSI_TOKEN_POST_URI).empty() ? "GET" : "POST";
        if (method == "GET") {
            int status = 200;
            std::string body = m_hooks.onGet ? m_hooks.onGet(path, &status) : std::string();
            if (body.empty() && status == 200) status = 404;
            // Queue the body and let HTTP_WRITEABLE emit headers AND body in one
            // place. Writing headers here as well produced two header blocks,
            // the second of which the client read as the response body.
            auto c = attach(wsi, false);
            {
                std::lock_guard<std::mutex> lock(c->mu);
                c->httpStatus = status;
                c->outbound.push_back(std::move(body));
            }
            lws_callback_on_writable(wsi);
            return 0;
        }

        // Reject anything that is not a JSON POST before reading a body. The
        // Content-Type requirement forces a CORS preflight for any cross-origin
        // caller, and no CORS headers are ever emitted, so the preflight fails.
        const std::string ctype = hdr(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE);
        if (ctype.rfind(kJson, 0) != 0)
            return lws_return_http_status(wsi, 415, nullptr);

        auto c = attach(wsi, false);
        c->httpRoute = path;
        return 0;
    }

    case LWS_CALLBACK_HTTP_BODY: {
        auto c = lookup(wsi);
        if (!c) return 0;
        if (c->httpBody.size() + len > static_cast<std::size_t>(m_cfg.limits.maxBodyBytes))
            return 1;   // over the cap: refuse before buffering more
        c->httpBody.append(static_cast<const char*>(in), len);
        return 0;
    }

    case LWS_CALLBACK_HTTP_BODY_COMPLETION: {
        auto c = lookup(wsi);
        if (!c) return 0;
        if (m_hooks.onBody) m_hooks.onBody(c, std::move(c->httpBody), false);
        c->httpBody.clear();
        return 0;   // the response arrives later, via send()
    }

    case LWS_CALLBACK_HTTP_WRITEABLE: {
        auto c = lookup(wsi);
        if (!c) return 0;
        std::string body;
        int status = 200;
        {
            std::lock_guard<std::mutex> lock(c->mu);
            if (c->outbound.empty()) return 0;
            body = std::move(c->outbound.front());
            c->outbound.pop_front();
            status = c->httpStatus;
        }
        unsigned char buf[LWS_PRE + 512], *p = buf + LWS_PRE, *end = buf + sizeof(buf);
        if (lws_add_http_common_headers(wsi, static_cast<unsigned>(status), kJson,
                                        body.size(), &p, end))
            return 1;
        if (lws_finalize_write_http_header(wsi, buf + LWS_PRE, &p, end)) return 1;
        std::vector<unsigned char> out(LWS_PRE + body.size());
        std::memcpy(out.data() + LWS_PRE, body.data(), body.size());
        if (lws_write(wsi, out.data() + LWS_PRE, body.size(), LWS_WRITE_HTTP_FINAL) < 0)
            return 1;
        return lws_http_transaction_completed(wsi) ? -1 : 0;
    }

    case LWS_CALLBACK_CLOSED_HTTP:
        detach(wsi);
        return 0;

    // ---- WebSocket -----------------------------------------------------
    case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION: {
        if (!hostAllowed(wsi) || !originAllowed(wsi)) return 1;
        if (!authorized(wsi) && m_cfg.authMode == AuthMode::Bearer) {
            // A browser cannot set headers on a WebSocket, so an unauthenticated
            // upgrade is allowed through and must authenticate with its first
            // frame instead. Non-browser clients use the header and are already
            // authenticated here.
            if (hdr(wsi, WSI_TOKEN_ORIGIN).empty()) return 1;
        }
        std::lock_guard<std::mutex> lock(m_connMu);
        if (static_cast<int>(m_conns.size()) >= m_cfg.limits.maxConnections) return 1;
        char peer[64] = {0};
        lws_get_peer_simple(wsi, peer, sizeof(peer));
        auto it = m_perPeer.find(peer);
        if (it != m_perPeer.end() && it->second >= m_cfg.limits.maxConnectionsPerPeer)
            return 1;
        return 0;
    }

    case LWS_CALLBACK_ESTABLISHED: {
        auto c = attach(wsi, true);
        c->authenticated = authorized(wsi);
        return 0;
    }

    case LWS_CALLBACK_RECEIVE: {
        auto c = lookup(wsi);
        if (!c) return 0;
        if (lws_frame_is_binary(wsi)) {
            requestClose(c, CloseReason::Unsupported);
            return 0;
        }
        c->httpBody.append(static_cast<const char*>(in), len);
        if (c->httpBody.size() > static_cast<std::size_t>(m_cfg.limits.maxFrameBytes)) {
            requestClose(c, CloseReason::TooBig);
            c->httpBody.clear();
            return 0;
        }
        if (!lws_is_final_fragment(wsi)) return 0;   // reassemble continuations
        std::string body;
        body.swap(c->httpBody);
        if (m_hooks.onBody) m_hooks.onBody(c, std::move(body), true);
        return 0;
    }

    case LWS_CALLBACK_SERVER_WRITEABLE: {
        auto c = lookup(wsi);
        if (!c) return 0;
        const CloseReason why = c->wantClose.load();
        if (why != CloseReason::None) {
            lws_close_reason(wsi,
                why == CloseReason::TooBig      ? LWS_CLOSE_STATUS_MESSAGE_TOO_LARGE :
                why == CloseReason::Unsupported ? LWS_CLOSE_STATUS_UNACCEPTABLE_OPCODE :
                                                  LWS_CLOSE_STATUS_POLICY_VIOLATION,
                nullptr, 0);
            return -1;
        }
        return writeQueued(wsi, c);
    }

    case LWS_CALLBACK_CLOSED:
        detach(wsi);
        return 0;

    default:
        return 0;
    }
}

} // namespace bridge
