#pragma once
// Frames built straight from a bropty Terminal, as the server would send a
// fresh attachment its first one; and History answers the same way.
#include <bromux/codec.h>
#include <bromux/protocol.h>
#include <bromux/screen_source.h>

#include <algorithm>

namespace frames {

inline void write_history(bromux::wire::Writer& w, const bropty::Terminal& t) {
    w.u8(bromux::Op_History);
    w.varint(uint64_t(t.history_first_row()));
    w.varint(t.history_rows());
    w.varint(t.row_numbering());
}

// What a client was sent: frame() then sends only rows that differ.
struct Sent {
    int cols{-1};
    std::vector<std::string> rows;
};

// A frame for `t`: with `sent`, the size and rows only when they changed
// (the rest of the state always); without, everything.
inline bromux::FrameMsg frame(const bropty::Terminal& t, uint64_t seq, Sent* sent) {
    using namespace bromux;
    FrameMsg f;
    f.frame_seq = seq;
    f.feed_seq = seq;
    wire::Writer w(f.ops);
    RowEncoder enc;
    if (!sent || sent->cols != t.cols() || sent->rows.size() != size_t(t.rows())) {
        w.u8(Op_Size);
        w.varint(uint64_t(t.cols()));
        w.varint(uint64_t(t.rows()));
        if (sent) {
            sent->cols = t.cols();
            sent->rows.assign(size_t(t.rows()), encode_blank_row(t.cols()));
        }
    }
    for (int y = 0; y < t.rows(); ++y) {
        std::string e;
        enc.encode(e, t.row(y), t);
        if (sent && sent->rows[size_t(y)] == e) continue;
        w.u8(Op_Row);
        w.varint(uint64_t(y));
        w.raw(e);
        if (sent) sent->rows[size_t(y)] = std::move(e);
    }
    const bropty::CursorState c = t.cursor();
    w.u8(Op_Cursor);
    w.varint(uint64_t(c.row));
    w.varint(uint64_t(c.col));
    w.u8(uint8_t((c.visible ? 1 : 0) | (c.pending_wrap ? 2 : 0) | (c.blink ? 4 : 0)));
    w.u8(uint8_t(c.shape));
    const ModeState m = mode_state_from(t);
    w.u8(Op_Modes);
    w.varint(m.bits);
    w.u8(uint8_t(m.mouse_tracking));
    w.u8(uint8_t(m.mouse_encoding));
    w.varint(m.kitty_flags);
    for (uint8_t which = 0; which < 3; ++which) {
        w.u8(Op_Text);
        w.u8(which);
        w.str(which == 0 ? t.title() : which == 1 ? t.icon_name() : t.cwd());
    }
    w.u8(Op_Palette);
    write_palette(w, t.palette());
    write_history(w, t);
    return f;
}

// A full-state frame for `t`, as a fresh attachment gets it.
inline bromux::FrameMsg full_frame(const bropty::Terminal& t, uint64_t seq) { return frame(t, seq, nullptr); }

// The History answer to FetchHistory(start, count), as the server builds it.
inline bromux::HistoryMsg history(const bropty::Terminal& t, uint64_t start, uint32_t count) {
    bromux::HistoryMsg r;
    r.first_row = uint64_t(t.history_first_row());
    r.history_rows = t.history_rows();
    r.epoch = t.row_numbering();
    const uint64_t end = r.first_row + r.history_rows;
    r.start = std::clamp<uint64_t>(start, r.first_row, end);
    const uint64_t n = std::min<uint64_t>(count, end - r.start);
    r.rows.resize(size_t(n));
    bromux::RowEncoder enc;
    for (uint64_t i = 0; i < n; ++i) enc.encode(r.rows[size_t(i)], t.history_row(size_t(r.start - r.first_row + i)), t);
    return r;
}

}  // namespace frames
