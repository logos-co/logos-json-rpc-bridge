#pragma once

// One connection's subscriptions, keyed by the client-assigned id (its JSON
// dump). Pure and unlocked: Conn::subMu guards it.

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace bridge {

class SubscriptionTable {
public:
    enum class Claim { Fresh, Duplicate, Full };

    struct Entry {
        std::string module, event;
        std::uint64_t owner = 0;   // the bridge subscriber id that holds this client id
    };

    // Duplicate is checked before Full, so re-subscribing a live id stays idempotent at the limit.
    Claim claim(const std::string& key, const std::string& module, const std::string& event,
                std::uint64_t owner, int limit) {
        if (m_entries.count(key)) return Claim::Duplicate;
        if (static_cast<int>(m_entries.size()) >= limit) return Claim::Full;
        m_entries.emplace(key, Entry{module, event, owner});
        return Claim::Fresh;
    }

    // Frees `key` only while `owner` still holds it, so a stale release cannot free a newer claim.
    bool release(const std::string& key, std::uint64_t owner) {
        auto it = m_entries.find(key);
        if (it == m_entries.end() || it->second.owner != owner) return false;
        m_entries.erase(it);
        return true;
    }

    // Unsubscribe: frees `key` whoever holds it, and reports what it was.
    bool take(const std::string& key, Entry* out) {
        auto it = m_entries.find(key);
        if (it == m_entries.end()) return false;
        if (out) *out = it->second;
        m_entries.erase(it);
        return true;
    }

    std::size_t size() const { return m_entries.size(); }

private:
    std::map<std::string, Entry> m_entries;
};

} // namespace bridge
