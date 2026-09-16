#pragma once

// An HTTP response body handed to lws one slice per writeable callback, so each
// slice can re-arm lws's content timeout while a slow reader makes progress.

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace bridge {

class HttpBodyWriter {
public:
    static constexpr std::size_t kSliceBytes = 16 * 1024;

    explicit HttpBodyWriter(std::string body, std::size_t sliceBytes = kSliceBytes)
        : m_body(std::move(body)), m_slice(std::max<std::size_t>(sliceBytes, 1)) {}

    std::size_t size() const { return m_body.size(); }
    bool done() const { return m_done; }

    // The next slice, in order; `last` marks the final one. An empty body is one empty final slice.
    std::pair<const char*, std::size_t> next(bool* last) {
        const std::size_t n = std::min(m_slice, m_body.size() - m_offset);
        const char* at = m_body.data() + m_offset;
        m_offset += n;
        m_done = m_offset >= m_body.size();
        *last = m_done;
        return {at, n};
    }

private:
    std::string m_body;
    std::size_t m_slice;
    std::size_t m_offset = 0;
    bool m_done = false;
};

} // namespace bridge
