#include "bromux/screen_model.h"

#include <algorithm>

namespace bromux {

namespace {
constexpr uint64_t kMaxRows = 0xFFFF;
constexpr uint64_t kMaxCols = 0xFFFF;
}  // namespace

ScreenModel::ScreenModel() { reset(); }

void ScreenModel::reset() {
    cols_ = 0;
    rows_.clear();
    dirty_.clear();
    serials_.clear();  // next_serial_ goes on: serials are never reused
    pool_.clear();
    compact_threshold_ = 4096;
    cursor_ = bropty::CursorState{};
    modes_ = ModeState{};
    title_.clear();
    icon_.clear();
    cwd_.clear();
    pointer_.clear();
    commands_.clear();
    ++commands_version_;
    images_.reset();
    palette_ = bropty::Palette::standard();
    history_rows_ = 0;
    history_first_ = 0;
    epoch_ = 0;
    feed_seq_ = 0;
    frame_seq_ = 0;
}

void ScreenModel::clear_dirty() noexcept { std::fill(dirty_.begin(), dirty_.end(), uint8_t(0)); }

void ScreenModel::resize_blank(int cols, int rows) {
    cols_ = cols;
    rows_.resize(size_t(rows));
    for (ModelRow& r : rows_) r.reset(cols);
    dirty_.assign(size_t(rows), uint8_t(1));
    serials_.resize(size_t(rows));
    for (uint64_t& s : serials_) s = ++next_serial_;
}

void ScreenModel::scroll(int k) {
    const int n = rows();
    if (k == 0 || n == 0) return;
    auto blank = [this](int y) {
        rows_[size_t(y)].reset(cols_);
        serials_[size_t(y)] = ++next_serial_;
    };
    if (k >= n || -k >= n) {
        for (int y = 0; y < n; ++y) blank(y);
    } else if (k > 0) {
        std::rotate(rows_.begin(), rows_.begin() + k, rows_.end());
        std::rotate(serials_.begin(), serials_.begin() + k, serials_.end());
        for (int y = n - k; y < n; ++y) blank(y);
    } else {
        std::rotate(rows_.begin(), rows_.end() + k, rows_.end());
        std::rotate(serials_.begin(), serials_.end() + k, serials_.end());
        for (int y = 0; y < -k; ++y) blank(y);
    }
    std::fill(dirty_.begin(), dirty_.end(), uint8_t(1));
}

bool ScreenModel::apply(const FrameMsg& frame, std::string* err, Effects* fx) {
    Effects local;
    Effects& e = fx ? *fx : local;
    e = Effects{};
    auto bad = [err](const char* what) {
        if (err) *err = what;
        return false;
    };
    wire::Reader r(frame.ops);
    while (!r.at_end()) {
        const uint8_t op = r.u8();
        switch (op) {
        case Op_Size: {
            const int cols = int(r.varint_max(kMaxCols));
            const int rows = int(r.varint_max(kMaxRows));
            if (!r.ok()) return bad("bad size op");
            resize_blank(cols, rows);
            e.size = true;
            break;
        }
        case Op_Scroll: {
            const int64_t k = r.svarint();
            if (!r.ok() || k < -int64_t(kMaxRows) || k > int64_t(kMaxRows)) return bad("bad scroll op");
            scroll(int(k));
            e.scrolled += int(k);
            break;
        }
        case Op_Row: {
            const uint64_t y = r.varint();
            if (!r.ok() || y >= rows_.size()) return bad("row op outside the screen");
            ModelRow& row = rows_[size_t(y)];
            if (!decode_row(r, row, pool_)) return bad("bad row encoding");
            if (int(row.cells.size()) != cols_) return bad("row width differs from the screen");
            dirty_[size_t(y)] = 1;
            serials_[size_t(y)] = ++next_serial_;
            ++e.rows_changed;
            break;
        }
        case Op_Cursor: {
            cursor_.row = int(r.varint_max(kMaxRows));
            cursor_.col = int(r.varint_max(kMaxCols));
            const uint8_t f = r.u8();
            const uint8_t shape = r.u8();
            if (!r.ok() || shape > uint8_t(bropty::CursorShape::Bar)) return bad("bad cursor op");
            cursor_.visible = (f & 1) != 0;
            cursor_.pending_wrap = (f & 2) != 0;
            cursor_.blink = (f & 4) != 0;
            cursor_.shape = bropty::CursorShape(shape);
            e.cursor = true;
            break;
        }
        case Op_Modes: {
            modes_.bits = r.varint();
            const uint8_t tracking = r.u8();
            const uint8_t encoding = r.u8();
            modes_.kitty_flags = uint32_t(r.varint_max(UINT32_MAX));
            if (!r.ok() || tracking > uint8_t(bropty::MouseTracking::Any) ||
                encoding > uint8_t(bropty::MouseEncoding::SgrPixels))
                return bad("bad modes op");
            modes_.mouse_tracking = bropty::MouseTracking(tracking);
            modes_.mouse_encoding = bropty::MouseEncoding(encoding);
            e.modes = true;
            break;
        }
        case Op_Text: {
            const uint8_t which = r.u8();
            std::string v = r.str();
            if (!r.ok() || which > 3) return bad("bad text op");
            if (which == 0) {
                title_ = std::move(v);
                e.title = true;
            } else if (which == 1) {
                icon_ = std::move(v);
                e.icon_name = true;
            } else if (which == 2) {
                cwd_ = std::move(v);
                e.cwd = true;
            } else {
                pointer_ = std::move(v);
                e.pointer_shape = true;
            }
            break;
        }
        case Op_Commands: {
            const uint64_t drop = r.varint();
            const uint64_t keep = r.varint();
            const uint64_t n = r.varint_max(std::min<uint64_t>(r.remaining(), bropty::Terminal::kMaxCommands));
            if (!r.ok()) return bad("bad commands op");
            commands_.erase(commands_.begin(), commands_.begin() + ptrdiff_t(std::min<uint64_t>(drop, commands_.size())));
            if (keep < commands_.size()) commands_.resize(size_t(keep));
            for (uint64_t i = 0; i < n; ++i) {
                bropty::CommandRecord c;
                if (!read_command(r, c)) return bad("bad command record");
                commands_.push_back(std::move(c));
            }
            if (commands_.size() > bropty::Terminal::kMaxCommands) return bad("too many command records");
            ++commands_version_;
            e.commands = true;
            break;
        }
        case Op_Images:
            if (!images_.apply_images(r)) return bad("bad images op");
            break;
        case Op_ImageData:
            if (!images_.apply_data(r)) return bad("bad image data op");
            break;
        case Op_Palette:
            if (!read_palette(r, palette_)) return bad("bad palette op");
            e.palette = true;
            break;
        case Op_History: {
            const uint64_t first = r.varint_max(uint64_t(INT64_MAX) / 2);
            const uint64_t n = r.varint_max(uint64_t(INT64_MAX) / 2);
            const uint64_t epoch = r.varint();
            if (!r.ok()) return bad("bad history op");
            history_first_ = int64_t(first);
            history_rows_ = n;
            e.epoch = epoch != epoch_;
            epoch_ = epoch;
            e.history = true;
            break;
        }
        default:
            return bad("unknown frame op");
        }
    }
    if (!r.ok()) return bad("truncated frame");
    if (cursor_.row >= rows() && rows() > 0) return bad("cursor outside the screen");
    e.images = images_.finish();
    feed_seq_ = frame.feed_seq;
    frame_seq_ = frame.frame_seq;
    maybe_compact();
    return true;
}

void ScreenModel::maybe_compact() {
    if (pool_.size() <= compact_threshold_) return;
    StylePool fresh;
    std::vector<uint32_t> remap(pool_.size(), UINT32_MAX);
    for (ModelRow& row : rows_) {
        for (bropty::Cell& c : row.cells) {
            uint32_t& m = remap[c.style];
            if (m == UINT32_MAX) {
                const bropty::Style& s = pool_.get(c.style);
                m = fresh.intern(s, pool_.link(s.link));
            }
            c.style = m;
        }
    }
    pool_ = std::move(fresh);
    compact_threshold_ = std::max<size_t>(4096, pool_.size() * 4);
}

}  // namespace bromux
