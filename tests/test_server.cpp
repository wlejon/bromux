// The server's behaviour through the client library: sessions, input
// routing, resize policies, events, OSC 52 both ways, metadata and blobs,
// session notifications, history, exit, close, and protocol errors.
// argv[1]: the mux_child helper.
#include "mux_fixture.h"

#include <bromux/stream.h>

using namespace bromux;
using namespace std::chrono_literals;

namespace {

std::string g_child;

uint64_t make(Client& c, const SessionSpec& spec) {
    std::string err;
    auto info = c.create_session(spec, &err);
    CHECK_MSG(info.has_value(), "create: " + err);
    return info ? info->id : 0;
}

bool attach(Client& c, uint64_t id, int cols, int rows, uint32_t flags = 0) {
    std::string err;
    auto info = c.attach(id, cols, rows, flags, &err);
    CHECK_MSG(info.has_value(), "attach: " + err);
    return info.has_value();
}

bool wait_text(Client& c, uint64_t id, std::string_view text, std::chrono::milliseconds t = 10s) {
    bool ok = fx::pump_until(c, [&] { return fx::screen_has(c.screen(id), text); }, t);
    if (!ok) std::printf("   waiting for [%.*s]; screen:\n%s", int(text.size()), text.data(), fx::screen_dump(c.screen(id)).c_str());
    return ok;
}

void test_basics() {
    check::phase("basics");
    fx::Fixture f("basics", g_child);
    auto c = f.client();
    if (!c) return;
    CHECK(c->ping());
    CHECK_EQ(c->server_info().major, kProtocolMajor);
    auto list = c->list_sessions();
    CHECK(list && list->empty());

    SessionSpec s = f.spec({"lines", "50"}, 70, 20);
    s.meta = {{"name", "first"}};
    uint64_t id = make(*c, s);
    list = c->list_sessions();
    CHECK(list && list->size() == 1);
    if (list && !list->empty()) {
        CHECK_EQ((*list)[0].id, id);
        CHECK_EQ((*list)[0].meta_value("name"), std::string("first"));
        CHECK((*list)[0].running);
        CHECK((*list)[0].pid > 0);
    }
    if (!attach(*c, id, 100, 30)) return;
    CHECK(wait_text(*c, id, "LINES DONE"));
    const ScreenModel* m = c->screen(id);
    CHECK(m && m->cols() == 100 && m->rows() == 30);  // Latest: the only client decides
    std::string d = fx::verify(*c, id, f);
    CHECK_MSG(d.empty(), d);

    // Errors that do not end the connection.
    std::string err;
    CHECK(!c->attach(9999, 80, 24, 0, &err));
    CHECK(!err.empty());
    std::vector<ClientEvent> evs;
    c->send_raw(424242, "x");
    CHECK(fx::pump_until(*c, [&] {
        for (const ClientEvent& e : evs)
            if (e.kind == ClientEvent::Kind::Error && e.code == ErrorCode::NotAttached) return true;
        return false;
    }, 5s, &evs));
    CHECK(c->ping());

    // Close: attached clients are told.
    evs.clear();
    CHECK(c->close_session(id));
    CHECK(fx::pump_until(*c, [&] { return c->screen(id) == nullptr; }, 5s, &evs));
    bool told = false;
    for (const ClientEvent& e : evs)
        told = told || (e.kind == ClientEvent::Kind::Detached && e.detach_reason == DetachReason::SessionClosed);
    CHECK(told);
    list = c->list_sessions();
    CHECK(list && list->empty());
}

void test_input() {
    check::phase("input routing");
    fx::Fixture f("input", g_child);
    auto c = f.client();
    if (!c) return;
    uint64_t id = make(*c, f.spec({"echo"}));
    if (!attach(*c, id, 80, 24)) return;
    CHECK(wait_text(*c, id, "READY"));
    c->send_raw(id, "abc");
    CHECK(wait_text(*c, id, "[abc]"));
    c->send_key(id, bropty::KeyEvent::functional(bropty::Key::Up));
    CHECK(wait_text(*c, id, "[\\e[A]"));
    c->send_key(id, bropty::KeyEvent::character('c', bropty::Mod_Ctrl));
    CHECK(wait_text(*c, id, "[\\x03]"));
    c->send_text(id, "zz");
    CHECK(wait_text(*c, id, "[zz]"));
    c->paste(id, "p1\np2");
    // The program may read the paste in more than one chunk.
    auto echoed = [&] {
        std::string all;
        const ScreenModel* m = c->screen(id);
        for (int y = 0; m && y < m->rows(); ++y) {
            std::string t = m->row(y).text();
            if (t.size() >= 2 && t.front() == '[' && t.back() == ']') all += t.substr(1, t.size() - 2);
        }
        return all;
    };
    CHECK(fx::pump_until(*c, [&] { return echoed().find("p1\\rp2") != std::string::npos; }, 10s));
    // Read-only attachments may not type.
    auto viewer = f.client("viewer");
    if (!viewer) return;
    if (!attach(*viewer, id, 80, 24, Attach_ReadOnly)) return;
    std::vector<ClientEvent> evs;
    viewer->send_raw(id, "nope");
    CHECK(fx::pump_until(*viewer, [&] {
        for (const ClientEvent& e : evs)
            if (e.kind == ClientEvent::Kind::Error && e.code == ErrorCode::ReadOnly) return true;
        return false;
    }, 5s, &evs));
    // Both see the same screen.
    c->send_raw(id, "both");
    CHECK(wait_text(*c, id, "[both]"));
    CHECK(wait_text(*viewer, id, "[both]"));
    CHECK(!fx::screen_has(c->screen(id), "nope"));
    std::string d = fx::verify(*viewer, id, f);
    CHECK_MSG(d.empty(), d);
}

void expect_size(Client& c, uint64_t id, int cols, int rows, const char* what) {
    const bool ok = fx::pump_until(c, [&] {
        const ScreenModel* m = c.screen(id);
        return m && m->cols() == cols && m->rows() == rows;
    }, 5s);
    const ScreenModel* m = c.screen(id);
    CHECK_MSG(ok, std::string(what) + ": got " + (m ? std::to_string(m->cols()) + "x" + std::to_string(m->rows()) : "none") +
                      ", want " + std::to_string(cols) + "x" + std::to_string(rows));
}

void test_resize_policy() {
    check::phase("resize policies");
    fx::Fixture f("resize", g_child);
    auto a = f.client("a");
    auto b = f.client("b");
    if (!a || !b) return;
    uint64_t id = make(*a, f.spec({"size"}));
    if (!attach(*a, id, 100, 30)) return;
    expect_size(*a, id, 100, 30, "latest: a alone");
    if (!attach(*b, id, 60, 20)) return;
    expect_size(*a, id, 60, 20, "latest: b attached last");
    a->send_raw(id, "x");  // a becomes the most recent
    expect_size(*a, id, 100, 30, "latest: a typed");
    // The program sees the size too.
    CHECK(wait_text(*a, id, "SIZE 100 30"));

    a->set_policy(id, ResizePolicy::Smallest, std::nullopt);
    expect_size(*a, id, 60, 20, "smallest");
    a->set_policy(id, ResizePolicy::Largest, std::nullopt);
    expect_size(*a, id, 100, 30, "largest");
    b->resize(id, 120, 40);
    expect_size(*a, id, 120, 40, "largest after b grew");
    a->set_policy(id, ResizePolicy::Fixed, std::nullopt);
    a->resize(id, 50, 10);
    b->resize(id, 51, 11);
    CHECK(a->ping());
    CHECK(b->ping());
    expect_size(*a, id, 120, 40, "fixed");
    // A no-resize client never counts.
    a->set_policy(id, ResizePolicy::Smallest, std::nullopt);
    auto c = f.client("c");
    if (!c || !attach(*c, id, 20, 5, Attach_NoResize)) return;
    expect_size(*a, id, 50, 10, "smallest ignores the no-resize client");
    // Detaching re-evaluates.
    a->detach(id);
    expect_size(*b, id, 51, 11, "after a left");
    std::string d = fx::verify(*b, id, f);
    CHECK_MSG(d.empty(), d);
}

bool has_event(const std::vector<ClientEvent>& evs, EventKind k, const std::string& a = {}, const std::string& b = {}) {
    for (const ClientEvent& e : evs)
        if (e.kind == ClientEvent::Kind::Event && e.event.kind == k && (a.empty() || e.event.a == a) &&
            (b.empty() || e.event.b == b))
            return true;
    return false;
}

void test_events() {
    check::phase("events");
    fx::Fixture f("events", g_child);
    auto c = f.client();
    if (!c) return;
    std::vector<ClientEvent> evs;
    uint64_t id = make(*c, f.spec({"osc"}));
    if (!attach(*c, id, 80, 24)) return;
    CHECK(wait_text(*c, id, "OSC READY"));
    c->send_raw(id, "g");
    CHECK(fx::pump_until(*c, [&] { return fx::screen_has(c->screen(id), "OSC DONE"); }, 10s, &evs));
    const ScreenModel* m = c->screen(id);
    CHECK(m && m->title() == "the title");
    CHECK(has_event(evs, EventKind::Title, "the title"));
    CHECK(has_event(evs, EventKind::Bell));
    std::string d = fx::verify(*c, id, f);
    CHECK_MSG(d.empty(), d);
    // A hyperlinked cell carries its URI.
    bool link = false;
    for (int y = 0; m && y < m->rows(); ++y) {
        bropty::RowView r = m->row(y);
        for (int x = 0; x < r.cols; ++x) {
            const ModelLink* l = m->hyperlink(r.style(x).link);
            link = link || (l && l->uri == "https://example.com");
        }
    }
#if !defined(_WIN32)
    // ConPTY consumes these sequences; on a POSIX pty they reach the emulator.
    CHECK(link);
    CHECK(m && m->cwd() == "file://host/some/dir");
    CHECK(has_event(evs, EventKind::Cwd, "file://host/some/dir"));
    CHECK(has_event(evs, EventKind::Notification, "", "notify body"));
    CHECK(has_event(evs, EventKind::Notification, "Title", "Body"));
    CHECK(has_event(evs, EventKind::ClipboardWrite, "c", "hello clipboard"));
    std::string marks;  // OSC 133 kinds, with D's parameters
    for (const ClientEvent& e : evs)
        if (e.kind == ClientEvent::Kind::Event && e.event.kind == EventKind::SemanticMark) {
            marks += char(e.event.x);
            if (!e.event.a.empty()) marks += "(" + e.event.a + ")";
        }
    CHECK_EQ(marks, std::string("ABCD(0)"));
#endif

    // OSC 52 writes are dropped under the Deny policy.
    evs.clear();
    uint64_t id2 = make(*c, f.spec({"osc"}));
    c->set_policy(id2, std::nullopt, ClipboardPolicy::Deny);
    if (!attach(*c, id2, 80, 24)) return;
    CHECK(wait_text(*c, id2, "OSC READY"));
    c->send_raw(id2, "g");
    CHECK(fx::pump_until(*c, [&] { return fx::screen_has(c->screen(id2), "OSC DONE"); }, 10s, &evs));
    CHECK(!has_event(evs, EventKind::ClipboardWrite));
    CHECK(has_event(evs, EventKind::Title, "the title"));
}

void test_clipboard() {
    check::phase("clipboard (OSC 52 both ways)");
    ServerOptions opt;
    opt.clipboard_policy = ClipboardPolicy::ReadWrite;
    fx::Fixture f("clip", g_child, opt);
    auto c = f.client();
    if (!c) return;
    uint64_t id = make(*c, f.spec({"clipread"}));
    if (!attach(*c, id, 80, 24)) return;
    CHECK(wait_text(*c, id, "CLIP READY"));
    std::vector<ClientEvent> evs;
    c->send_raw(id, "g");
    uint32_t token = 0;
    std::string sel;
    const bool asked = fx::pump_until(*c, [&] {
        for (const ClientEvent& e : evs)
            if (e.kind == ClientEvent::Kind::ClipboardRequest) {
                token = e.token;
                sel = e.text;
                return true;
            }
        return false;
    }, 5s, &evs);
#if defined(_WIN32)
    // ConPTY does not forward OSC 52 queries; only the policy plumbing applies.
    (void)asked;
#else
    CHECK(asked);
    CHECK_EQ(sel, std::string("c"));
    c->answer_clipboard(id, token, true, "secret");
    CHECK(wait_text(*c, id, "GOT:c2VjcmV0"));
#endif
}

void test_meta_blobs_notify() {
    check::phase("meta, blobs, notifications");
    fx::Fixture f("meta", g_child);
    auto c = f.client();
    auto w = f.client("watcher", true);
    if (!c || !w) return;
    std::vector<ClientEvent> evs;
    uint64_t id = make(*c, f.spec({"lines", "3"}));
    c->set_meta(id, "name", "renamed");
    c->set_meta(id, "layout", std::string("\x00\x01", 2));
    c->erase_meta(id, "layout");
    auto list = c->list_sessions();
    CHECK(list && list->size() == 1 && (*list)[0].meta_value("name") == "renamed" && (*list)[0].meta.size() == 1);

    c->put_blob("layout/main", std::string("{\"tree\":[1,2]}\0x", 16));
    auto blob = c->get_blob("layout/main");
    CHECK(blob && blob->size() == 16);
    std::string err;
    CHECK(!c->get_blob("nope", &err));
    c->put_blob("layout/main", "");
    CHECK(!c->get_blob("layout/main"));

    CHECK(c->close_session(id));
    bool added = false, changed = false, removed = false;
    fx::pump_until(*w, [&] {
        for (const ClientEvent& e : evs) {
            if (e.kind != ClientEvent::Kind::SessionNotify || e.session != id) continue;
            added = added || e.notify == NotifyKind::Added;
            changed = changed || (e.notify == NotifyKind::Changed && e.info.meta_value("name") == "renamed");
            removed = removed || e.notify == NotifyKind::Removed;
        }
        return added && changed && removed;
    }, 5s, &evs);
    CHECK(added);
    CHECK(changed);
    CHECK(removed);
}

void test_history() {
    check::phase("history");
    fx::Fixture f("history", g_child);
    auto c = f.client();
    if (!c) return;
    uint64_t id = make(*c, f.spec({"lines", "700"}, 80, 24));
    if (!attach(*c, id, 80, 24)) return;
    CHECK(wait_text(*c, id, "LINES DONE", 20s));
    CHECK(c->sync(id));
    const ScreenModel* m = c->screen(id);
    CHECK(m && m->history_rows() > 600);
    std::string err;
    auto h = c->fetch_history(id, 0, 100000, &err);
    CHECK_MSG(h.has_value(), err);
    if (!h || !m) return;
    CHECK_EQ(h->rows.size(), size_t(h->history_rows));
    TeeHeader th;
    std::vector<TeeRecord> recs;
    CHECK(read_tee(f.tee(id), th, recs, &err));
    auto t = replay_tee(th, recs, size_t(h->feed_seq));
    std::string d = oracle::compare_history(*h, *t);
    CHECK_MSG(d.empty(), d);
    // A window in the middle.
    auto part = c->fetch_history(id, 100, 50, &err);
    CHECK(part && part->start == 100 && part->rows.size() == 50);
    if (part) {
        auto t2 = replay_tee(th, recs, size_t(part->feed_seq));
        d = oracle::compare_history(*part, *t2);
        CHECK_MSG(d.empty(), d);
    }
    // After a resize the history reflows; it still matches.
    c->resize(id, 37, 24);
    CHECK(fx::pump_until(*c, [&] { return c->screen(id)->cols() == 37; }, 5s));
    auto h2 = c->fetch_history(id, 0, 100000, &err);
    CHECK(h2.has_value());
    if (h2) {
        CHECK(read_tee(f.tee(id), th, recs, &err));
        auto t3 = replay_tee(th, recs, size_t(h2->feed_seq));
        d = oracle::compare_history(*h2, *t3);
        CHECK_MSG(d.empty(), d);
    }
}

void test_exit() {
    check::phase("program exit");
    fx::Fixture f("exit", g_child);
    auto c = f.client();
    if (!c) return;
    uint64_t id = make(*c, f.spec({"exit", "7"}));
    std::vector<ClientEvent> evs;
    if (!attach(*c, id, 80, 24)) return;
    bool exited = fx::pump_until(*c, [&] {
        for (const ClientEvent& e : evs)
            if (e.kind == ClientEvent::Kind::Event && e.event.kind == EventKind::Exited) return true;
        return false;
    }, 10s, &evs);
    CHECK(exited);
    for (const ClientEvent& e : evs)
        if (e.kind == ClientEvent::Kind::Event && e.event.kind == EventKind::Exited) CHECK_EQ(e.event.x, 7);
    auto list = c->list_sessions();
    CHECK(list && list->size() == 1 && !(*list)[0].running && (*list)[0].exit_code == 7);
    CHECK(wait_text(*c, id, "bye"));
    std::string d = fx::verify(*c, id, f);
    CHECK_MSG(d.empty(), d);

    // A program that crashes ends: no fault dialog holds it (the error mode
    // reaches session programs), and its status says it crashed.
    const uint64_t crash = make(*c, f.spec({"crash"}));
    int64_t crash_code = 0;
    const bool crashed = fx::pump_until(*c, [&] {
        auto l = c->list_sessions();
        if (l)
            for (const SessionInfo& i : *l)
                if (i.id == crash && !i.running) {
                    crash_code = i.exit_code;
                    return true;
                }
        return false;
    }, 20s);
    CHECK_MSG(crashed, "a crashing session program did not end (a fault dialog?)");
#if defined(_WIN32)
    CHECK_MSG(check::is_crash_status(crash_code), "exit status 0x" + fx::hex(uint32_t(crash_code)));
#else
    CHECK(crash_code != 0);
#endif
    CHECK(c->close_session(crash));

    // remove_on_exit: the session goes away by itself.
    SessionSpec s = f.spec({"exit", "0"});
    s.remove_on_exit = true;
    uint64_t id2 = make(*c, s);
    CHECK(fx::pump_until(*c, [&] {
        auto l = c->list_sessions();
        if (!l) return false;
        for (const SessionInfo& i : *l)
            if (i.id == id2) return false;
        return true;
    }, 10s));
}

void test_protocol_errors() {
    check::phase("protocol errors");
    fx::Fixture f("proto", g_child);
    auto good = f.client();
    if (!good) return;
    // Garbage framing: the server drops that connection only.
    {
        std::string err;
        auto s = connect_local(f.address, &err);
        CHECK(s != nullptr);
        if (s) {
            std::string junk(64, char(0xFF));
            s->write(junk);
            char buf[256];
            size_t total = 0;
            for (size_t n; (n = s->read(buf, sizeof buf)) > 0;) total += n;
            CHECK(total > 0);  // an Error message, then the close
        }
    }
    // A message before Hello.
    {
        auto s = connect_local(f.address);
        if (s) {
            PingMsg p;
            s->write(encode(p));
            wire::MessageSplitter sp;
            char buf[256];
            bool saw_error = false;
            for (size_t n; (n = s->read(buf, sizeof buf)) > 0;) {
                sp.feed(buf, n);
                wire::MessageSplitter::Message m;
                while (sp.next(m)) {
                    ErrorMsg e;
                    saw_error = saw_error || (m.type == uint16_t(MsgType::Error) && decode(m.payload, e) &&
                                              e.code == ErrorCode::HelloRequired);
                }
            }
            CHECK(saw_error);
        }
    }
    // Another major version.
    {
        auto s = connect_local(f.address);
        if (s) {
            HelloMsg h;
            h.major = kProtocolMajor + 1;
            s->write(encode(h));
            wire::MessageSplitter sp;
            char buf[256];
            bool mismatch = false;
            for (size_t n; (n = s->read(buf, sizeof buf)) > 0;) {
                sp.feed(buf, n);
                wire::MessageSplitter::Message m;
                while (sp.next(m)) {
                    ErrorMsg e;
                    mismatch = mismatch || (m.type == uint16_t(MsgType::Error) && decode(m.payload, e) &&
                                            e.code == ErrorCode::VersionMismatch);
                }
            }
            CHECK(mismatch);
        }
    }
    CHECK(good->ping());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: test_server <mux_child>\n");
        return 2;
    }
    g_child = std::filesystem::absolute(argv[1]).string();
    check::start_watchdog("test_server");
    test_basics();
    test_input();
    test_resize_policy();
    test_events();
    test_clipboard();
    test_meta_blobs_notify();
    test_history();
    test_exit();
    test_protocol_errors();
    return check::finish("test_server");
}
