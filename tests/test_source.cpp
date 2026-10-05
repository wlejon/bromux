// ScreenSource in process: a ScreenModel fed full frames from a bropty
// Terminal, history answered from that terminal as the server answers
// FetchHistory. bropty's TerminalView, selection, search and links over the
// source must answer as they do over the terminal itself; then the parts
// only a client has: rows that arrive later and in blocks, answers from an
// older numbering, eviction, the newest history row's wrap flag, refusals.
#include "check.h"
#include "frames.h"

#include <bromux/screen_source.h>
#include <bropty/view.h>

#include <chrono>
#include <random>
#include <thread>

using namespace bromux;
using bropty::RowPos;
using bropty::RowRange;
using bropty::TerminalView;

namespace {

// One session as a client sees it, with the server in process.
struct Remote {
    bropty::Terminal t;
    ScreenModel model;
    std::unique_ptr<ScreenSource> src;
    struct Req {
        uint32_t id;
        int64_t start;
        uint32_t count;
    };
    std::vector<Req> reqs;
    uint32_t next_id{1};
    uint64_t seq{0};
    frames::Sent sent;
    bool refuse{false};  // fetch returns 0 (as when disconnected)

    static bropty::TerminalOptions opts(int cols, int rows, size_t scrollback) {
        bropty::TerminalOptions o;
        o.cols = cols;
        o.rows = rows;
        o.scrollback_rows = scrollback;
        return o;
    }
    Remote(int cols, int rows, size_t scrollback) : t(opts(cols, rows, scrollback)) {
        push();
        src = std::make_unique<ScreenSource>(model, [this](int64_t start, uint32_t count) -> uint32_t {
            if (refuse) return 0;
            reqs.push_back(Req{next_id, start, count});
            return next_id++;
        });
    }
    void feed(std::string_view s) { t.feed(s); }
    // The server sends a frame: the rows that changed.
    void push() {
        ScreenModel::Effects fx;
        std::string err;
        CHECK_MSG(model.apply(frames::frame(t, ++seq, &sent), &err, &fx), err);
        if (src) src->frame_applied(fx);
    }
    HistoryChunk read(const Req& r) const {
        HistoryChunk c;
        CHECK(decode_history(frames::history(t, uint64_t(r.start), r.count), c));
        return c;
    }
    // Answer every request out.
    size_t answer() {
        const std::vector<Req> rs = std::move(reqs);
        reqs.clear();
        for (const Req& r : rs) src->history_arrived(r.id, read(r));
        return rs.size();
    }
    void fetch_all() {
        src->request_rows(src->first_row(), src->screen_top_row());
        answer();
    }
};

struct Rng {
    std::mt19937 g;
    explicit Rng(uint32_t seed) : g(seed) {}
    int below(int n) { return int(g() % uint32_t(n)); }
};

const char* kWords[] = {"alpha", "beta", "gamma", "http://ex.am/ple?q=1", "/usr/lib/x.so:12", "src/main.cpp:4:2",
                        "naïve", "中文字", "e\xCC\x81t\xC3\xA9", "x", "foo-bar_baz", "ALPHA"};

std::string random_output(Rng& r, int lines) {
    std::string s;
    for (int i = 0; i < lines; ++i) {
        const int k = r.below(12);
        if (k == 0) s += "\x1b]133;A\x07$ \x1b]133;B\x07" "cmd\r\n\x1b]133;C\x07";
        if (k == 1)
            s += "\x1b]8;id=" + std::to_string(r.below(3)) + ";https://link/" + std::to_string(r.below(4)) +
                 "\x1b\\linked text\x1b]8;;\x1b\\ ";
        if (k == 2) s += "\x1b[3" + std::to_string(r.below(8)) + "m";
        if (k == 3) s += "\x1b[0m";
        const int n = 1 + r.below(k == 4 ? 14 : 6);  // sometimes long enough to wrap
        for (int w = 0; w < n; ++w) {
            s += kWords[r.below(int(sizeof kWords / sizeof *kWords))];
            s += ' ';
        }
        if (k == 5) s += "\x1b]133;D\x07";
        s += "\r\n";
    }
    return s;
}

std::string row_text(const bropty::FrameRow& r) { return r.view().text(false) + "|" + std::to_string(r.flags); }

bool same_link(const std::optional<bropty::LinkHit>& a, const std::optional<bropty::LinkHit>& b) {
    if (a.has_value() != b.has_value()) return false;
    return !a || (a->range == b->range && a->kind == b->kind && a->target == b->target);
}

void run_search(bropty::Search& s) {
    for (int i = 0; i < 100000 && s.step(); ++i) {
    }
}

std::vector<RowRange> matches(const bropty::Search& s) {
    std::vector<RowRange> v;
    for (size_t i = 0; i < s.size(); ++i) v.push_back(s.at(i));
    return v;
}

// The source, once it holds its history, answers as the terminal does.
void same_answers(uint32_t seed) {
    Rng r(seed);
    Remote rm(24 + r.below(20), 6 + r.below(6), 60 + size_t(r.below(80)));
    TerminalView tv(rm.t);
    TerminalView mv(*rm.src);
    for (int round = 0; round < 10; ++round) {
        rm.feed(random_output(r, 2 + r.below(15)));
        if (r.below(9) == 0) {
            rm.feed("\x1b[?1049h" + random_output(r, 3));
            if (r.below(2)) rm.feed("\x1b[?1049l");
        }
        if (r.below(6) == 0) rm.t.resize(20 + r.below(30), 5 + r.below(6));
        rm.push();
        rm.fetch_all();
        tv.sync();
        mv.sync();
        const int64_t lo = rm.t.first_row(), hi = rm.t.end_row();
        CHECK_EQ(rm.src->first_row(), lo);
        CHECK_EQ(rm.src->end_row(), hi);
        CHECK_EQ(rm.src->alt_screen_active(), rm.t.alt_screen_active());
        CHECK_EQ(rm.src->modes().autowrap, rm.t.modes().autowrap);
        CHECK_EQ(rm.src->modes().bracketed_paste, rm.t.modes().bracketed_paste);
        auto cell = [&] { return RowPos{lo + r.below(int(hi - lo)), r.below(rm.t.cols())}; };
        for (int g = 0; g < 6; ++g) {
            const auto mode = bropty::SelectionMode(r.below(5));
            const RowPos a = cell(), b = cell();
            tv.selection().start(a, mode);
            mv.selection().start(a, mode);
            tv.selection().extend(b);
            mv.selection().extend(b);
            CHECK(mv.selection().range() == tv.selection().range());
            CHECK_EQ(mv.selection().text(), tv.selection().text());
            CHECK_EQ(mv.selection().html(rm.t.palette()), tv.selection().html(rm.t.palette()));
        }
        tv.selection().select_all();
        mv.selection().select_all();
        CHECK_EQ(mv.selection().text(), tv.selection().text());
        for (int k = 0; k < 30; ++k) {
            const RowPos c = cell();
            CHECK(same_link(bropty::link_at(*rm.src, c), bropty::link_at(rm.t, c)));
        }
        const char* needle = kWords[r.below(int(sizeof kWords / sizeof *kWords))];
        tv.search().start(std::make_shared<bropty::LiteralMatcher>(needle, false));
        mv.search().start(std::make_shared<bropty::LiteralMatcher>(needle, false));
        run_search(tv.search());
        run_search(mv.search());
        CHECK(mv.search().complete());
        CHECK(matches(mv.search()) == matches(tv.search()));
        const int64_t back = -r.below(30);
        tv.scroll_to_bottom();
        mv.scroll_to_bottom();
        tv.scroll_by(back);
        mv.scroll_by(back);
        CHECK_EQ(mv.top_row(), tv.top_row());
        auto fa = tv.snapshot();
        auto fb = mv.snapshot();
        CHECK_EQ(fb->top_row, fa->top_row);
        CHECK_EQ(fb->rows, fa->rows);
        for (int y = 0; y < std::min(fa->rows, fb->rows); ++y)
            CHECK_EQ(row_text(*fb->lines[size_t(y)]), row_text(*fa->lines[size_t(y)]));
    }
    CHECK_EQ(rm.src->requests_in_flight(), size_t(0));
}

// History comes later, in aligned blocks, each asked for once; a frame shows
// it blank meanwhile, a search waits for it.
void rows_arrive_later() {
    check::phase("rows arrive later");
    Remote rm(30, 6, 5000);
    std::string out;
    for (int i = 0; i < 1000; ++i) out += "line " + std::to_string(i) + (i % 100 == 7 ? " needle" : "") + "\r\n";
    rm.feed(out);
    rm.push();
    TerminalView mv(*rm.src), tv(rm.t);
    CHECK_EQ(rm.src->cached_rows(), size_t(0));
    mv.scroll_by(-300);
    tv.scroll_by(-300);
    auto blank = mv.snapshot();
    CHECK_EQ(blank->lines[0]->view().text(), std::string(""));
    CHECK(!rm.reqs.empty());
    for (const Remote::Req& q : rm.reqs) {
        CHECK_EQ(q.start % int64_t(ScreenSource::kFetchRows), int64_t(0));
        CHECK(q.count <= ScreenSource::kFetchRows);
    }
    const size_t asked = rm.reqs.size();
    (void)mv.snapshot();  // the next frame asks for nothing new while those are out
    CHECK_EQ(rm.reqs.size(), asked);
    CHECK_EQ(rm.src->requests_in_flight(), asked);
    const uint64_t before = rm.src->change_count();
    rm.answer();
    CHECK(rm.src->change_count() != before);
    auto full = mv.snapshot();
    auto want = tv.snapshot();
    for (int y = 0; y < 6; ++y) CHECK_EQ(row_text(*full->lines[size_t(y)]), row_text(*want->lines[size_t(y)]));

    // A search waits for each block in turn and finds every match.
    mv.search().start(std::make_shared<bropty::LiteralMatcher>("needle"));
    int rounds = 0;
    while (!mv.search().complete() && rounds++ < 100) {
        run_search(mv.search());
        rm.answer();
    }
    CHECK(mv.search().complete());
    CHECK_EQ(mv.search().size(), size_t(10));
    CHECK(rounds > 1);  // it had to wait

    // Refused: the block is asked for again once a backoff has passed, not by
    // every frame, and the backoff doubles with each refusal.
    Remote other(30, 6, 5000);
    other.feed(out);
    other.push();
    TerminalView ov(*other.src);
    ov.scroll_by(-500);
    (void)ov.snapshot();
    CHECK(!other.reqs.empty());
    std::vector<Remote::Req> refused = std::move(other.reqs);
    other.reqs.clear();
    other.src->history_failed(99999);  // an unknown id: ignored
    CHECK_EQ(other.src->requests_in_flight(), refused.size());
    for (const Remote::Req& q : refused) other.src->history_failed(q.id);
    CHECK_EQ(other.src->requests_in_flight(), size_t(0));
    using namespace std::chrono_literals;
    const auto first_delay = ScreenSource::kRetryDelay;
    for (int i = 0; i < 20; ++i) (void)ov.snapshot();
    CHECK(other.reqs.empty());
    std::this_thread::sleep_for(first_delay + 50ms);
    (void)ov.snapshot();
    CHECK_EQ(other.reqs.size(), refused.size());
    refused = std::move(other.reqs);
    other.reqs.clear();
    for (const Remote::Req& q : refused) other.src->history_failed(q.id);
    std::this_thread::sleep_for(first_delay + 50ms);  // past the first delay, short of the doubled one
    (void)ov.snapshot();
    CHECK(other.reqs.empty());
    std::this_thread::sleep_for(first_delay + 50ms);
    (void)ov.snapshot();
    CHECK_EQ(other.reqs.size(), refused.size());
    // Answered at last: those rows are held and nothing more is asked.
    other.answer();
    CHECK_EQ(other.src->requests_in_flight(), size_t(0));
    (void)ov.snapshot();
    CHECK(other.reqs.empty());
    ov.scroll_by(-1000);  // rows further back are new blocks, asked for at once
    (void)ov.snapshot();
    CHECK(!other.reqs.empty());
    // Not connected (the fetch cannot be sent): nothing is left waiting.
    refused = std::move(other.reqs);
    other.reqs.clear();
    for (const Remote::Req& q : refused) other.src->history_failed(q.id);
    other.refuse = true;
    (void)ov.snapshot();
    CHECK(other.reqs.empty());
    CHECK_EQ(other.src->requests_in_flight(), size_t(0));
}

// Answers from another numbering are dropped; a resize drops the cache and
// reaches observers; rows evicted from the server leave the cache.
void epochs_and_eviction() {
    check::phase("epochs and eviction");
    Remote rm(20, 5, 100);
    for (int i = 0; i < 150; ++i) rm.feed("row " + std::to_string(i) + " word\r\n");
    rm.push();
    TerminalView mv(*rm.src);
    rm.src->request_rows(rm.src->first_row(), rm.src->screen_top_row());
    std::vector<Remote::Req> old = rm.reqs;
    rm.reqs.clear();
    std::vector<HistoryChunk> stale;
    for (const Remote::Req& q : old) stale.push_back(rm.read(q));
    mv.selection().select_all();
    mv.search().start(std::make_shared<bropty::LiteralMatcher>("word"));
    mv.scroll_by(-10);
    CHECK(mv.selection().active());
    const uint64_t epoch = rm.model.history_epoch();
    rm.t.resize(13, 5);
    rm.push();
    CHECK(rm.model.history_epoch() != epoch);
    CHECK(!mv.selection().active());  // positions cannot be carried
    CHECK(mv.at_bottom());
    CHECK_EQ(rm.src->requests_in_flight(), size_t(0));
    for (size_t i = 0; i < old.size(); ++i) rm.src->history_arrived(old[i].id, stale[i]);
    CHECK_EQ(rm.src->cached_rows(), size_t(0));  // read in the old numbering
    rm.fetch_all();
    CHECK(rm.src->cached_rows() > 0);
    run_search(mv.search());
    while (!mv.search().complete()) {
        rm.answer();
        run_search(mv.search());
    }
    TerminalView tv(rm.t);
    tv.search().start(std::make_shared<bropty::LiteralMatcher>("word"));
    run_search(tv.search());
    CHECK(matches(mv.search()) == matches(tv.search()));

    // Eviction: the server's history moves on; rows before it leave the cache.
    const int64_t first = rm.src->first_row();
    for (int i = 0; i < 60; ++i) rm.feed("more " + std::to_string(i) + "\r\n");
    rm.push();
    CHECK(rm.src->first_row() > first);
    CHECK(rm.src->row_at(first).cells == nullptr);
    rm.fetch_all();
    CHECK_EQ(rm.src->cached_rows(), size_t(rm.src->screen_top_row() - rm.src->first_row()));
    for (int64_t a = rm.src->first_row(); a < rm.src->screen_top_row(); ++a)
        CHECK_EQ(rm.src->row_at(a).text(), rm.t.row_at(a).text());
    // The alternate screen shows no history, and asks for none.
    rm.feed("\x1b[?1049h");
    rm.push();
    CHECK_EQ(rm.src->first_row(), rm.src->screen_top_row());
    rm.src->request_rows(0, rm.src->screen_top_row());
    CHECK(rm.reqs.empty());
}

// The newest history row, while its line continues on screen row 0, is held
// only until screen row 0 or history change; others stay.
void newest_row_provisional() {
    check::phase("newest row");
    Remote rm(10, 3, 100);
    rm.feed("aaaaaaaaaabbbbbbbbbbccccccccccdddddddddd");  // one line over four rows
    rm.push();
    CHECK_EQ(rm.model.history_rows(), uint64_t(1));
    rm.fetch_all();
    const int64_t newest = rm.src->screen_top_row() - 1;
    CHECK(rm.src->row_at(newest).wrapped());
    // A frame that leaves screen row 0 alone keeps it.
    rm.feed("\x1b[3;1Hz");
    rm.push();
    CHECK(rm.src->row_at(newest).cells != nullptr);
    // Screen row 0 rewritten: the row is dropped, then fetched as it is now.
    rm.feed("\x1b[1;1H\x1b[2K");
    rm.push();
    CHECK(rm.src->row_at(newest).cells == nullptr);
    rm.fetch_all();
    CHECK_EQ(rm.src->row_at(newest).wrapped(), rm.t.row_at(newest).wrapped());
    CHECK_EQ(rm.src->row_at(newest).text(), rm.t.row_at(newest).text());
    // Once more output pushes it deeper, it is final and stays.
    rm.feed("\x1b[3;1H\r\nx\r\ny\r\nz\r\n");
    rm.push();
    rm.fetch_all();
    const size_t held = rm.src->cached_rows();
    rm.feed("\x1b[1;1Hq");
    rm.push();
    CHECK(rm.src->cached_rows() + 1 >= held);  // at most the (new) newest row goes
    for (int64_t a = rm.src->first_row(); a < rm.src->screen_top_row() - 1; ++a)
        CHECK(rm.src->row_at(a).cells != nullptr);
}

// Screen rows carry the model's serials: a view re-reads only rows a frame
// wrote, and a scroll op moves serials with their rows.
void serials() {
    check::phase("serials");
    ScreenModel m;
    bropty::Terminal t(10, 4);
    t.feed("a\r\nb\r\nc\r\nd");
    CHECK(m.apply(frames::full_frame(t, 1)));
    std::vector<uint64_t> s0;
    for (int y = 0; y < 4; ++y) s0.push_back(m.row_serial(y));
    FrameMsg f;
    f.frame_seq = f.feed_seq = 2;
    wire::Writer w(f.ops);
    w.u8(Op_Scroll);
    w.svarint(1);
    t.feed("\r\ne");
    w.u8(Op_Row);
    w.varint(3);
    RowEncoder enc;
    enc.encode(f.ops, t.row(3), t);
    CHECK(m.apply(f));
    for (int y = 0; y < 3; ++y) CHECK_EQ(m.row_serial(y), s0[size_t(y) + 1]);
    for (uint64_t s : s0) CHECK(m.row_serial(3) != s);
    ScreenSource src(m, nullptr);
    CHECK_EQ(src.row_serial(src.screen_top_row() + 2), m.row_serial(2));
    CHECK_EQ(src.row_serial(src.screen_top_row() - 1), uint64_t(0));
}

}  // namespace

int main() {
    check::start_watchdog("test_source");
    check::phase("same answers", 120);
    for (uint32_t seed = 1; seed <= 10; ++seed) same_answers(seed);
    rows_arrive_later();
    epochs_and_eviction();
    newest_row_provisional();
    serials();
    return check::finish("test_source");
}
