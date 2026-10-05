#include "bromux/codec.h"

namespace bromux {

namespace {

enum RunKind : uint32_t { Run_Blank = 0, Run_Plain = 1, Run_General = 2 };

constexpr uint32_t kCellBitsMask = 0x1FFFFFF;  // code point, width role, cluster, protect
constexpr uint64_t kMaxCols = 0xFFFF;

bool is_plain(const bropty::Cell& c) noexcept { return c.bits != 0 && c.bits == uint32_t(c.cp()); }

void write_style(wire::Writer& w, const bropty::Style& s, const bropty::Terminal& term) {
    w.u32(s.fg.packed());
    w.u32(s.bg.packed());
    w.u32(s.underline_color.packed());
    w.varint(s.attrs);
    w.u8(uint8_t(s.underline));
    w.u8(uint8_t(s.zone));
    const bropty::Hyperlink* link = s.link ? term.hyperlink(s.link) : nullptr;
    if (link) {
        w.u8(1);
        w.str(link->id);
        w.str(link->uri);
    } else {
        w.u8(0);
    }
}

}  // namespace

ModeState mode_state_from(const bropty::Terminal& t) noexcept {
    const bropty::Modes& m = t.modes();
    ModeState s;
    uint64_t b = 0;
    auto set = [&b](bool on, ModeBit bit) {
        if (on) b |= bit;
    };
    set(m.insert, Mode_Insert);
    set(m.linefeed_newline, Mode_LinefeedNewline);
    set(m.app_cursor_keys, Mode_AppCursorKeys);
    set(m.reverse_video, Mode_ReverseVideo);
    set(m.origin, Mode_Origin);
    set(m.autowrap, Mode_Autowrap);
    set(m.cursor_blink, Mode_CursorBlink);
    set(m.cursor_visible, Mode_CursorVisible);
    set(m.reverse_wrap, Mode_ReverseWrap);
    set(m.app_keypad, Mode_AppKeypad);
    set(m.backarrow_sends_bs, Mode_BackarrowSendsBs);
    set(m.left_right_margins, Mode_LeftRightMargins);
    set(m.focus_events, Mode_FocusEvents);
    set(m.alternate_scroll, Mode_AlternateScroll);
    set(m.meta_sends_escape, Mode_MetaSendsEscape);
    set(m.alt_sends_escape, Mode_AltSendsEscape);
    set(m.bracketed_paste, Mode_BracketedPaste);
    set(m.synchronized_output, Mode_SynchronizedOutput);
    set(m.grapheme_clustering, Mode_GraphemeClustering);
    set(m.color_scheme_updates, Mode_ColorSchemeUpdates);
    set(m.in_band_resize, Mode_InBandResize);
    set(m.allow_deccolm, Mode_AllowDeccolm);
    set(m.deccolm, Mode_Deccolm);
    set(m.deccolm_no_clear, Mode_DeccolmNoClear);
    set(t.alt_screen_active(), Mode_AltScreen);
    s.bits = b;
    s.mouse_tracking = m.mouse_tracking;
    s.mouse_encoding = m.mouse_encoding;
    s.kitty_flags = t.kitty_keyboard_flags();
    return s;
}

void RowEncoder::encode(std::string& out, const bropty::RowView& row, const bropty::Terminal& term) {
    wire::Writer w(out);
    const int cols = row.cells ? row.cols : 0;
    w.varint(row.flags);
    w.varint(uint64_t(cols));

    // Row-local palette in order of first use.
    map_.clear();
    order_.clear();
    uint32_t last_id = UINT32_MAX;
    for (int x = 0; x < cols; ++x) {
        const uint32_t id = row.cells[x].style;
        if (id == last_id) continue;
        last_id = id;
        if (map_.emplace(id, uint32_t(order_.size())).second) order_.push_back(id);
    }
    w.varint(order_.size());
    for (uint32_t id : order_) write_style(w, row.styles[id], term);

    int x = 0;
    while (x < cols) {
        const bropty::Cell& c = row.cells[x];
        const uint32_t sid = c.style;
        const uint32_t pi = map_[sid];
        RunKind kind = c.bits == 0 ? Run_Blank : is_plain(c) ? Run_Plain : Run_General;
        int end = x + 1;
        while (end < cols) {
            const bropty::Cell& n = row.cells[end];
            if (n.style != sid) break;
            RunKind k = n.bits == 0 ? Run_Blank : is_plain(n) ? Run_Plain : Run_General;
            if (k != kind) break;
            ++end;
        }
        w.varint((uint64_t(end - x) << 2) | kind);
        w.varint(pi);
        if (kind == Run_Plain) {
            for (int i = x; i < end; ++i) w.varint(row.cells[i].cp());
        } else if (kind == Run_General) {
            for (int i = x; i < end; ++i) {
                const bropty::Cell& g = row.cells[i];
                w.varint(g.bits & kCellBitsMask);
                if (g.has_cluster()) {
                    std::u32string_view tail = row.clusters ? row.clusters->find(i) : std::u32string_view();
                    w.varint(tail.size());
                    for (char32_t cp : tail) w.varint(uint32_t(cp));
                }
            }
        }
        x = end;
    }
}

std::string encode_blank_row(int cols) {
    std::string out;
    wire::Writer w(out);
    w.varint(0);
    w.varint(uint64_t(cols));
    if (cols <= 0) {
        w.varint(0);
        return out;
    }
    w.varint(1);  // the default style
    w.u32(bropty::Color().packed());
    w.u32(bropty::Color().packed());
    w.u32(bropty::Color().packed());
    w.varint(0);
    w.u8(0);
    w.u8(0);
    w.u8(0);
    w.varint((uint64_t(cols) << 2) | Run_Blank);
    w.varint(0);
    return out;
}

void write_palette(wire::Writer& w, const bropty::Palette& p) {
    auto rgb = [&w](bropty::Rgb c) {
        w.u8(c.r);
        w.u8(c.g);
        w.u8(c.b);
    };
    for (const bropty::Rgb& c : p.colors) rgb(c);
    rgb(p.foreground);
    rgb(p.background);
    rgb(p.cursor);
}

bool read_palette(wire::Reader& r, bropty::Palette& p) {
    auto rgb = [&r] {
        bropty::Rgb c;
        c.r = r.u8();
        c.g = r.u8();
        c.b = r.u8();
        return c;
    };
    for (bropty::Rgb& c : p.colors) c = rgb();
    p.foreground = rgb();
    p.background = rgb();
    p.cursor = rgb();
    return r.ok();
}

// ---- client side -------------------------------------------------------------------

StylePool::StylePool() { clear(); }

void StylePool::clear() {
    styles_.clear();
    index_.clear();
    links_.clear();
    link_index_.clear();
    styles_.push_back(bropty::Style{});
    index_.emplace(bropty::Style{}, 0);
}

uint32_t StylePool::intern(bropty::Style s, const ModelLink* link) {
    s.link = 0;
    if (link) {
        std::string key = link->id;
        key.push_back('\0');
        key += link->uri;
        auto it = link_index_.find(key);
        if (it == link_index_.end()) {
            links_.push_back(*link);
            it = link_index_.emplace(std::move(key), uint32_t(links_.size())).first;
        }
        s.link = it->second;
    }
    auto [it, inserted] = index_.emplace(s, uint32_t(styles_.size()));
    if (inserted) styles_.push_back(s);
    return it->second;
}

void ModelRow::reset(int cols) {
    cells.assign(size_t(cols > 0 ? cols : 0), bropty::Cell{});
    clusters.clear();
    flags = 0;
}

bool decode_row(wire::Reader& r, ModelRow& out, StylePool& pool) {
    const uint32_t flags = uint32_t(r.varint_max(UINT32_MAX));
    const int cols = int(r.varint_max(kMaxCols));
    const size_t nstyles = r.count(15);
    if (!r.ok() || nstyles > size_t(cols > 0 ? cols : 1)) return false;
    std::vector<uint32_t> ids(nstyles);
    for (size_t i = 0; i < nstyles; ++i) {
        bropty::Style s;
        s.fg = bropty::Color::from_packed(r.u32());
        s.bg = bropty::Color::from_packed(r.u32());
        s.underline_color = bropty::Color::from_packed(r.u32());
        s.attrs = uint16_t(r.varint_max(0xFFFF));
        const uint8_t ul = r.u8();
        const uint8_t zone = r.u8();
        if (ul > uint8_t(bropty::Underline::Dashed) || zone > 3) return false;
        s.underline = bropty::Underline(ul);
        s.zone = bropty::Zone(zone);
        const uint8_t has_link = r.u8();
        if (has_link > 1) return false;
        ModelLink link;
        if (has_link) {
            link.id = r.str();
            link.uri = r.str();
        }
        if (!r.ok()) return false;
        ids[i] = pool.intern(s, has_link ? &link : nullptr);
    }

    out.flags = flags;
    out.clusters.clear();
    out.cells.assign(size_t(cols), bropty::Cell{});
    int x = 0;
    std::u32string tail;
    while (x < cols) {
        const uint64_t hdr = r.varint();
        const uint64_t count = hdr >> 2;
        const uint32_t kind = uint32_t(hdr & 3);
        const uint64_t si = r.varint();
        if (!r.ok() || count == 0 || count > uint64_t(cols - x) || si >= nstyles || kind > Run_General) return false;
        const uint32_t style = ids[size_t(si)];
        const int end = x + int(count);
        for (int i = x; i < end; ++i) {
            bropty::Cell& c = out.cells[size_t(i)];
            if (kind == Run_Blank) {
                c = bropty::Cell::blank(style);
            } else if (kind == Run_Plain) {
                const uint64_t cp = r.varint_max(0x10FFFF);
                if (cp == 0) return false;
                c = bropty::Cell::make(char32_t(cp), style);
            } else {
                const uint32_t bits = uint32_t(r.varint_max(kCellBitsMask));
                c.bits = bits;
                c.style = style;
                if (c.cp() > 0x10FFFF) return false;
                if (c.has_cluster()) {
                    const size_t n = r.count(1);
                    tail.clear();
                    for (size_t k = 0; k < n; ++k) tail.push_back(char32_t(r.varint_max(0x10FFFF)));
                    if (!r.ok()) return false;
                    if (!tail.empty()) out.clusters.set(i, tail);
                }
            }
            if (!r.ok()) return false;
        }
        x = end;
    }
    return r.ok();
}

}  // namespace bromux
