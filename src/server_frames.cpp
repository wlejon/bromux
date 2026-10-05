// Frames: the diff between what a client was last sent and the session now.
#include "server_impl.h"

#include <algorithm>
#include <cstdlib>
#include <unordered_map>

namespace bromux::detail {

namespace {

constexpr uint32_t kMaxHistoryRows = 10000;

bool same_cursor(const bropty::CursorState& a, const bropty::CursorState& b) {
    return a.row == b.row && a.col == b.col && a.pending_wrap == b.pending_wrap && a.visible == b.visible &&
           a.shape == b.shape && a.blink == b.blink;
}

uint64_t hash_palette(const bropty::Palette& p) {
    std::string bytes;
    wire::Writer w(bytes);
    write_palette(w, p);
    return wire::hash64(bytes);
}

// A whole-screen shift that turns `old` (the row keys the client has) into
// more of `cur` than it already matches: rows moving up k (k > 0) or down.
// Votes come from rows whose content now sits elsewhere; keys that occur more
// than once in `old` (blank rows, key 0) do not vote. Returns 0 when no shift
// pays.
int detect_scroll(const std::vector<uint64_t>& old, const std::vector<uint64_t>& cur) {
    const int n = int(cur.size());
    if (n < 3 || int(old.size()) != n) return 0;
    int differing = 0;
    for (int y = 0; y < n; ++y) differing += old[size_t(y)] != cur[size_t(y)];
    if (differing < 3) return 0;
    std::unordered_map<uint64_t, int> where;  // key -> row in old, -1 when ambiguous
    where.reserve(size_t(n) * 2);
    for (int y = 0; y < n; ++y) {
        auto [it, inserted] = where.emplace(old[size_t(y)], y);
        if (!inserted) it->second = -1;
    }
    std::unordered_map<int, int> votes;
    for (int y = 0; y < n; ++y) {
        if (old[size_t(y)] == cur[size_t(y)]) continue;
        auto it = where.find(cur[size_t(y)]);
        if (it == where.end() || it->second < 0) continue;
        ++votes[it->second - y];
    }
    int best = 0;
    int best_votes = 1;  // a shift must save at least two rows
    for (auto [k, v] : votes) {
        if (v > best_votes || (v == best_votes && best != 0 && std::abs(k) < std::abs(best))) {
            best = k;
            best_votes = v;
        }
    }
    return best;
}

// The client's rows after Op_Scroll k: blank rows (key 0) shift in.
void shift_rows(std::vector<uint64_t>& rows, int k) {
    const int n = int(rows.size());
    if (k >= n || -k >= n) {
        std::fill(rows.begin(), rows.end(), 0);
    } else if (k > 0) {
        std::rotate(rows.begin(), rows.begin() + k, rows.end());
        std::fill(rows.end() - k, rows.end(), 0);
    } else if (k < 0) {
        std::rotate(rows.begin(), rows.end() + k, rows.end());
        std::fill(rows.begin(), rows.begin() - k, 0);
    }
}

}  // namespace

// Row stamps are content serials across both screens and resizes: a row
// whose stamp was encoded before (wherever it sat) has that encoding still.
// Only rows with new stamps are encoded; advance_generation() then makes the
// next change to any row take a stamp not seen here.
void ServerSession::refresh() {
    if (enc_version == feed_seq) return;
    static const bool full_always = std::getenv("BROMUX_FULL_REFRESH") != nullptr;
    bropty::Terminal& term_ref = t();
    const int cols = term_ref.cols();
    const int rows = term_ref.rows();
    if (cols != enc_cols) blank_enc = encode_blank_row(cols);
    enc_cols = cols;
    enc_rows = rows;
    std::vector<std::string> enc(static_cast<size_t>(rows));
    std::vector<uint64_t> stamps(static_cast<size_t>(rows));
    std::unordered_map<uint64_t, size_t> was;  // stamp -> old row, built when a row moved
    bool indexed = false;
    for (int y = 0; y < rows; ++y) {
        const size_t i = size_t(y);
        const uint64_t st = term_ref.row_stamp(y);
        stamps[i] = st;
        size_t from = SIZE_MAX;
        if (!full_always) {
            if (i < row_stamp.size() && row_stamp[i] == st) {
                from = i;
            } else {
                if (!indexed) {
                    was.reserve(row_stamp.size() * 2);
                    for (size_t j = 0; j < row_stamp.size(); ++j) was.emplace(row_stamp[j], j);
                    indexed = true;
                }
                auto it = was.find(st);
                if (it != was.end()) from = it->second;
            }
        }
        if (from == i)
            enc[i] = std::move(row_enc[i]);  // stamps are unique on a screen: nothing else maps here
        else if (from != SIZE_MAX)
            enc[i] = row_enc[from];
        else
            encoder.encode(enc[i], term_ref.row(y), term_ref);
    }
    term_ref.advance_generation();
    row_enc = std::move(enc);
    row_stamp = std::move(stamps);
    row_key.resize(size_t(rows));
    for (size_t i = 0; i < size_t(rows); ++i) row_key[i] = row_enc[i] == blank_enc ? 0 : row_stamp[i];
    palette_hash = hash_palette(term_ref.palette());
    enc_version = feed_seq;
}

bool ServerCore::send_frame(Attachment& a, Clock::time_point now) {
    ServerSession& s = *a.session;
    s.refresh();
    bropty::Terminal& t = s.t();
    std::string ops;
    wire::Writer w(ops);

    if (a.sent_cols != s.enc_cols || a.sent_rows != s.enc_rows) {
        w.u8(Op_Size);
        w.varint(uint64_t(s.enc_cols));
        w.varint(uint64_t(s.enc_rows));
        a.sent_cols = s.enc_cols;
        a.sent_rows = s.enc_rows;
        a.sent_key.assign(size_t(s.enc_rows), 0);  // a resized model is blank
    }
    if (int k = detect_scroll(a.sent_key, s.row_key)) {
        w.u8(Op_Scroll);
        w.svarint(k);
        shift_rows(a.sent_key, k);
    }
    for (int y = 0; y < s.enc_rows; ++y) {
        if (a.sent_key[size_t(y)] == s.row_key[size_t(y)]) continue;
        w.u8(Op_Row);
        w.varint(uint64_t(y));
        w.raw(s.row_enc[size_t(y)]);
        a.sent_key[size_t(y)] = s.row_key[size_t(y)];
    }
    const bropty::CursorState cur = t.cursor();
    if (!a.cursor_sent || !same_cursor(cur, a.sent_cursor)) {
        w.u8(Op_Cursor);
        w.varint(uint64_t(std::max(cur.row, 0)));
        w.varint(uint64_t(std::max(cur.col, 0)));
        w.u8(uint8_t((cur.visible ? 1 : 0) | (cur.pending_wrap ? 2 : 0) | (cur.blink ? 4 : 0)));
        w.u8(uint8_t(cur.shape));
        a.sent_cursor = cur;
        a.cursor_sent = true;
    }
    const ModeState modes = mode_state_from(t);
    if (!a.modes_sent || !(modes == a.sent_modes)) {
        w.u8(Op_Modes);
        w.varint(modes.bits);
        w.u8(uint8_t(modes.mouse_tracking));
        w.u8(uint8_t(modes.mouse_encoding));
        w.varint(modes.kitty_flags);
        a.sent_modes = modes;
        a.modes_sent = true;
    }
    auto text = [&w](uint8_t which, const std::string& now_value, std::string& sent, bool force) {
        if (!force && now_value == sent) return;
        w.u8(Op_Text);
        w.u8(which);
        w.str(now_value);
        sent = now_value;
    };
    text(0, t.title(), a.sent_title, !a.text_sent);
    text(1, t.icon_name(), a.sent_icon, !a.text_sent);
    text(2, t.cwd(), a.sent_cwd, !a.text_sent);
    a.text_sent = true;
    if (!a.palette_sent || a.sent_palette != s.palette_hash) {
        w.u8(Op_Palette);
        write_palette(w, t.palette());
        a.sent_palette = s.palette_hash;
        a.palette_sent = true;
    }
    const uint64_t hfirst = uint64_t(t.history_first_row());
    const uint64_t hrows = t.history_rows();
    const uint64_t epoch = t.row_numbering();
    if (!a.history_sent || hfirst != a.sent_history_first || hrows != a.sent_history ||
        epoch != a.sent_epoch) {
        w.u8(Op_History);
        w.varint(hfirst);
        w.varint(hrows);
        w.varint(epoch);
        a.sent_history_first = hfirst;
        a.sent_history = hrows;
        a.sent_epoch = epoch;
        a.history_sent = true;
    }

    a.sent_version = s.feed_seq;
    if (ops.empty() && !a.force_frame) return false;
    FrameMsg f;
    f.session = s.id;
    f.frame_seq = ++a.frame_seq;
    f.feed_seq = s.feed_seq;
    f.ops = std::move(ops);
    std::string msg = encode(f);
    a.inflight.emplace_back(f.frame_seq, msg.size());
    a.inflight_bytes += msg.size();
    a.last_frame = now;
    send_raw(*a.conn, msg);
    return true;
}

void ServerCore::flush_events(Attachment& a) {
    if (a.conn->closing) return;
    // A client that stopped reading keeps its events queued (and bounded).
    constexpr size_t kMaxBacklog = 8u << 20;
    while (!a.events.empty() && loop_->pending_output(a.conn->id) < kMaxBacklog) {
        a.events_bytes -= a.events.front().size();
        send_raw(*a.conn, a.events.front());
        a.events.pop_front();
    }
    if (a.events.empty()) a.last_event_bell = false;
    if (a.events_dropped && a.events.empty()) {
        EventMsg e;
        e.session = a.session->id;
        e.kind = EventKind::EventsDropped;
        e.x = int64_t(a.events_dropped);
        a.events_dropped = 0;
        send(*a.conn, e);
    }
}

void ServerCore::send_history(Conn& c, const FetchHistoryMsg& m) {
    auto it = sessions_.find(m.session);
    if (it == sessions_.end()) {
        send_error(c, m.req, ErrorCode::NoSuchSession, "no session " + std::to_string(m.session));
        return;
    }
    ServerSession& s = *it->second;
    bropty::Terminal& t = s.t();
    HistoryMsg r;
    r.req = m.req;
    r.session = s.id;
    r.feed_seq = s.feed_seq;
    r.first_row = uint64_t(t.history_first_row());
    r.history_rows = t.history_rows();
    r.epoch = t.row_numbering();
    // Absolute rows, clamped to what is held: evicted rows are gone, and the
    // screen is not history.
    const uint64_t end = r.first_row + r.history_rows;
    r.start = std::clamp<uint64_t>(m.start, r.first_row, end);
    const uint64_t n = std::min<uint64_t>({uint64_t(m.count), end - r.start, kMaxHistoryRows});
    r.rows.resize(size_t(n));
    for (uint64_t i = 0; i < n; ++i)
        s.encoder.encode(r.rows[size_t(i)], t.history_row(size_t(r.start - r.first_row + i)), t);
    send(c, r);
}

}  // namespace bromux::detail
