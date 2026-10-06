// Protocol minor 1: Feed, the foreground process, OSC 133 command records,
// the OSC 22 pointer shape, OSC 99's notification fields and inline images
// (pixels chunked across frames, decoded through ServerOptions::decode_image)
// -- each checked against a local bropty Terminal fed the same bytes -- and
// the older client's path: a client that says minor 0 gets none of it, and
// every frame it gets still decodes.
// argv[1]: the mux_child helper.
#include "mux_fixture.h"

#include <bropty/view.h>

#include <cstring>

using namespace bromux;
using namespace std::chrono_literals;

namespace {

std::string g_child;

constexpr int kCols = 60;
constexpr int kRows = 20;

uint64_t make(Client& c, const SessionSpec& spec) {
    std::string err;
    auto info = c.create_session(spec, &err);
    CHECK_MSG(info.has_value(), "create: " + err);
    return info ? info->id : 0;
}

bool attach(Client& c, uint64_t id) {
    std::string err;
    auto info = c.attach(id, kCols, kRows, 0, &err);
    CHECK_MSG(info.has_value(), "attach: " + err);
    // Cells of 10 x 20 px, as the local terminal below lays images out.
    c.resize(id, kCols, kRows, 10, 20);
    return info.has_value();
}

std::unique_ptr<Client> client(fx::Fixture& f, uint16_t minor) {
    ConnectOptions o;
    o.address = f.address;
    o.autostart = false;
    o.client_name = minor ? "new" : "old";
    o.protocol_minor = minor;
    std::string err;
    auto c = Client::connect(o, &err);
    CHECK_MSG(c != nullptr, "connect: " + err);
    return c;
}

const ClientEvent* find_event(const std::vector<ClientEvent>& evs, EventKind k) {
    for (const ClientEvent& e : evs)
        if (e.kind == ClientEvent::Kind::Event && e.event.kind == k) return &e;
    return nullptr;
}

bool any_error(const std::vector<ClientEvent>& evs) {
    for (const ClientEvent& e : evs)
        if (e.kind == ClientEvent::Kind::Error) {
            std::printf("   error: %s\n", e.text.c_str());
            return true;
        }
    return false;
}

std::string b64(const std::string& in) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const uint32_t v = uint32_t(uint8_t(in[i])) << 16 | uint32_t(uint8_t(in[i + 1])) << 8 | uint8_t(in[i + 2]);
        out += k[v >> 18];
        out += k[(v >> 12) & 63];
        out += k[(v >> 6) & 63];
        out += k[v & 63];
    }
    if (i + 1 == in.size()) {
        const uint32_t v = uint32_t(uint8_t(in[i])) << 16;
        out += k[v >> 18];
        out += k[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == in.size()) {
        const uint32_t v = uint32_t(uint8_t(in[i])) << 16 | uint32_t(uint8_t(in[i + 1])) << 8;
        out += k[v >> 18];
        out += k[(v >> 12) & 63];
        out += k[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

// A kitty transmission of a w x h RGBA image (a gradient), in chunks.
std::string kitty_image(uint32_t id, int w, int h, int cols, int rows) {
    std::string px(size_t(w) * h * 4, '\0');
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            char* p = &px[(size_t(y) * w + x) * 4];
            p[0] = char(x * 255 / w);
            p[1] = char(y * 255 / h);
            p[2] = char((x + y) & 255);
            p[3] = char(255);
        }
    const std::string data = b64(px);
    std::string out;
    const size_t chunk = 4096;
    for (size_t at = 0; at < data.size(); at += chunk) {
        const bool first = at == 0;
        const bool last = at + chunk >= data.size();
        out += "\x1b_G";
        if (first)
            out += "a=T,f=32,i=" + std::to_string(id) + ",s=" + std::to_string(w) + ",v=" + std::to_string(h) +
                   ",c=" + std::to_string(cols) + ",r=" + std::to_string(rows) + ",q=2,";
        out += std::string("m=") + (last ? "0" : "1") + ";" + data.substr(at, chunk) + "\x1b\\";
    }
    return out;
}

// The decoder the server is given: "FAKEPNG" is a 3 x 2 image of one colour.
bool fake_decode(std::string_view data, const bropty::ImageLimits&, bropty::DecodedImage& out) {
    if (data != "FAKEPNG") return false;
    out.width = 3;
    out.height = 2;
    bropty::DecodedFrame f;
    f.rgba.assign(3 * 2 * 4, uint8_t(0x5A));
    out.frames = {f};
    return true;
}

// The images a view over `source` shows, as a view over `t` would show them.
void same_images(const bropty::Frame& a, const bropty::Frame& b) {
    CHECK_EQ(a.images.size(), b.images.size());
    if (a.images.size() != b.images.size()) return;
    for (size_t i = 0; i < a.images.size(); ++i) {
        const bropty::FrameImage& x = a.images[i];
        const bropty::FrameImage& y = b.images[i];
        CHECK(x.x == y.x && x.y == y.y && x.w == y.w && x.h == y.h);
        CHECK(x.src_x == y.src_x && x.src_y == y.src_y && x.src_w == y.src_w && x.src_h == y.src_h);
        CHECK(x.plane == y.plane && x.z == y.z && x.source == y.source && x.image_id == y.image_id);
        CHECK(x.pixels && y.pixels && x.pixels->rgba == y.pixels->rgba);
    }
}

void test_feed_commands_pointer_notification() {
    check::phase("feed, commands, pointer, notification");
    fx::Fixture f("minor1-feed", g_child);
    auto c = client(f, kProtocolMinor);
    if (!c) return;
    CHECK_EQ(c->server_minor(), kProtocolMinor);
    uint64_t id = make(*c, f.spec({"echo"}, kCols, kRows));
    if (!attach(*c, id)) return;
    CHECK(fx::pump_until(*c, [&] { return fx::screen_has(c->screen(id), "READY"); }, 10s));

    std::vector<ClientEvent> evs;
    const std::string bytes =
        "\x1b]22;pointer\x07"
        "\x1b]133;A\x07$ \x1b]133;B\x07make all\r\n\x1b]133;C\x07" "building\r\n\x1b]133;D;3\x07"
        "\x1b]133;A\x07$ \x1b]133;B\x07true\r\n\x1b]133;C;cmdline=true\x07\x1b]133;D;0\x07"
        "\x1b]133;A\x07$ "
        "\x1b]99;i=job1:u=2:d=0;Build\x1b\\\x1b]99;i=job1:p=body;finished\x1b\\"
        "FED\r\n";
    CHECK(c->feed(id, bytes));
    const ScreenModel* m = nullptr;
    CHECK(fx::pump_until(*c, [&] {
        m = c->screen(id);
        return m && fx::screen_has(m, "FED") && m->commands().size() == 3;
    }, 10s, &evs));
    CHECK(!any_error(evs));
    m = c->screen(id);
    if (!m) return;
    CHECK_EQ(m->pointer_shape(), std::string("pointer"));
    const auto& cmds = m->commands();
    if (cmds.size() == 3) {
        CHECK(cmds[0].finished && cmds[0].exit_code == 3);
        CHECK_EQ(cmds[0].command_line, std::string("make all"));  // read from the screen
        CHECK(cmds[1].finished && cmds[1].exit_code == 0);
        CHECK_EQ(cmds[1].command_line, std::string("true"));  // as the shell said it
        CHECK(!cmds[2].finished);
        CHECK(cmds[0].output && cmds[0].end && cmds[0].prompt.row >= m->history_first_row());
    }
    const ClientEvent* n = find_event(evs, EventKind::Notification);
    CHECK(n != nullptr);
    if (n) {
        CHECK_EQ(n->event.c, std::string("job1"));
        CHECK_EQ(n->event.d, std::string("osc99"));
        CHECK_EQ(n->event.x, int64_t(2));
        CHECK_EQ(n->event.a, std::string("Build"));
        CHECK_EQ(n->event.b, std::string("finished"));
    }
    // The tee recorded the feed: the oracle still holds.
    std::string d = fx::verify(*c, id, f);
    CHECK_MSG(d.empty(), d);

    // The records follow edits: a new command appends, and the list stays
    // what the terminal holds.
    CHECK(c->feed(id, "\x1b]133;B\x07ls\r\n\x1b]133;C\x07\x1b]133;D;1\x07"));
    CHECK(fx::pump_until(*c, [&] {
        const ScreenModel* mm = c->screen(id);
        return mm && mm->commands().size() == 3 && mm->commands()[2].finished;
    }, 10s));
    if ((m = c->screen(id)) && m->commands().size() == 3) {
        CHECK(m->commands()[2].exit_code == 1);
        CHECK_EQ(m->commands()[2].command_line, std::string("ls"));
    }
    // Reset (RIS) drops them all, and the pointer.
    CHECK(c->feed(id, "\x1b" "c"));
    CHECK(fx::pump_until(*c, [&] {
        const ScreenModel* mm = c->screen(id);
        return mm && mm->commands().empty() && mm->pointer_shape().empty();
    }, 10s));
}

void test_images() {
    check::phase("images");
    ServerOptions opt;
    opt.decode_image = fake_decode;
    fx::Fixture f("minor1-images", g_child, opt);
    auto c = client(f, kProtocolMinor);
    if (!c) return;
    uint64_t id = make(*c, f.spec({"echo"}, kCols, kRows));
    if (!attach(*c, id)) return;
    CHECK(fx::pump_until(*c, [&] { return fx::screen_has(c->screen(id), "READY"); }, 10s));

    // A 700 x 500 image (1.4 MB of RGBA: more than one frame carries), a
    // second one, a sixel, and a "PNG" the server's decoder knows.
    const std::string bytes = std::string("\x1b[2J\x1b[H") + kitty_image(5, 700, 500, 20, 8) + "\r\nafter\r\n" +
                              kitty_image(6, 16, 16, 4, 2) + "\x1bPq#1;2;100;0;0!30~-!30~\x1b\\\r\n" +
                              "\x1b_Ga=T,f=100,i=9,c=3,r=1,q=2;" + b64("FAKEPNG") + "\x1b\\\r\nEND";
    CHECK(c->feed(id, bytes));
    bropty::Terminal t(bropty::TerminalOptions{kCols, kRows, 2000});
    struct Host : bropty::TerminalHost {
        bool decode_image(std::string_view d, const bropty::ImageLimits& l, bropty::DecodedImage& o) override {
            return fake_decode(d, l, o);
        }
    } host;
    t.set_host(&host);
    t.set_cell_pixel_size(10, 20);
    t.feed(bytes);
    bropty::TerminalView tv(t);
    auto ft = tv.snapshot();
    CHECK_EQ(ft->images.size(), size_t(4));

    std::vector<ClientEvent> evs;
    bropty::TerminalView* mv = nullptr;
    std::shared_ptr<const bropty::Frame> fm;
    CHECK(fx::pump_until(*c, [&] {
        mv = c->view(id);
        if (!mv || !fx::screen_has(c->screen(id), "END")) return false;
        fm = mv->snapshot();
        return fm->images.size() == ft->images.size();
    }, 20s, &evs));
    CHECK(!any_error(evs));
    if (fm) same_images(*fm, *ft);
    if (const ScreenModel* m = c->screen(id)) {
        CHECK_EQ(m->images().image_count(), size_t(4));
        CHECK_EQ(m->images().placement_count(), size_t(3));
        CHECK(m->images().bytes() >= size_t(700) * 500 * 4);
    }

    // Deleting them all: the client's copy goes too.
    CHECK(c->feed(id, "\x1b_Ga=d,d=A,q=2\x1b\\\x1b[2J"));
    t.feed("\x1b_Ga=d,d=A,q=2\x1b\\\x1b[2J");
    CHECK(fx::pump_until(*c, [&] {
        bropty::TerminalView* v = c->view(id);
        return v && v->snapshot()->images.empty();
    }, 10s));
    if (const ScreenModel* m = c->screen(id)) CHECK_EQ(m->images().placement_count(), size_t(0));

    // A second client attaching later gets the images it has never seen.
    CHECK(c->feed(id, kitty_image(7, 300, 300, 10, 5)));
    auto c2 = client(f, kProtocolMinor);
    if (!c2 || !attach(*c2, id)) return;
    CHECK(fx::pump_until(*c2, [&] {
        bropty::TerminalView* v = c2->view(id);
        return v && v->snapshot()->images.size() == 1;
    }, 10s));
}

void test_foreground() {
    check::phase("foreground");
    fx::Fixture f("minor1-fg", g_child);
    auto c = client(f, kProtocolMinor);
    if (!c) return;
    std::string err;
    auto info = c->create_session(f.spec({"echo"}, kCols, kRows), &err);
    CHECK_MSG(info.has_value(), err);
    if (!info || !attach(*c, info->id)) return;
    std::vector<ClientEvent> evs;
    // At attach: what owns the terminal (the session's program itself).
    CHECK(fx::pump_until(*c, [&] { return find_event(evs, EventKind::Foreground) != nullptr; }, 10s, &evs));
    if (const ClientEvent* e = find_event(evs, EventKind::Foreground)) {
        CHECK_EQ(e->event.x, info->pid);
        CHECK(e->event.a.find("mux_child") != std::string::npos);
        CHECK(!e->event.b.empty());
    }
    // The program exits: nothing owns it any more.
    evs.clear();
    c->send_raw(info->id, "q");
    CHECK(fx::pump_until(*c, [&] {
        for (const ClientEvent& e : evs)
            if (e.kind == ClientEvent::Kind::Event && e.event.kind == EventKind::Foreground && e.event.x == 0)
                return true;
        return false;
    }, 10s, &evs));
}

void test_old_client() {
    check::phase("old client");
    fx::Fixture f("minor1-old", g_child);
    auto oldc = client(f, 0);
    auto newc = client(f, kProtocolMinor);
    if (!oldc || !newc) return;
    uint64_t id = make(*newc, f.spec({"echo"}, kCols, kRows));
    if (!attach(*newc, id) || !attach(*oldc, id)) return;
    std::vector<ClientEvent> old_evs, new_evs;
    // Feed only once the program is up: before that, conhost's first paint
    // may still be on its way, and some builds (Windows Server 2022's) begin
    // it by clearing the screen, which would erase what was fed.
    CHECK(fx::pump_until(*newc, [&] { return fx::screen_has(newc->screen(id), "READY"); }, 10s, &new_evs));
    CHECK(newc->feed(id, "\x1b]22;wait\x07\x1b]133;A\x07$ " + kitty_image(3, 8, 8, 2, 1) + "\r\nOLDTEST"));
    CHECK(fx::pump_until(*newc, [&] {
        const ScreenModel* m = newc->screen(id);
        return m && fx::screen_has(m, "OLDTEST") && m->commands().size() == 1 && m->images().placement_count() == 1;
    }, 10s, &new_evs));
    CHECK(fx::pump_until(*oldc, [&] { return fx::screen_has(oldc->screen(id), "OLDTEST"); }, 10s, &old_evs));
    std::string err;
    CHECK(oldc->sync(id, &err));
    std::vector<ClientEvent> more;
    oldc->dispatch(more);
    old_evs.insert(old_evs.end(), more.begin(), more.end());
    CHECK(!any_error(old_evs));  // nothing it cannot decode
    if (const ScreenModel* m = oldc->screen(id)) {
        CHECK(m->commands().empty());
        CHECK(m->pointer_shape().empty());
        CHECK_EQ(m->images().image_count(), size_t(0));
    }
    CHECK(find_event(old_evs, EventKind::Foreground) == nullptr);
    CHECK(find_event(new_evs, EventKind::Foreground) != nullptr);
    std::string d = fx::verify(*oldc, id, f);
    CHECK_MSG(d.empty(), d);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: test_minor1 <mux_child>\n");
        return 2;
    }
    g_child = std::filesystem::absolute(argv[1]).string();
    check::start_watchdog("test_minor1");
    test_feed_commands_pointer_notification();
    test_images();
    test_foreground();
    test_old_client();
    return check::finish("test_minor1");
}
