#pragma once

// SHA-256 (FIPS 180-4) for the contract and interface digests. Pure and
// dependency-free; nothing here is a security boundary, only an identity.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace bridge {

class Sha256 {
public:
    Sha256() { reset(); }

    void reset() {
        m_state = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        m_bits = 0;
        m_used = 0;
    }

    void update(const unsigned char* data, std::size_t len) {
        m_bits += static_cast<std::uint64_t>(len) * 8;
        while (len > 0) {
            const std::size_t take = std::min(len, m_block.size() - m_used);
            for (std::size_t i = 0; i < take; ++i) m_block[m_used + i] = data[i];
            m_used += take;
            data += take;
            len -= take;
            if (m_used == m_block.size()) {
                compress();
                m_used = 0;
            }
        }
    }

    void update(const std::string& s) {
        update(reinterpret_cast<const unsigned char*>(s.data()), s.size());
    }

    std::array<std::uint8_t, 32> finish() {
        const std::uint64_t bits = m_bits;
        m_block[m_used++] = 0x80;
        if (m_used > 56) {
            while (m_used < 64) m_block[m_used++] = 0;
            compress();
            m_used = 0;
        }
        while (m_used < 56) m_block[m_used++] = 0;
        for (int i = 7; i >= 0; --i) m_block[m_used++] = static_cast<std::uint8_t>(bits >> (i * 8));
        compress();
        std::array<std::uint8_t, 32> out{};
        for (std::size_t i = 0; i < 8; ++i)
            for (std::size_t b = 0; b < 4; ++b)
                out[i * 4 + b] = static_cast<std::uint8_t>(m_state[i] >> (24 - b * 8));
        reset();
        return out;
    }

private:
    static std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void compress() {
        static const std::uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
        };
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (static_cast<std::uint32_t>(m_block[i * 4]) << 24) |
                   (static_cast<std::uint32_t>(m_block[i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(m_block[i * 4 + 2]) << 8) |
                   static_cast<std::uint32_t>(m_block[i * 4 + 3]);
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
        std::uint32_t e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) +
                                     ((e & f) ^ (~e & g)) + k[i] + w[i];
            const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) +
                                     ((a & b) ^ (a & c) ^ (b & c));
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        m_state[0] += a; m_state[1] += b; m_state[2] += c; m_state[3] += d;
        m_state[4] += e; m_state[5] += f; m_state[6] += g; m_state[7] += h;
    }

    std::array<std::uint32_t, 8> m_state{};
    std::array<std::uint8_t, 64> m_block{};
    std::uint64_t m_bits = 0;
    std::size_t m_used = 0;
};

// Lowercase hex SHA-256 of `bytes`.
inline std::string sha256Hex(const std::string& bytes) {
    Sha256 h;
    h.update(bytes);
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (std::uint8_t b : h.finish()) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0x0f]);
    }
    return out;
}

} // namespace bridge
