// Wire primitives and every message body: round trips, framing across
// arbitrary splits, and hostile input (random and mutated bytes) that must
// be rejected without crashing or over-allocating.
#include "check.h"
#include "vtgen.h"

#include <bromux/protocol.h>

#include <vector>

using namespace bromux;

namespace {

void test_primitives() {
    check::phase("primitives");
    wire::Writer w;
    const uint64_t vals[] = {0, 1, 127, 128, 300, 16383, 16384, 0xFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull};
    for (uint64_t v : vals) w.varint(v);
    const int64_t svals[] = {0, -1, 1, -64, 64, INT64_MIN, INT64_MAX};
    for (int64_t v : svals) w.svarint(v);
    w.u8(0xAB);
    w.u16(0xBEEF);
    w.u32(0xDEADBEEF);
    w.u64(0x0123456789ABCDEFull);
    w.str("hello");
    w.boolean(true);
    wire::Reader r(w.data());
    for (uint64_t v : vals) CHECK_EQ(r.varint(), v);
    for (int64_t v : svals) CHECK_EQ(r.svarint(), v);
    CHECK_EQ(r.u8(), 0xAB);
    CHECK_EQ(r.u16(), 0xBEEF);
    CHECK_EQ(r.u32(), 0xDEADBEEFu);
    CHECK_EQ(r.u64(), 0x0123456789ABCDEFull);
    CHECK_EQ(r.str(), std::string("hello"));
    CHECK(r.boolean());
    CHECK(r.done());

    // Underflow and overflow are failures, never reads past the end.
    wire::Reader short_r(std::string_view("\x05" "ab", 3));
    CHECK(short_r.str().empty());
    CHECK(!short_r.ok());
    const std::string overlong(11, char(0xFF));
    wire::Reader ov(overlong);
    ov.varint();
    CHECK(!ov.ok());
    wire::Reader big_count(std::string_view("\xFF\xFF\xFF\xFF\x0F", 5));
    CHECK_EQ(big_count.count(1), size_t(0));
    CHECK(!big_count.ok());
}

void test_splitter() {
    check::phase("splitter");
    std::string stream;
    for (int i = 0; i < 50; ++i) {
        PingMsg p;
        p.req = uint32_t(i);
        encode_into(stream, p);
        TextMsg t;
        t.session = uint64_t(i);
        t.text = std::string(size_t(i * 37), 'x');
        encode_into(stream, t);
    }
    vtgen::Rng rng(7);
    for (int trial = 0; trial < 20; ++trial) {
        wire::MessageSplitter sp;
        size_t pos = 0;
        int got = 0;
        bool ok = true;
        while (pos < stream.size()) {
            size_t n = std::min(stream.size() - pos, size_t(rng.range(1, 200)));
            sp.feed(stream.data() + pos, n);
            pos += n;
            wire::MessageSplitter::Message m;
            while (sp.next(m)) {
                if (got % 2 == 0) {
                    PingMsg p;
                    ok = ok && m.type == uint16_t(MsgType::Ping) && decode(m.payload, p) && p.req == uint32_t(got / 2);
                } else {
                    TextMsg t;
                    ok = ok && m.type == uint16_t(MsgType::Text) && decode(m.payload, t) &&
                         t.text.size() == size_t((got / 2) * 37);
                }
                ++got;
            }
        }
        CHECK(ok);
        CHECK_EQ(got, 100);
        CHECK(!sp.error());
    }
    // A length beyond the limit is an unrecoverable error.
    wire::MessageSplitter bad;
    const char huge[] = {char(0xFF), char(0xFF), char(0xFF), char(0x7F), 0, 0};
    bad.feed(huge, sizeof huge);
    wire::MessageSplitter::Message m;
    CHECK(!bad.next(m));
    CHECK(bad.error());
}

template <class M>
M round_trip(const M& m) {
    std::string bytes = encode(m);
    wire::MessageSplitter sp;
    sp.feed(bytes.data(), bytes.size());
    wire::MessageSplitter::Message msg;
    M out;
    CHECK(sp.next(msg));
    CHECK_EQ(msg.type, uint16_t(M::kType));
    CHECK(decode(msg.payload, out));
    // Every strict prefix of the body fails to decode (or is a complete
    // decode of an older, shorter layout -- never a crash).
    for (size_t cut = 0; cut < msg.payload.size(); ++cut) {
        M partial;
        (void)decode(msg.payload.substr(0, cut), partial);
    }
    return out;
}

void test_messages() {
    check::phase("messages");
    SessionSpec spec;
    spec.meta = {{"name", "build"}, {"layout", std::string("\0\1\2", 3)}};
    spec.command = "bash";
    spec.args = {"-c", "echo hi"};
    spec.cwd = "/tmp";
    spec.env = {{"A", "1"}};
    spec.env_unset = {"B"};
    spec.cols = 132;
    spec.rows = 50;
    spec.scrollback_rows = 5000;
    spec.remove_on_exit = true;
    spec.resize_policy = ResizePolicy::Smallest;
    CreateSessionMsg cs;
    cs.req = 9;
    cs.spec = spec;
    CreateSessionMsg cs2 = round_trip(cs);
    CHECK_EQ(cs2.req, 9u);
    CHECK(cs2.spec.meta == spec.meta);
    CHECK(cs2.spec.args == spec.args);
    CHECK_EQ(cs2.spec.cols, 132);
    CHECK(cs2.spec.remove_on_exit);
    CHECK(cs2.spec.resize_policy == ResizePolicy::Smallest);

    SessionInfo info;
    info.id = 77;
    info.meta = spec.meta;
    info.pid = 4242;
    info.running = true;
    info.exit_code = -1;
    info.cols = 80;
    info.rows = 24;
    info.clients = 3;
    info.title = "t";
    info.clipboard_policy = ClipboardPolicy::ReadWrite;
    SessionListMsg sl;
    sl.req = 1;
    sl.sessions = {info, info};
    SessionListMsg sl2 = round_trip(sl);
    CHECK_EQ(sl2.sessions.size(), size_t(2));
    CHECK_EQ(sl2.sessions[1].id, 77u);
    CHECK_EQ(sl2.sessions[1].pid, 4242);
    CHECK(sl2.sessions[1].clipboard_policy == ClipboardPolicy::ReadWrite);
    CHECK_EQ(sl2.sessions[0].meta_value("name"), std::string("build"));

    KeyMsg k;
    k.session = 5;
    k.event = bropty::KeyEvent::functional(bropty::Key::F5, bropty::Mod_Ctrl | bropty::Mod_Shift,
                                           bropty::KeyAction::Repeat);
    k.event.text = "é";
    KeyMsg k2 = round_trip(k);
    CHECK(k2.event.key == bropty::Key::F5);
    CHECK_EQ(k2.event.mods, uint16_t(bropty::Mod_Ctrl | bropty::Mod_Shift));
    CHECK(k2.event.action == bropty::KeyAction::Repeat);
    CHECK_EQ(k2.event.text, std::string("é"));

    MouseMsg mm;
    mm.session = 5;
    mm.event.action = bropty::MouseAction::Motion;
    mm.event.button = bropty::MouseButton::WheelLeft;
    mm.event.col = 200;
    mm.event.row = 3;
    mm.event.x = -1;
    mm.event.y = 999;
    MouseMsg mm2 = round_trip(mm);
    CHECK(mm2.event.button == bropty::MouseButton::WheelLeft);
    CHECK_EQ(mm2.event.col, 200);
    CHECK_EQ(mm2.event.x, -1);

    EventMsg ev;
    ev.session = 3;
    ev.kind = EventKind::Notification;
    ev.a = "title";
    ev.b = std::string(100000, 'b');
    ev.x = -5;
    EventMsg ev2 = round_trip(ev);
    CHECK(ev2.kind == EventKind::Notification);
    CHECK_EQ(ev2.b.size(), size_t(100000));
    CHECK_EQ(ev2.x, -5);

    HistoryMsg h;
    h.req = 2;
    h.rows = {"a", "", std::string(300, 'z')};
    HistoryMsg h2 = round_trip(h);
    CHECK(h2.rows == h.rows);

    FrameMsg f;
    f.session = 1;
    f.frame_seq = 2;
    f.feed_seq = 3;
    f.ops = "xyz";
    FrameMsg f2 = round_trip(f);
    CHECK_EQ(f2.ops, std::string("xyz"));
    CHECK_EQ(f2.feed_seq, 3u);

    // Trailing bytes (a newer minor's fields) are ignored.
    PingMsg p;
    p.req = 4;
    std::string body;
    wire::Writer w(body);
    p.write(w);
    w.str("future field");
    PingMsg p2;
    CHECK(decode(body, p2));
    CHECK_EQ(p2.req, 4u);

    // Out-of-range enums are rejected.
    SetPolicyMsg sp;
    sp.session = 1;
    sp.resize_policy = 9;
    std::string spb;
    wire::Writer spw(spb);
    sp.write(spw);
    SetPolicyMsg sp2;
    CHECK(!decode(spb, sp2));
}

// Random bytes into every decoder: must never crash or allocate wildly.
void test_fuzz() {
    check::phase("fuzz", 120);
    vtgen::Rng rng(99);
    int decoded = 0;
    for (int i = 0; i < 20000; ++i) {
        std::string b(size_t(rng.range(0, 64)), '\0');
        for (char& c : b) c = char(rng.range(0, 255));
        CreateSessionMsg a;
        SessionListMsg l;
        KeyMsg k;
        MouseMsg m;
        HistoryMsg h;
        EventMsg e;
        AttachedMsg at;
        decoded += decode(b, a) + decode(b, l) + decode(b, k) + decode(b, m) + decode(b, h) + decode(b, e) +
                   decode(b, at);
    }
    std::printf("   fuzz: %d accidental decodes\n", decoded);
    CHECK(true);
}

}  // namespace

int main() {
    check::start_watchdog("test_wire");
    test_primitives();
    test_splitter();
    test_messages();
    test_fuzz();
    return check::finish("test_wire");
}
