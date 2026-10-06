#pragma once
// ScreenModel: a client's reconstruction of one session's screen, built from
// Frame messages. It holds what a renderer reads -- rows of bropty cells
// (row(y) is a bropty::RowView, so code written for a local bropty Terminal
// renders a muxed session unchanged), the cursor, modes, title / icon / cwd,
// the palette -- plus where history is in absolute row numbers (rows
// themselves are fetched on demand: Client::fetch_history, or a
// ScreenSource, which serves this model with its history to bropty's
// selection, search, links and TerminalView).
//
// Single-threaded: owned by whoever calls Client::dispatch().

#include "bromux/codec.h"
#include "bromux/image_codec.h"
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
    [[nodiscard]] bool alt_screen_active() const noexcept { return modes_.has(Mode_AltScreen); }
    // ---- minor 1 (empty from an older server) ----
    // The pointer shape the program asked for (OSC 22), "" for none.
    [[nodiscard]] const std::string& pointer_shape() const noexcept { return pointer_; }
    // The OSC 133 command records, oldest first, positions in absolute rows.
    [[nodiscard]] const std::vector<bropty::CommandRecord>& commands() const noexcept { return commands_; }
    // Changes whenever commands() does.
    [[nodiscard]] uint64_t commands_version() const noexcept { return commands_version_; }
    // The active screen's inline images.
    [[nodiscard]] const ImageMirror& images() const noexcept { return images_; }
    // History (Op_History): absolute rows history_first_row() ..
    // screen_top_row() - 1; the screen's rows follow. Numbers hold while
    // history_epoch() stays the same (bropty::Terminal::row_numbering()).
    [[nodiscard]] uint64_t history_rows() const noexcept { return history_rows_; }
    [[nodiscard]] int64_t history_first_row() const noexcept { return history_first_; }
    [[nodiscard]] int64_t screen_top_row() const noexcept { return history_first_ + int64_t(history_rows_); }
    [[nodiscard]] uint64_t history_epoch() const noexcept { return epoch_; }

    // A content serial per screen row: a new one whenever a frame writes the
    // row (or a resize / scroll blanks it); it moves with the row when the
    // screen scrolls. Equal serials mean equal content, for this model's life.
    [[nodiscard]] uint64_t row_serial(int y) const noexcept { return serials_[size_t(y)]; }

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
        bool epoch{false};  // rows were renumbered (a resize's reflow)
        bool pointer_shape{false};
        bool commands{false};
        bool images{false};  // what images() shows changed
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
    std::vector<uint64_t> serials_;
    uint64_t next_serial_{0};
    StylePool pool_;
    size_t compact_threshold_{4096};
    bropty::CursorState cursor_{};
    ModeState modes_{};
    std::string title_;
    std::string icon_;
    std::string cwd_;
    std::string pointer_;
    std::vector<bropty::CommandRecord> commands_;
    uint64_t commands_version_{0};
    ImageMirror images_;
    bropty::Palette palette_;
    uint64_t history_rows_{0};
    int64_t history_first_{0};
    uint64_t epoch_{0};
    uint64_t feed_seq_{0};
    uint64_t frame_seq_{0};
};

}  // namespace bromux
