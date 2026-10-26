#pragma once

#include <cstdint>

// Explicit little-endian access, so the parsers do not depend on the host's byte order.
namespace mc3 {

inline uint16_t load_le16(const void* p) {
    const auto* b = static_cast<const uint8_t*>(p);
    return static_cast<uint16_t>(b[0] | (b[1] << 8));
}

inline uint32_t load_le32(const void* p) {
    const auto* b = static_cast<const uint8_t*>(p);
    return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
           (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
}

inline void store_le32(void* p, uint32_t value) {
    auto* b = static_cast<uint8_t*>(p);
    b[0] = static_cast<uint8_t>(value);
    b[1] = static_cast<uint8_t>(value >> 8);
    b[2] = static_cast<uint8_t>(value >> 16);
    b[3] = static_cast<uint8_t>(value >> 24);
}

}  // namespace mc3
