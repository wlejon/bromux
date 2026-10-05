#pragma once
// ScreenSource: a session's ScreenModel plus the history rows fetched for it,
// as a bropty::RowSource -- so bropty's TerminalView, Selection, Search and
// link detection run on the client over a muxed session as they do over a
// local Terminal (Client::view() builds one).
//
// Rows keep the server terminal's absolute numbers (Op_History). History
// rows are not in frames: row_at() returns an empty view for one not fetched
// yet, request_rows() fetches it (in blocks, once), and when it arrives the
// Client hands it over (history_arrived) and reports ClientEvent::History so
// the UI redraws. A new numbering epoch (the session was resized) drops the
// cache and tells observers it was a resize; entering or leaving the
// alternate screen tells them that. Screen rows carry the model's serials, so
// a TerminalView re-reads only rows a frame rewrote.
//
// Single-threaded, like the ScreenModel it reads (and which must outlive it).

#include "bromux/codec.h"
#include "bromux/protocol.h"
#include "bromux/screen_model.h"

#include <bropty/row_source.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace bromux {

// History rows fetched on demand, with their own styles. Rows are numbered
// absolutely (ScreenModel::history_first_row()): rows[i] is row start + i.
struct HistoryChunk {
    uint64_t feed_seq{0};
    int64_t first_row{0};      // the oldest history row the session held then
    uint64_t history_rows{0};  // rows the session held when they were read
    uint64_t epoch{0};         // the numbering (ScreenModel::history_epoch())
    int64_t start{0};
    StylePool styles;
    std::vector<ModelRow> rows;
    [[nodiscard]] bropty::RowView row(size_t i) const noexcept { return rows[i].view(styles); }
};
// Decode a History message's rows. False (and `err`) on malformed rows.
bool decode_history(const HistoryMsg& m, HistoryChunk& out, std::string* err = nullptr);

class ScreenSource final : public bropty::RowSource {
public:
    // Sends a request for history rows [start, start + count) and returns its
    // id (0: it could not be sent). The answer comes back through
    // history_arrived() / history_failed() with that id.
    using FetchFn = std::function<uint32_t(int64_t start, uint32_t count)>;
    static constexpr uint32_t kFetchRows = 256;          // rows per request (aligned blocks)
    static constexpr size_t kMaxCachedRows = 200000;     // history rows kept

    ScreenSource(const ScreenModel& model, FetchFn fetch);

    // ---- bropty::RowSource
    [[nodiscard]] int cols() const noexcept override { return m_.cols(); }
    [[nodiscard]] int rows() const noexcept override { return m_.rows(); }
    [[nodiscard]] int64_t first_row() const noexcept override {
        return m_.alt_screen_active() ? m_.screen_top_row() : m_.history_first_row();
    }
    [[nodiscard]] int64_t screen_top_row() const noexcept override { return m_.screen_top_row(); }
    [[nodiscard]] bool alt_screen_active() const noexcept override { return m_.alt_screen_active(); }
    [[nodiscard]] bropty::RowView row_at(int64_t abs) const override;
    [[nodiscard]] uint64_t row_serial(int64_t abs) const noexcept override;
    [[nodiscard]] const std::string* hyperlink_uri(int64_t row, uint32_t id) const noexcept override;
    [[nodiscard]] uint64_t change_count() const noexcept override { return changes_; }
    [[nodiscard]] bropty::CursorState cursor() const noexcept override { return m_.cursor(); }
    [[nodiscard]] const bropty::Modes& modes() const noexcept override { return modes_; }
    [[nodiscard]] const bropty::Palette& palette() const noexcept override { return m_.palette(); }
    void request_rows(int64_t first, int64_t end) const override;

    // ---- fed by the Client
    // The model applied a frame.
    void frame_applied(const ScreenModel::Effects& fx);
    // The answer to request `req` (rows of another epoch are dropped).
    void history_arrived(uint32_t req, const HistoryChunk& chunk);
    // Request `req` was refused (or its answer was malformed).
    void history_failed(uint32_t req);

    [[nodiscard]] size_t cached_rows() const noexcept { return hist_.size(); }
    [[nodiscard]] size_t requests_in_flight() const noexcept { return inflight_.size(); }

private:
    struct HistRow {
        ModelRow row;
        std::shared_ptr<const StylePool> styles;  // the chunk it came in
    };
    void drop_history();

    const ScreenModel& m_;
    FetchFn fetch_;
    std::map<int64_t, HistRow> hist_;
    // Requests out: id -> block start; blocks being fetched.
    mutable std::map<uint32_t, int64_t> inflight_;
    mutable std::set<int64_t> inflight_blocks_;
    uint64_t changes_{1};
    bropty::Modes modes_;
    uint64_t epoch_{0};
    int seen_cols_{-1};
    int seen_rows_{-1};
    bool seen_alt_{false};
    int64_t seen_top_{0};
    // The newest history row, cached while it still wraps into screen row 0:
    // its wrap flag may yet change, so it goes when screen row 0 or history do.
    int64_t provisional_{-1};
    uint64_t provisional_top_{0};
};

}  // namespace bromux
