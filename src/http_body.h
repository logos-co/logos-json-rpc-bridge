#pragma once

// An HTTP response body handed to lws one slice per writeable callback, so each
// slice can re-arm lws's content timeout while a slow reader makes progress; and
// whether a request's body follows its headers.

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

// What lws parsed about a request's body, before reading any of it.
struct RequestFraming {
    bool transferEncoding = false;   // any Transfer-Encoding
    bool hasLength = false;          // a Content-Length header
    std::string length;              // its value; lws joins repeats with ", "
    bool bodyMethod = false;         // POST, PUT or PATCH, which lws reads 100 MiB of without a length
};

// Whether lws reads a body after the headers. An answer that leaves it unread closes
// the connection: lws's discard of it can swallow the next request.
inline bool bodyFollows(const RequestFraming& f) {
    if (f.transferEncoding) return true;
    if (f.hasLength) return f.length.empty() || f.length.find_first_not_of('0') != std::string::npos;
    return f.bodyMethod;
}

// The only POST body the bridge reads: a plain Content-Length, and no Transfer-Encoding.
inline bool usableLength(const RequestFraming& f) {
    return !f.transferEncoding && f.hasLength && !f.length.empty() &&
           f.length.find_first_not_of("0123456789") == std::string::npos;
}

} // namespace bridge
