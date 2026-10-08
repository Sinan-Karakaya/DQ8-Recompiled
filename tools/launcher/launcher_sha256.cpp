#include "launcher_sha256.h"

#include <algorithm>
#include <cstring>

namespace dq8::launcher {
namespace {
constexpr uint32_t kRound[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

constexpr uint32_t rotr(uint32_t value, int bits) { return (value >> bits) | (value << (32 - bits)); }
} // namespace

Sha256::Sha256()
    : m_state{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
              0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u} {}

void Sha256::update(const void *data, size_t size) {
    const auto *bytes = static_cast<const uint8_t *>(data);
    m_length += size;
    if (m_buffered != 0u) {
        const size_t take = std::min(size, m_buffer.size() - m_buffered);
        std::memcpy(m_buffer.data() + m_buffered, bytes, take);
        m_buffered += take;
        bytes += take;
        size -= take;
        if (m_buffered < m_buffer.size())
            return;
        block(m_buffer.data());
        m_buffered = 0u;
    }
    for (; size >= 64u; bytes += 64, size -= 64u)
        block(bytes);
    std::memcpy(m_buffer.data(), bytes, size);
    m_buffered = size;
}

std::string Sha256::finishHex() {
    const uint64_t bits = m_length * 8u;
    const uint8_t one = 0x80u, zero = 0u;
    update(&one, 1u);
    while (m_buffered != 56u)
        update(&zero, 1u);
    uint8_t length[8];
    for (int i = 0; i < 8; ++i)
        length[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    update(length, sizeof(length));

    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex;
    for (const uint32_t word : m_state)
        for (int shift = 28; shift >= 0; shift -= 4)
            hex += kHex[(word >> shift) & 15u];
    return hex;
}

void Sha256::block(const uint8_t *data) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = uint32_t(data[i * 4]) << 24 | uint32_t(data[i * 4 + 1]) << 16 | uint32_t(data[i * 4 + 2]) << 8 |
               uint32_t(data[i * 4 + 3]);
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
    uint32_t e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kRound[i] + w[i];
        const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
    m_state[4] += e;
    m_state[5] += f;
    m_state[6] += g;
    m_state[7] += h;
}

} // namespace dq8::launcher
