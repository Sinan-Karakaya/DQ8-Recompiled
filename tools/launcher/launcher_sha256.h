// SHA-256 (FIPS 180-4), for checking a disc image against config/*/hashes.json
// without depending on a crypto library.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace dq8::launcher {

class Sha256 {
public:
    Sha256();
    void update(const void *data, size_t size);
    // Lower-case hex of the digest; the object is spent afterwards.
    std::string finishHex();

private:
    void block(const uint8_t *data);

    std::array<uint32_t, 8> m_state{};
    std::array<uint8_t, 64> m_buffer{};
    size_t m_buffered = 0;
    uint64_t m_length = 0;
};

} // namespace dq8::launcher
