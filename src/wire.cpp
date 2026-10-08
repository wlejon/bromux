#include "bromux/wire.h"

#include <cstring>

namespace bromux::wire {

uint64_t hash64(std::string_view data) noexcept {
    // A small multiply-xorshift hash over 8-byte words (wyhash-like mixing).
    constexpr uint64_t k0 = 0x9E3779B97F4A7C15ull;
    constexpr uint64_t k1 = 0xBF58476D1CE4E5B9ull;
    constexpr uint64_t k2 = 0x94D049BB133111EBull;
    uint64_t h = k0 ^ (uint64_t(data.size()) * k1);
    const char* p = data.data();
    size_t n = data.size();
    while (n >= 8) {
        uint64_t w;
        std::memcpy(&w, p, 8);
        h ^= w * k1;
        h = (h << 31) | (h >> 33);
        h *= k2;
        p += 8;
        n -= 8;
    }
    uint64_t tail = 0;
    for (size_t i = 0; i < n; ++i) tail |= uint64_t(uint8_t(p[i])) << (8 * i);
    h ^= tail * k0;
    h ^= h >> 30;
    h *= k1;
    h ^= h >> 27;
    h *= k2;
    h ^= h >> 31;
    return h;
}

}  // namespace bromux::wire
