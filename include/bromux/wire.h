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
//
// The primitives and the framing are brolink's (brolink/wire.h); bromux
// fixes its own message bound and adds the row hash.

#include <brolink/wire.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace bromux::wire {

// Largest message either side accepts (type + body). A frame with a full
// screen of styled wide text stays far below it.
inline constexpr size_t kMaxMessage = 32u << 20;
using brolink::wire::kLengthBytes;

using brolink::wire::Reader;
using brolink::wire::Writer;
using brolink::wire::frame_message;
using brolink::wire::make_message;

// Splits a byte stream into messages, bounded by bromux's kMaxMessage. A
// length beyond it (or below the type field) is a protocol error: the stream
// cannot be resynchronised and must be closed.
class MessageSplitter : public brolink::wire::MessageSplitter {
public:
    MessageSplitter() : brolink::wire::MessageSplitter(kMaxMessage) {}
};

// 64-bit content hash (not cryptographic): used to compare row encodings.
[[nodiscard]] uint64_t hash64(std::string_view data) noexcept;

}  // namespace bromux::wire
