#pragma once

// Per-peer connection accounting for the transport. Pure and unlocked:
// WsServer guards it with m_connMu.

#include <map>
#include <string>

namespace bridge {

// One slot per connection, keyed by an opaque handle (the lws wsi). Keep-alive
// requests and a WebSocket upgrade reuse their connection's handle, and its slot.
class PeerSlots {
public:
    // Idempotent per handle: true only when this call took a new slot.
    bool acquire(const void* handle, const std::string& peer) {
        if (!m_owner.emplace(handle, peer).second) return false;
        ++m_count[peer];
        return true;
    }

    // Returns the slot to the peer recorded at acquire; unknown handles are ignored.
    bool release(const void* handle) {
        auto it = m_owner.find(handle);
        if (it == m_owner.end()) return false;
        auto c = m_count.find(it->second);
        if (c != m_count.end() && --c->second <= 0) m_count.erase(c);
        m_owner.erase(it);
        return true;
    }

    int count(const std::string& peer) const {
        auto it = m_count.find(peer);
        return it == m_count.end() ? 0 : it->second;
    }

    bool holds(const void* handle) const { return m_owner.count(handle) != 0; }

    bool admits(const std::string& peer, int limit) const { return count(peer) < limit; }

    void clear() {
        m_owner.clear();
        m_count.clear();
    }

private:
    std::map<const void*, std::string> m_owner;   // handle -> peer it was taken for
    std::map<std::string, int> m_count;           // peer -> live connections
};

} // namespace bridge
