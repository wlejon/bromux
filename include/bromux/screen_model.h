#pragma once
// ScreenModel: a client's reconstruction of one session's screen, built from
// Frame messages. It holds what a renderer reads -- rows of bropty cells
// (row(y) is a bropty::RowView, so code written for a local bropty Terminal
// renders a muxed session unchanged), the cursor, modes, title / icon / cwd,
// the palette -- plus the history row count (rows themselves are fetched on
// demand, see Client::fetch_history).
//
// Single-threaded: owned by whoever calls Client::dispatch().

#include "bromux/codec.h"
#include "bromux/protocol.h"

#include <bropty/terminal.h>

#include <cstdint>
#include <string>
#include <vector>

namespace bromux {

class ScreenModel {
public:
    ScreenModel();

    [[nodiscard]] int cols() const noexcept { return cols_; }
    [[nodiscard]] int rows() const noexcept { return int(rows_.size()); }
    [[nodiscard]] bropty::RowView row(int y) const noexcept { return rows_[size_t(y)].view(pool_); }
    [[nodiscard]] const ModelRow& model_row(int y) const noexcept { return rows_[size_t(y)]; }
    [[nodiscard]] const StylePool& styles() const noexcept { return pool_; }
    [[nodiscard]] const bropty::Style& style(uint32_t id) const noexcept { return pool_.get(id); }
    // Hyperlink of a style's `link` (1-based), or nullptr.
    [[nodiscard]] const ModelLink* hyperlink(uint32_t link) const noexcept { return pool_.link(link); }

    [[nodiscard]] const bropty::CursorState& cursor() const noexcept { return cursor_; }
    [[nodiscard]] const ModeState& modes() const noexcept { return modes_; }
    [[nodiscard]] const std::string& title() const noexcept { return title_; }
    [[nodiscard]] const std::string& icon_name() const noexcept { return icon_; }
    [[nodiscard]] const std::string& cwd() const noexcept { return cwd_; }
    [[nodiscard]] const bropty::Palette& palette() const noexcept { return palette_; }
    [[nodiscard]] uint64_t history_rows() const noexcept { return history_rows_; }

    // Version of the session state this model shows (FrameMsg::feed_seq) and
    // the last frame applied.
    [[nodiscard]] uint64_t feed_seq() const noexcept { return feed_seq_; }
    [[nodiscard]] uint64_t frame_seq() const noexcept { return frame_seq_; }

    // Damage for renderers: rows a frame changed since clear_dirty().
    [[nodiscard]] bool row_dirty(int y) const noexcept { return dirty_[size_t(y)] != 0; }
    void clear_dirty() noexcept;

    struct Effects {
        bool size{false};
        bool cursor{false};
        bool modes{false};
        bool title{false};
        bool icon_name{false};
        bool cwd{false};
        bool palette{false};
        bool history{false};
        int rows_changed{0};
        int scrolled{0};  // net rows scrolled up
    };
    // Apply one frame. False (and `err` set) on a malformed op stream; the
    // model may then be partially updated and should be discarded.
    bool apply(const FrameMsg& frame, std::string* err = nullptr, Effects* fx = nullptr);

    void reset();

private:
    void resize_blank(int cols, int rows);
    void scroll(int k);
    void maybe_compact();

    int cols_{0};
    std::vector<ModelRow> rows_;
    std::vector<uint8_t> dirty_;
    StylePool pool_;
    size_t compact_threshold_{4096};
    bropty::CursorState cursor_{};
    ModeState modes_{};
    std::string title_;
    std::string icon_;
    std::string cwd_;
    bropty::Palette palette_;
    uint64_t history_rows_{0};
    uint64_t feed_seq_{0};
    uint64_t frame_seq_{0};
};

}  // namespace bromux
