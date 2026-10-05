// Row codec and ScreenModel, in process: random terminals (vtgen) are
// encoded row by row and as full frames, decoded, and compared with the
// terminal through the oracle; history rows likewise. Malformed op streams
// and rows must be rejected.
#include "check.h"
#include "frames.h"
#include "oracle.h"
#include "vtgen.h"

#include <bromux/codec.h>
#include <bromux/screen_model.h>

using namespace bromux;
using frames::full_frame;
using frames::write_history;

namespace {

void test_random_terminals() {
    check::phase("random terminals", 300);
    int compared = 0;
    for (uint64_t seed = 1; seed <= 200; ++seed) {
        vtgen::Rng r(seed);
        const int cols = r.range(2, 140);
        const int rows = r.range(1, 50);
        bropty::TerminalOptions o;
        o.cols = cols;
        o.rows = rows;
        o.scrollback_rows = 300;
        bropty::Terminal t(o);
        ScreenModel m;
        uint64_t seq = 0;
        for (int step = 0; step < 6; ++step) {
            t.feed(vtgen::stream(seed * 100 + uint64_t(step), 150, cols, rows));
            if (r.chance(30)) t.resize(r.range(2, 140), r.range(1, 50));
            FrameMsg f = full_frame(t, ++seq);
            std::string err;
            CHECK_MSG(m.apply(f, &err), err);
            std::string d = oracle::compare_screen(m, t);
            CHECK_MSG(d.empty(), "seed " + std::to_string(seed) + " step " + std::to_string(step) + ": " + d);
            ++compared;
        }
        // History rows survive the codec too.
        RowEncoder enc;
        HistoryChunk h;
        h.first_row = t.history_first_row();
        h.history_rows = t.history_rows();
        h.epoch = t.row_numbering();
        h.start = h.first_row;
        h.rows.resize(t.history_rows());
        for (size_t i = 0; i < t.history_rows(); ++i) {
            std::string bytes;
            enc.encode(bytes, t.history_row(i), t);
            wire::Reader rr(bytes);
            CHECK(decode_row(rr, h.rows[i], h.styles));
            CHECK(rr.done());
        }
        std::string d = oracle::compare_history(h, t);
        CHECK_MSG(d.empty(), "seed " + std::to_string(seed) + ": " + d);
    }
    std::printf("   %d screens compared\n", compared);
}

void test_scroll_op() {
    check::phase("scroll op");
    bropty::Terminal t(10, 5);
    for (int i = 0; i < 5; ++i) t.feed("line" + std::to_string(i) + (i < 4 ? "\r\n" : ""));
    ScreenModel m;
    CHECK(m.apply(full_frame(t, 1)));
    // Scroll up 2 by op, then rewrite the two vacated rows.
    t.feed("\r\nline5\r\nline6");
    FrameMsg f;
    f.frame_seq = 2;
    f.feed_seq = 2;
    wire::Writer w(f.ops);
    w.u8(Op_Scroll);
    w.svarint(2);
    RowEncoder enc;
    for (int y = 3; y < 5; ++y) {
        w.u8(Op_Row);
        w.varint(uint64_t(y));
        enc.encode(f.ops, t.row(y), t);
    }
    w.u8(Op_Cursor);
    w.varint(4);
    w.varint(5);
    w.u8(1 | 4);
    w.u8(0);
    write_history(w, t);
    ScreenModel::Effects fx;
    CHECK(m.apply(f, nullptr, &fx));
    CHECK_EQ(fx.scrolled, 2);
    CHECK_EQ(fx.rows_changed, 2);
    std::string d = oracle::compare_screen(m, t);
    CHECK_MSG(d.empty(), d);
}

void test_malformed() {
    check::phase("malformed");
    ScreenModel m;
    auto rejects = [&](std::string ops) {
        FrameMsg f;
        f.ops = std::move(ops);
        ScreenModel fresh;
        std::string err;
        return !fresh.apply(f, &err) && !err.empty();
    };
    std::string row_before_size;
    wire::Writer w(row_before_size);
    w.u8(Op_Row);
    w.varint(0);
    w.raw(encode_blank_row(10));
    CHECK(rejects(row_before_size));                 // no screen yet
    CHECK(rejects(std::string("\x63", 1)));          // unknown op
    CHECK(rejects(std::string("\x01\x05", 2)));      // truncated size
    std::string wrong_width;
    wire::Writer w2(wrong_width);
    w2.u8(Op_Size);
    w2.varint(10);
    w2.varint(2);
    w2.u8(Op_Row);
    w2.varint(1);
    w2.raw(encode_blank_row(11));
    CHECK(rejects(wrong_width));
    // Random op streams after a valid size: never crash.
    vtgen::Rng rng(5);
    for (int i = 0; i < 20000; ++i) {
        std::string ops;
        wire::Writer ww(ops);
        ww.u8(Op_Size);
        ww.varint(uint64_t(rng.range(0, 20)));
        ww.varint(uint64_t(rng.range(0, 10)));
        int n = rng.range(0, 40);
        for (int k = 0; k < n; ++k) ops.push_back(char(rng.range(0, 255)));
        FrameMsg f;
        f.ops = ops;
        ScreenModel s;
        (void)s.apply(f);
    }
    CHECK(true);
}

}  // namespace

int main() {
    check::start_watchdog("test_codec");
    test_random_terminals();
    test_scroll_op();
    test_malformed();
    return check::finish("test_codec");
}
