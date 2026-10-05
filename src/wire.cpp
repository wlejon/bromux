#include "bromux/wire.h"

namespace bromux::wire {

void frame_message(std::string& out, uint16_t type, std::string_view payload) {
    const uint32_t len = uint32_t(2 + payload.size());
    char hdr[6];
    for (int i = 0; i < 4; ++i) hdr[i] = char(len >> (8 * i));
    hdr[4] = char(type);
    hdr[5] = char(type >> 8);
    out.append(hdr, 6);
    out.append(payload.data(), payload.size());
}

std::string make_message(uint16_t type, std::string_view payload) {
    std::string out;
    out.reserve(payload.size() + 6);
    frame_message(out, type, payload);
    return out;
}

void MessageSplitter::feed(const char* data, size_t n) {
    if (error_ || n == 0) return;
    // Compact consumed bytes before growing.
    if (head_ > 0 && (head_ >= buf_.size() / 2 || head_ > (1u << 20))) {
        buf_.erase(0, head_);
        head_ = 0;
    }
    buf_.append(data, n);
}

bool MessageSplitter::next(Message& m) {
    if (error_) return false;
    const size_t avail = buf_.size() - head_;
    if (avail < kLengthBytes) return false;
    const auto* p = reinterpret_cast<const unsigned char*>(buf_.data() + head_);
    const uint32_t len = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    if (len < 2 || len > kMaxMessage) {
        error_ = true;
        return false;
    }
    if (avail < kLengthBytes + len) return false;
    m.type = uint16_t(p[4] | (p[5] << 8));
    m.payload = std::string_view(buf_.data() + head_ + 6, len - 2);
    // The bytes stay put until the next feed() (which compacts), so the view
    // stays valid across further next() calls.
    head_ += kLengthBytes + len;
    return true;
}

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
