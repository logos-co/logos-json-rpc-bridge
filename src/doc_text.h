#pragma once

// Text helpers for the self-description documents: UTF-8 checks, one-line
// summaries, and Doxygen code blocks turned into Markdown fences. Pure.

#include <cstddef>
#include <string>

namespace bridge {
namespace docs {

constexpr std::size_t kSummaryMaxBytes = 120;
constexpr const char* kEllipsis = "\xE2\x80\xA6";  // U+2026

// Length of the well-formed UTF-8 sequence starting at `i`, or 0 if there is none.
inline std::size_t utf8SeqLen(const std::string& s, std::size_t i) {
    const std::size_t n = s.size();
    if (i >= n) return 0;
    const auto at = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = at(i);
    if (c < 0x80) return 1;
    std::size_t len = 0;
    unsigned char lo = 0x80, hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF)                              len = 2;
    else if (c == 0xE0)                                      { len = 3; lo = 0xA0; }
    else if ((c >= 0xE1 && c <= 0xEC) || c == 0xEE || c == 0xEF) len = 3;
    else if (c == 0xED)                                      { len = 3; hi = 0x9F; }  // no surrogates
    else if (c == 0xF0)                                      { len = 4; lo = 0x90; }
    else if (c >= 0xF1 && c <= 0xF3)                         len = 4;
    else if (c == 0xF4)                                      { len = 4; hi = 0x8F; }  // <= U+10FFFF
    else return 0;
    if (i + len > n || at(i + 1) < lo || at(i + 1) > hi) return 0;
    for (std::size_t k = 2; k < len; ++k)
        if (at(i + k) < 0x80 || at(i + k) > 0xBF) return 0;
    return len;
}

inline bool utf8Valid(const std::string& s) {
    for (std::size_t i = 0; i < s.size();) {
        const std::size_t len = utf8SeqLen(s, i);
        if (len == 0) return false;
        i += len;
    }
    return true;
}

// Every invalid byte becomes U+FFFD, so a document can always be dumped.
inline std::string utf8Sanitize(const std::string& s) {
    if (utf8Valid(s)) return s;
    std::string out;
    out.reserve(s.size() + 8);
    for (std::size_t i = 0; i < s.size();) {
        const std::size_t len = utf8SeqLen(s, i);
        if (len == 0) { out += "\xEF\xBF\xBD"; ++i; continue; }
        out.append(s, i, len);
        i += len;
    }
    return out;
}

// Longest prefix of valid UTF-8 `s` that fits in `max` bytes without splitting a code point.
inline std::size_t utf8Floor(const std::string& s, std::size_t max) {
    std::size_t i = 0;
    while (i < s.size()) {
        std::size_t len = utf8SeqLen(s, i);
        if (len == 0) len = 1;
        if (i + len > max) break;
        i += len;
    }
    return i;
}

namespace detail {

inline bool isBlank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

inline bool isWordChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// "e.g." and "i.e." do not end a sentence.
inline bool endsAbbreviation(const std::string& line, std::size_t dot) {
    for (const char* abbr : {"e.g", "i.e"}) {
        if (dot < 3 || line.compare(dot - 3, 3, abbr) != 0) continue;
        if (dot == 3 || !isWordChar(line[dot - 4])) return true;
    }
    return false;
}

inline void trimRight(std::string* s) {
    while (!s->empty() && isBlank(s->back())) s->pop_back();
}

inline bool keywordAt(const std::string& s, std::size_t i, const char* kw, std::size_t len) {
    if (s.compare(i, len, kw) != 0) return false;
    if (i > 0 && isWordChar(s[i - 1])) return false;
    return i + len >= s.size() || !isWordChar(s[i + len]);
}

} // namespace detail

// The first sentence of the first non-blank line, at most 120 bytes; "…" marks a cut.
inline std::string summary(const std::string& desc) {
    const std::string text = utf8Sanitize(desc);
    std::size_t b = 0;
    while (b < text.size() && detail::isBlank(text[b])) ++b;
    if (b == text.size()) return {};
    std::size_t e = text.find('\n', b);
    std::string line = text.substr(b, e == std::string::npos ? std::string::npos : e - b);
    for (std::size_t i = 0; i + 1 < line.size(); ++i) {
        const char c = line[i];
        if ((c != '.' && c != '!' && c != '?') || (line[i + 1] != ' ' && line[i + 1] != '\t'))
            continue;
        if (c == '.' && detail::endsAbbreviation(line, i)) continue;
        line.resize(i + 1);
        break;
    }
    detail::trimRight(&line);
    if (line.size() <= kSummaryMaxBytes) return line;
    const std::string ellipsis = kEllipsis;
    line.resize(utf8Floor(line, kSummaryMaxBytes - ellipsis.size()));
    detail::trimRight(&line);
    return line + ellipsis;
}

// Doxygen `@code{.lang}` / `@code` ... `@endcode` become Markdown fences; all else is kept.
inline std::string description(const std::string& desc) {
    const std::string in = utf8Sanitize(desc);
    std::string out;
    out.reserve(in.size() + 16);
    bool inCode = false;
    const auto startLine = [&out] {
        std::size_t n = out.size();
        while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t')) --n;
        out.resize(n);
        if (!out.empty() && out.back() != '\n') out += '\n';
    };
    const auto skipSpaces = [&in](std::size_t j) {
        while (j < in.size() && (in[j] == ' ' || in[j] == '\t')) ++j;
        return j;
    };
    for (std::size_t i = 0; i < in.size();) {
        if (!inCode && in[i] == '@' && detail::keywordAt(in, i, "@code", 5)) {
            std::size_t j = i + 5;
            std::string lang;
            if (j < in.size() && in[j] == '{') {
                const std::size_t close = in.find('}', j);
                if (close != std::string::npos) {
                    lang = in.substr(j + 1, close - j - 1);
                    if (!lang.empty() && lang[0] == '.') lang.erase(0, 1);
                    for (char c : lang)
                        if (!detail::isWordChar(c) && c != '+' && c != '-') { lang.clear(); break; }
                    j = close + 1;
                }
            }
            startLine();
            out += "```" + lang + "\n";
            j = skipSpaces(j);
            if (j < in.size() && in[j] == '\n') ++j;
            inCode = true;
            i = j;
            continue;
        }
        if (inCode && in[i] == '@' && detail::keywordAt(in, i, "@endcode", 8)) {
            startLine();
            out += "```";
            std::size_t j = skipSpaces(i + 8);
            if (j < in.size() && in[j] != '\n') out += '\n';
            inCode = false;
            i = j;
            continue;
        }
        out += in[i++];
    }
    if (inCode) {
        startLine();
        out += "```";
    }
    return out;
}

} // namespace docs
} // namespace bridge
