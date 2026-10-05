#include "bromux/screen_source.h"

#include <algorithm>
#include <iterator>

namespace bromux {

bool decode_history(const HistoryMsg& h, HistoryChunk& out, std::string* err) {
    out.feed_seq = h.feed_seq;
    out.first_row = int64_t(h.first_row);
    out.history_rows = h.history_rows;
    out.epoch = h.epoch;
    out.start = int64_t(h.start);
    out.styles.clear();
    out.rows.resize(h.rows.size());
    for (size_t i = 0; i < h.rows.size(); ++i) {
        wire::Reader rr(h.rows[i]);
        if (!decode_row(rr, out.rows[i], out.styles)) {
            if (err) *err = "bad history row";
            return false;
        }
    }
    return true;
}

ScreenSource::ScreenSource(const ScreenModel& model, FetchFn fetch)
    : m_(model), fetch_(std::move(fetch)), modes_(modes_from(model.modes())), epoch_(model.history_epoch()),
      seen_cols_(model.cols()), seen_rows_(model.rows()), seen_alt_(model.alt_screen_active()),
      seen_top_(model.screen_top_row()) {}

bropty::RowView ScreenSource::row_at(int64_t abs) const {
    const int64_t top = m_.screen_top_row();
    if (abs >= top) {
        const int64_t y = abs - top;
        return y < m_.rows() ? m_.row(int(y)) : bropty::RowView{};
    }
    if (abs < first_row()) return bropty::RowView{};
    auto it = hist_.find(abs);
    if (it == hist_.end()) return bropty::RowView{};
    return it->second.row.view(*it->second.styles);
}

uint64_t ScreenSource::row_serial(int64_t abs) const noexcept {
    const int64_t y = abs - m_.screen_top_row();
    return y >= 0 && y < m_.rows() ? m_.row_serial(int(y)) : 0;
}

const std::string* ScreenSource::hyperlink_uri(int64_t row, uint32_t id) const noexcept {
    const ModelLink* link = nullptr;
    if (row >= m_.screen_top_row()) {
        link = m_.hyperlink(id);
    } else {
        auto it = hist_.find(row);
        if (it != hist_.end()) link = it->second.styles->link(id);
    }
    return link ? &link->uri : nullptr;
}

// Whole aligned blocks, each asked for once: a view asks for the rows it
// shows on every frame until they come, a search for the lines it reaches.
void ScreenSource::request_rows(int64_t first, int64_t end) const {
    if (!fetch_ || m_.alt_screen_active()) return;
    first = std::max(first, m_.history_first_row());
    end = std::min(end, m_.screen_top_row());
    if (first >= end) return;
    const int64_t k = int64_t(kFetchRows);
    for (int64_t b = first - first % k; b < end; b += k) {
        if (inflight_blocks_.count(b)) continue;
        const int64_t lo = std::max(b, first);
        const int64_t hi = std::min(b + k, end);
        // Held rows are consecutive keys: all of [lo, hi) is here when the
        // entry at lo and the one at hi - 1 are hi - lo entries apart.
        auto a = hist_.find(lo);
        auto z = hist_.find(hi - 1);
        if (a != hist_.end() && z != hist_.end() && std::distance(a, z) == hi - 1 - lo) continue;
        const int64_t from = std::max(b, m_.history_first_row());
        const int64_t to = std::min(b + k, m_.screen_top_row());
        const uint32_t req = fetch_(from, uint32_t(to - from));
        if (req == 0) return;  // not connected: nothing to wait for
        inflight_.emplace(req, b);
        inflight_blocks_.insert(b);
    }
}

void ScreenSource::drop_history() {
    hist_.clear();
    inflight_.clear();  // their answers carry the old epoch and are dropped
    inflight_blocks_.clear();
    provisional_ = -1;
}

void ScreenSource::frame_applied(const ScreenModel::Effects& fx) {
    ++changes_;
    if (fx.modes) modes_ = modes_from(m_.modes());
    const bool renumbered = m_.history_epoch() != epoch_ || m_.cols() != seen_cols_ || m_.rows() != seen_rows_;
    const bool switched = m_.alt_screen_active() != seen_alt_;
    if (renumbered) {
        // The server's reflow already happened: positions cannot be carried
        // (RowSource readers of a non-Terminal drop them), only reported.
        notify_before_resize();
        epoch_ = m_.history_epoch();
        seen_cols_ = m_.cols();
        seen_rows_ = m_.rows();
        drop_history();
    }
    // Rows evicted at the front of history are gone.
    hist_.erase(hist_.begin(), hist_.lower_bound(m_.history_first_row()));
    const bool history_moved = m_.screen_top_row() != seen_top_;
    seen_top_ = m_.screen_top_row();
    if (provisional_ >= 0 &&
        (history_moved || m_.rows() == 0 || m_.row_serial(0) != provisional_top_ || !hist_.count(provisional_))) {
        hist_.erase(provisional_);
        provisional_ = -1;
    }
    if (renumbered) notify_after_resize();
    if (switched) {
        seen_alt_ = m_.alt_screen_active();
        notify_screen_switched();
    }
}

void ScreenSource::history_arrived(uint32_t req, const HistoryChunk& chunk) {
    auto f = inflight_.find(req);
    if (f != inflight_.end()) {
        inflight_blocks_.erase(f->second);
        inflight_.erase(f);
    }
    if (chunk.epoch != m_.history_epoch()) return;
    const int64_t first = m_.history_first_row();
    const int64_t top = m_.screen_top_row();
    // The newest history row as the server read it may still unwrap (its
    // line continued on the screen).
    const int64_t newest = chunk.first_row + int64_t(chunk.history_rows) - 1;
    auto styles = std::make_shared<const StylePool>(chunk.styles);
    bool stored = false;
    for (size_t i = 0; i < chunk.rows.size(); ++i) {
        const int64_t abs = chunk.start + int64_t(i);
        if (abs < first || abs >= top) continue;  // evicted since / not history in this model yet
        const ModelRow& row = chunk.rows[i];
        if (abs == newest && (row.flags & bropty::Row_Wrapped)) {
            // Keep it only while it is still the newest here; a row that moved
            // on since was read before its line was settled.
            if (abs + 1 != top || m_.rows() == 0) continue;
            provisional_ = abs;
            provisional_top_ = m_.row_serial(0);
        }
        hist_[abs] = HistRow{row, styles};
        stored = true;
    }
    if (!stored) return;
    ++changes_;
    // Bounded: drop the rows farthest from what just arrived.
    while (hist_.size() > kMaxCachedRows) {
        const int64_t lo = hist_.begin()->first;
        const int64_t hi = std::prev(hist_.end())->first;
        if (chunk.start - lo > hi - chunk.start)
            hist_.erase(hist_.begin());
        else
            hist_.erase(std::prev(hist_.end()));
    }
    if (provisional_ >= 0 && !hist_.count(provisional_)) provisional_ = -1;
}

void ScreenSource::history_failed(uint32_t req) {
    auto f = inflight_.find(req);
    if (f == inflight_.end()) return;
    inflight_blocks_.erase(f->second);
    inflight_.erase(f);
}

}  // namespace bromux
