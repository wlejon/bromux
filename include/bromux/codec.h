#pragma once
// Screen codec: how rows, styles and terminal state travel in Frame and
// History messages, and the client-side containers they decode into.
//
// A row is sent whole, self-contained (no reference to earlier messages):
//     row   := varint flags, varint cols, varint nstyles, style * nstyles, run *
//     style := u32 fg, u32 bg, u32 underline_color   (bropty::Color::packed())
//              varint attrs, u8 underline, u8 zone,
//              u8 has_link [, str link_id, str uri]
//     run   := varint (count << 2 | kind), varint style_index, payload
//        kind 0 blank:   `count` cells of Cell{0, style}; no payload
//        kind 1 plain:   `count` varint code points (narrow, no cluster, unprotected)
//        kind 2 general: per cell varint cell_bits (bropty::Cell::bits, 25 bits);
//                        when the cluster bit is set: varint n, n varint code points
//                        (the cluster's tail)
// The runs cover exactly `cols` cells. Styles are a row-local palette in
// order of first use, so equal rows encode to equal bytes.
//
// The Frame op stream (FrameMsg::ops) is a sequence of
//     u8 op, body
// with the ops below. A client applies them in order to its ScreenModel; an
// op it does not know fails the frame, so a server sends the minor-1 ops
// only to clients that said they know them.

#include "bromux/wire.h"

#include <bropty/cell.h>
#include <bropty/color.h>
#include <bropty/style.h>
#include <bropty/terminal.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace bromux {

enum FrameOp : uint8_t {
    Op_Size = 1,     // varint cols, varint rows: every row becomes blank at the new size
    Op_Scroll = 2,   // svarint k: rows move up k (down -k); vacated rows become blank
    Op_Row = 3,      // varint y, row
    Op_Cursor = 4,   // varint row, varint col, u8 flags (1 visible, 2 pending_wrap, 4 blink), u8 shape
    Op_Modes = 5,    // varint mode bits, u8 mouse tracking, u8 mouse encoding, varint kitty flags
    Op_Text = 6,     // u8 which (0 title, 1 icon name, 2 cwd), str value
    Op_Palette = 7,  // 259 x (r, g, b): colors[0..255], foreground, background, cursor
    // varint first row, varint history rows held, varint epoch: history is
    // absolute rows first .. first + rows - 1, the screen starts at first +
    // rows (also on the alternate screen, which shows no history). A row
    // keeps its number while epoch stays the same; a resize (the reflow)
    // starts a new epoch.
    Op_History = 8,
    // ---- minor 1 (sent only to clients whose Hello said minor >= 1) ----
    // Op_Text gains which 3: the pointer shape the program asked for (OSC 22).
    // varint drop_front, varint keep, varint n, command * n: the OSC 133
    // command records (bropty Terminal::commands()). The client drops its
    // first drop_front records, keeps the next `keep`, and appends these n.
    Op_Commands = 9,
    // The active screen's images and placements (image_codec.h), whole,
    // whenever they change. Pixels travel separately, once per client.
    Op_Images = 10,
    // varint pixel serial, varint width, varint height, varint offset, str
    // bytes: part of an image frame's RGBA (image_codec.h).
    Op_ImageData = 11,
};

// One OSC 133 command record (Op_Commands):
//     svarint prompt row, varint prompt col,
//     u8 flags (1 input, 2 output, 4 end, 8 exit code, 16 finished, 32 trimmed),
//     [svarint row, varint col] for each of input / output / end present,
//     [svarint exit code], str command line
void write_command(wire::Writer& w, const bropty::CommandRecord& c);
bool read_command(wire::Reader& r, bropty::CommandRecord& c);
[[nodiscard]] bool same_command(const bropty::CommandRecord& a, const bropty::CommandRecord& b) noexcept;

