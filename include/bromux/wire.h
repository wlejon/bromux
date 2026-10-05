#pragma once
// Wire primitives: the byte-level encoding every bromux message uses.
//
// A message on the stream is
//     u32 length (little endian)  -- bytes that follow: the type and the body
//     u16 type   (little endian)  -- MsgType (protocol.h)
//     body                        -- type-specific, built from the primitives below
// Integers are little endian when fixed-width; `varint` is unsigned LEB128
// (at most 10 bytes), `svarint` is zigzag + LEB128; `str` / `bytes` are a
// varint length followed by that many bytes. Readers never trust a length:
// every read is bounds-checked and a malformed body marks the Reader failed
// instead of reading past it. docs/protocol.md has the full catalogue.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace bromux::wire {

// Largest message either side accepts (type + body). A frame with a full
// screen of styled wide text stays far below it.
inline constexpr size_t kMaxMessage = 32u << 20;
inline constexpr size_t kLengthBytes = 4;

class Writer {
public:
    Writer() = default;
    explicit Writer(std::string& out) : ext_(&out) {}

    void u8(uint8_t v) { buf().push_back(char(v)); }
    void u16(uint16_t v) {
        char b[2] = {char(v), char(v >> 8)};
        buf().append(b, 2);
    }
    void u32(uint32_t v) {
        char b[4];
        for (int i = 0; i < 4; ++i) b[i] = char(v >> (8 * i));
        buf().append(b, 4);
    }
    void u64(uint64_t v) {
        char b[8];
        for (int i = 0; i < 8; ++i) b[i] = char(v >> (8 * i));
        buf().append(b, 8);
    }
    void varint(uint64_t v) {
        char b[10];
        int n = 0;
        while (v >= 0x80) {
            b[n++] = char(uint8_t(v) | 0x80);
            v >>= 7;
        }
        b[n++] = char(v);
        buf().append(b, size_t(n));
    }
    void svarint(int64_t v) { varint((uint64_t(v) << 1) ^ uint64_t(v >> 63)); }
    void boolean(bool v) { u8(v ? 1 : 0); }
    void str(std::string_view s) {
        varint(s.size());
        buf().append(s.data(), s.size());
    }
    void raw(std::string_view s) { buf().append(s.data(), s.size()); }
    void strings(const std::vector<std::string>& v) {
        varint(v.size());
        for (const std::string& s : v) str(s);
    }
    void pairs(const std::vector<std::pair<std::string, std::string>>& v) {
        varint(v.size());
        for (const auto& [k, val] : v) {
            str(k);
            str(val);
        }
    }

    [[nodiscard]] std::string& data() { return buf(); }
    [[nodiscard]] size_t size() const { return ext_ ? ext_->size() : own_.size(); }
    [[nodiscard]] std::string take() { return std::move(buf()); }

private:
    std::string& buf() { return ext_ ? *ext_ : own_; }
    std::string own_;
    std::string* ext_{nullptr};
};

class Reader {
public:
    Reader() = default;
    explicit Reader(std::string_view data) : d_(data) {}

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] bool at_end() const noexcept { return pos_ >= d_.size(); }
    [[nodiscard]] size_t remaining() const noexcept { return ok_ ? d_.size() - pos_ : 0; }
    // Marks the reader failed (a semantic check of the caller failed).
    void fail() noexcept { ok_ = false; }
    // True when every byte was consumed and nothing failed.
    [[nodiscard]] bool done() const noexcept { return ok_ && pos_ == d_.size(); }

    uint8_t u8() {
        if (!need(1)) return 0;
        return uint8_t(d_[pos_++]);
    }
    uint16_t u16() {
        if (!need(2)) return 0;
        uint16_t v = uint16_t(uint8_t(d_[pos_]) | (uint8_t(d_[pos_ + 1]) << 8));
        pos_ += 2;
        return v;
    }
    uint32_t u32() {
        if (!need(4)) return 0;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= uint32_t(uint8_t(d_[pos_ + size_t(i)])) << (8 * i);
        pos_ += 4;
        return v;
    }
    uint64_t u64() {
        if (!need(8)) return 0;
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= uint64_t(uint8_t(d_[pos_ + size_t(i)])) << (8 * i);
        pos_ += 8;
        return v;
    }
    uint64_t varint() {
        uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (!need(1)) return 0;
            uint8_t b = uint8_t(d_[pos_++]);
            if (shift == 63 && b > 1) break;  // overflow
            v |= uint64_t(b & 0x7F) << shift;
            if (!(b & 0x80)) return v;
        }
        ok_ = false;
        return 0;
    }
    // A varint that must fit `limit` (inclusive); larger marks the reader failed.
    uint64_t varint_max(uint64_t limit) {
        uint64_t v = varint();
        if (v > limit) {
            ok_ = false;
            return 0;
        }
        return v;
    }
    int64_t svarint() {
        uint64_t v = varint();
        return int64_t(v >> 1) ^ -int64_t(v & 1);
    }
    bool boolean() {
        uint8_t v = u8();
        if (v > 1) ok_ = false;
        return v == 1;
    }
    std::string_view str_view() {
        uint64_t n = varint();
        if (!ok_ || !need(n)) return {};
        std::string_view s = d_.substr(pos_, size_t(n));
        pos_ += size_t(n);
        return s;
    }
    std::string str() { return std::string(str_view()); }
    std::string_view raw(size_t n) {
        if (!need(n)) return {};
        std::string_view s = d_.substr(pos_, n);
        pos_ += n;
        return s;
    }
    // A count of elements that each take at least `min_bytes`: rejected when
    // the remaining input could not possibly hold them (no huge reserve()).
    size_t count(size_t min_bytes = 1) {
        uint64_t n = varint();
        if (!ok_) return 0;
        if (min_bytes && n > remaining() / min_bytes) {
            ok_ = false;
            return 0;
        }
        return size_t(n);
    }
    std::vector<std::string> strings() {
        std::vector<std::string> v(count());
        for (std::string& s : v) s = str();
        if (!ok_) v.clear();
        return v;
    }
    std::vector<std::pair<std::string, std::string>> pairs() {
        std::vector<std::pair<std::string, std::string>> v(count(2));
        for (auto& [k, val] : v) {
            k = str();
            val = str();
        }
        if (!ok_) v.clear();
        return v;
    }

private:
    bool need(uint64_t n) {
        if (!ok_ || n > d_.size() - pos_) {
            ok_ = false;
            return false;
        }
        return true;
    }
    std::string_view d_;
    size_t pos_{0};
    bool ok_{true};
};

// Prefix `body` (type + payload already written) with its length.
void frame_message(std::string& out, uint16_t type, std::string_view payload);
[[nodiscard]] std::string make_message(uint16_t type, std::string_view payload);

// Splits a byte stream into messages. Feed bytes as they arrive; next()
// yields each complete message (type, payload) in order. A length beyond
// kMaxMessage (or below the type field) is a protocol error: the stream
// cannot be resynchronised and must be closed.
class MessageSplitter {
public:
    void feed(const char* data, size_t n);
    struct Message {
        uint16_t type{0};
        std::string_view payload;  // valid until the next feed()
    };
    // True and fills `m` when a message is ready.
    bool next(Message& m);
    [[nodiscard]] bool error() const noexcept { return error_; }
    [[nodiscard]] size_t buffered() const noexcept { return buf_.size() - head_; }

private:
    std::string buf_;
    size_t head_{0};
    bool error_{false};
};

// 64-bit content hash (not cryptographic): used to compare row encodings.
[[nodiscard]] uint64_t hash64(std::string_view data) noexcept;

}  // namespace bromux::wire
