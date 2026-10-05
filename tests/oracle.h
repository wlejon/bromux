#pragma once
// The oracle: a client's reconstructed screen must equal, cell for cell, a
// bropty Terminal fed the same PTY output directly. compare_*() return an
// empty string when equal, else a description of the first difference.

#include <bromux/client.h>
#include <bromux/screen_model.h>
#include <bromux/tee.h>

#include <bropty/terminal.h>

#include <string>

namespace oracle {

inline std::string style_text(const bropty::Style& s) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "{fg %08x bg %08x ul %08x attrs %04x u %d zone %d}", s.fg.packed(), s.bg.packed(),
                  s.underline_color.packed(), unsigned(s.attrs), int(s.underline), int(s.zone));
    return buf;
}

// `link_a` / `link_b` resolve a style's link id to "id\nuri" ("" = none).
template <class LinkA, class LinkB>
std::string compare_row(const bropty::RowView& a, const bropty::RowView& b, LinkA link_a, LinkB link_b) {
    if (a.cols != b.cols) return "width " + std::to_string(a.cols) + " vs " + std::to_string(b.cols);
    if (a.flags != b.flags) return "row flags " + std::to_string(a.flags) + " vs " + std::to_string(b.flags);
    for (int x = 0; x < a.cols; ++x) {
        const bropty::Cell& ca = a[x];
        const bropty::Cell& cb = b[x];
        const std::string at = " at col " + std::to_string(x);
        if ((ca.bits & 0x1FFFFFF) != (cb.bits & 0x1FFFFFF))
            return "cell bits " + std::to_string(ca.bits & 0x1FFFFFF) + " vs " + std::to_string(cb.bits & 0x1FFFFFF) + at;
        if (ca.has_cluster() && a.cluster(x) != b.cluster(x)) return "cluster differs" + at;
        bropty::Style sa = a.style(x);
        bropty::Style sb = b.style(x);
        const std::string la = link_a(sa.link);
        const std::string lb = link_b(sb.link);
        if (la != lb) return "hyperlink differs" + at;
        sa.link = 0;
        sb.link = 0;
        if (!(sa == sb)) return "style " + style_text(sa) + " vs " + style_text(sb) + at;
    }
    return {};
}

inline std::string term_link(const bropty::Terminal& t, uint32_t id) {
    if (!id) return {};
    const bropty::Hyperlink* h = t.hyperlink(id);
    return h ? h->id + "\n" + h->uri : std::string();
}

inline std::string model_link(const bromux::StylePool& p, uint32_t id) {
    const bromux::ModelLink* l = p.link(id);
    return l ? l->id + "\n" + l->uri : std::string();
}

inline std::string compare_screen(const bromux::ScreenModel& m, const bropty::Terminal& t) {
    if (m.cols() != t.cols() || m.rows() != t.rows())
        return "size " + std::to_string(m.cols()) + "x" + std::to_string(m.rows()) + " vs " +
               std::to_string(t.cols()) + "x" + std::to_string(t.rows());
    for (int y = 0; y < t.rows(); ++y) {
        std::string d = compare_row(
            m.row(y), t.row(y), [&](uint32_t id) { return model_link(m.styles(), id); },
            [&](uint32_t id) { return term_link(t, id); });
        if (!d.empty())
            return "row " + std::to_string(y) + ": " + d + "\n   model: [" + m.row(y).text(false) + "]\n   term:  [" +
                   t.row(y).text(false) + "]";
    }
    const bropty::CursorState a = m.cursor();
    const bropty::CursorState b = t.cursor();
    if (a.row != b.row || a.col != b.col || a.visible != b.visible || a.pending_wrap != b.pending_wrap ||
        a.shape != b.shape || a.blink != b.blink)
        return "cursor " + std::to_string(a.row) + "," + std::to_string(a.col) + " vs " + std::to_string(b.row) + "," +
               std::to_string(b.col);
    if (!(m.modes() == bromux::mode_state_from(t))) return "modes differ";
    if (m.title() != t.title()) return "title [" + m.title() + "] vs [" + t.title() + "]";
    if (m.icon_name() != t.icon_name()) return "icon name differs";
    if (m.cwd() != t.cwd()) return "cwd differs";
    const bropty::Palette& pa = m.palette();
    const bropty::Palette& pb = t.palette();
    if (!(pa.colors == pb.colors) || !(pa.foreground == pb.foreground) || !(pa.background == pb.background) ||
        !(pa.cursor == pb.cursor))
        return "palette differs";
    if (m.history_rows() != t.history_rows())
        return "history rows " + std::to_string(m.history_rows()) + " vs " + std::to_string(t.history_rows());
    return {};
}

inline std::string compare_history(const bromux::HistoryChunk& h, const bropty::Terminal& t) {
    if (h.history_rows != t.history_rows())
        return "history rows " + std::to_string(h.history_rows) + " vs " + std::to_string(t.history_rows());
    for (size_t i = 0; i < h.rows.size(); ++i) {
        const size_t idx = size_t(h.start + i);
        std::string d = compare_row(
            h.row(i), t.history_row(idx), [&](uint32_t id) { return model_link(h.styles, id); },
            [&](uint32_t id) { return term_link(t, id); });
        if (!d.empty()) return "history row " + std::to_string(idx) + ": " + d;
    }
    return {};
}

// Replay a session's recording up to `feed_seq` and compare.
inline std::string compare_with_tee(const bromux::ScreenModel& m, const std::string& tee_path) {
    bromux::TeeHeader h;
    std::vector<bromux::TeeRecord> recs;
    std::string err;
    if (!bromux::read_tee(tee_path, h, recs, &err)) return "tee: " + err;
    if (recs.size() < m.feed_seq())
        return "tee has " + std::to_string(recs.size()) + " records, model shows " + std::to_string(m.feed_seq());
    auto t = bromux::replay_tee(h, recs, size_t(m.feed_seq()));
    return compare_screen(m, *t);
}

}  // namespace oracle
