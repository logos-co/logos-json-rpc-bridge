#pragma once

// When to replace a module's client: a QtRO node that lost its peer mid-packet never syncs again,
// and to the bridge that looks like "unavailable", as does a module that is not loaded.

#include <algorithm>
#include <chrono>

namespace bridge {

class RecyclePolicy {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds kFirstGap{2000};
    static constexpr std::chrono::milliseconds kMaxGap{60000};

    explicit RecyclePolicy(Clock::time_point clientCreated) : m_since(clientCreated) {}

    // A call through the current client found the module unavailable. True: replace it now.
    bool onUnavailable(Clock::time_point now) {
        if (now - m_since < m_gap) return false;
        m_since = now;
        m_gap = std::min(m_gap * 2, std::chrono::milliseconds(kMaxGap));
        return true;
    }

    // The provider answered through the current client.
    void onReached() { m_gap = kFirstGap; }

    std::chrono::milliseconds gap() const { return m_gap; }

private:
    Clock::time_point m_since;   // when the current client was created
    std::chrono::milliseconds m_gap{kFirstGap};
};

} // namespace bridge