enum ModeBit : uint64_t {
    Mode_Insert = 1ull << 0,
    Mode_LinefeedNewline = 1ull << 1,
    Mode_AppCursorKeys = 1ull << 2,
    Mode_ReverseVideo = 1ull << 3,
    Mode_Origin = 1ull << 4,
    Mode_Autowrap = 1ull << 5,
    Mode_CursorBlink = 1ull << 6,
    Mode_CursorVisible = 1ull << 7,
    Mode_ReverseWrap = 1ull << 8,
    Mode_AppKeypad = 1ull << 9,
    Mode_BackarrowSendsBs = 1ull << 10,
    Mode_LeftRightMargins = 1ull << 11,
    Mode_FocusEvents = 1ull << 12,
    Mode_AlternateScroll = 1ull << 13,
    Mode_MetaSendsEscape = 1ull << 14,
    Mode_AltSendsEscape = 1ull << 15,
    Mode_BracketedPaste = 1ull << 16,
    Mode_SynchronizedOutput = 1ull << 17,
    Mode_GraphemeClustering = 1ull << 18,
    Mode_ColorSchemeUpdates = 1ull << 19,
    Mode_InBandResize = 1ull << 20,
    Mode_AllowDeccolm = 1ull << 21,
    Mode_Deccolm = 1ull << 22,
    Mode_DeccolmNoClear = 1ull << 23,
    Mode_AltScreen = 1ull << 24,
};

// The terminal modes a client sees (what a UI needs to decide how to treat
// mouse, paste and scrolling, and what a renderer needs).
struct ModeState {
    uint64_t bits{Mode_Autowrap | Mode_CursorVisible | Mode_MetaSendsEscape | Mode_AltSendsEscape |
                  Mode_GraphemeClustering};
    bropty::MouseTracking mouse_tracking{bropty::MouseTracking::None};
    bropty::MouseEncoding mouse_encoding{bropty::MouseEncoding::Default};
    uint32_t kitty_flags{0};

    [[nodiscard]] bool has(ModeBit b) const noexcept { return (bits & b) != 0; }
    bool operator==(const ModeState&) const noexcept = default;
};
[[nodiscard]] ModeState mode_state_from(const bropty::Terminal& t) noexcept;
// The other way: bropty Modes with what a ModeState carries (the rest at
// their defaults).
[[nodiscard]] bropty::Modes modes_from(const ModeState& m) noexcept;

// ---- server side -------------------------------------------------------------------

// Encodes rows; keeps scratch state between calls (not thread-safe).
class RowEncoder {
public:
    // Append the encoding of `row`. Hyperlink ids in its styles resolve
    // through `term` (both screen and history rows use the terminal's ids).
    void encode(std::string& out, const bropty::RowView& row, const bropty::Terminal& term);

private:
    std::unordered_map<uint32_t, uint32_t> map_;  // style id -> palette index
    std::vector<uint32_t> order_;                 // palette index -> style id
};

// Encoding of a blank row (every cell Cell{0, 0}, no flags).
[[nodiscard]] std::string encode_blank_row(int cols);

void write_palette(wire::Writer& w, const bropty::Palette& p);
bool read_palette(wire::Reader& r, bropty::Palette& p);

// ---- client side -------------------------------------------------------------------

struct ModelLink {
    std::string id;
    std::string uri;
};

// Interned styles for decoded rows. Id 0 is the default style. A style's
// `link` field is a 1-based index into links() (0 = none), so a RowView over
// decoded cells resolves exactly like a bropty one.
class StylePool {
public:
    StylePool();
    uint32_t intern(bropty::Style s, const ModelLink* link);
    [[nodiscard]] const bropty::Style& get(uint32_t id) const noexcept { return styles_[id]; }
    [[nodiscard]] const bropty::Style* data() const noexcept { return styles_.data(); }
    [[nodiscard]] size_t size() const noexcept { return styles_.size(); }
    [[nodiscard]] const ModelLink* link(uint32_t link_id) const noexcept {
        return link_id >= 1 && link_id <= links_.size() ? &links_[link_id - 1] : nullptr;
    }
    void clear();

private:
    std::vector<bropty::Style> styles_;
    std::unordered_map<bropty::Style, uint32_t, bropty::StyleHash> index_;
    std::vector<ModelLink> links_;
    std::unordered_map<std::string, uint32_t> link_index_;
};

// A decoded row.
struct ModelRow {
    std::vector<bropty::Cell> cells;
    bropty::ClusterMap clusters;
    uint32_t flags{0};

    void reset(int cols);
    [[nodiscard]] bropty::RowView view(const StylePool& pool) const noexcept {
        return bropty::RowView{cells.data(), int(cells.size()), flags, pool.data(),
                               clusters.empty() ? nullptr : &clusters};
    }
};

// Decode one row (the reader is left after it). False on malformed input.
bool decode_row(wire::Reader& r, ModelRow& out, StylePool& pool);

}  // namespace bromux
