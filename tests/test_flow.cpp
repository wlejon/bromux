// Flow control and failure isolation: a flooding session must not starve a
// quiet one or the clients; a client that stops reading must not stall the
// server or the other clients (and catches up with the latest state when it
// reads again); client processes killed mid-stream -- reading, hung, or in
// the middle of writing a large message -- must leave the server and every
// other client unharmed. The oracle checks every screen afterwards.
// argv[1]: mux_child, argv[2]: mux_client_child.
#include "mux_fixture.h"

#include <bromux/stream.h>

using namespace bromux;
using namespace std::chrono_literals;

namespace {

std::string g_child;
std::string g_client_child;

struct Timer {
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    double ms() const {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
};

void test_flood_fairness() {
    check::phase("flood vs a quiet session", 300);
    fx::Fixture f("flow-fair", g_child);
    auto c = f.client();
    if (!c) return;
    // A flood that outlasts the test (closed at the end).
    auto flood = c->create_session(f.spec({"flood", std::to_string(1ll << 40)}, 120, 40));
    auto quiet = c->create_session(f.spec({"lines", "200", "10"}, 80, 24));
    CHECK(flood && quiet);
    if (!flood || !quiet) return;
    CHECK(c->attach(flood->id, 120, 40).has_value());
    CHECK(c->attach(quiet->id, 80, 24).has_value());
    Timer t;
    int quiet_frames = 0;
    int flood_frames = 0;
    std::vector<ClientEvent> evs;
    const bool quiet_done = fx::pump_until(*c, [&] {
        for (const ClientEvent& e : evs) {
            if (e.kind != ClientEvent::Kind::Frame) continue;
            if (e.session == quiet->id) ++quiet_frames;
            if (e.session == flood->id) ++flood_frames;
        }
        evs.clear();
        return fx::screen_has(c->screen(quiet->id), "LINES DONE");
    }, 60s, &evs);
    const double quiet_ms = t.ms();
    CHECK(quiet_done);
    // 200 lines at 10 ms: about 2 s on its own. Under the flood it must not
    // be held back by much (bound generous for loaded CI machines).
    std::printf("   quiet session finished in %.0f ms (%d frames) while the flood produced %d frames\n", quiet_ms,
                quiet_frames, flood_frames);
    CHECK(quiet_ms < 15000);
    CHECK(quiet_frames > 20);
    CHECK(flood_frames > 20);
    // Mid-flood, the flooding session's screen is exact too. (Each check
    // replays the whole recording so far, which a fast pty makes large.)
    CHECK(!fx::screen_has(c->screen(flood->id), "FLOOD DONE"));
    for (int i = 0; i < 2; ++i) {
        std::string d = fx::verify(*c, flood->id, f);
        CHECK_MSG(d.empty(), d);
    }
    std::string d = fx::verify(*c, quiet->id, f);
    CHECK_MSG(d.empty(), d);
    CHECK(c->close_session(flood->id));
}

void test_stalled_client() {
    check::phase("a client that stops reading", 300);
    fx::Fixture f("flow-stall", g_child);
    auto live = f.client("live");
    auto stuck = f.client("stuck");
    if (!live || !stuck) return;
    auto flood = live->create_session(f.spec({"flood", std::to_string(20 << 20)}, 100, 30));
    CHECK(flood.has_value());
    if (!flood) return;
    CHECK(stuck->attach(flood->id, 100, 30).has_value());
    CHECK(live->attach(flood->id, 100, 30, Attach_NoResize).has_value());
    // `stuck` never dispatches during the flood; `live` must still finish.
    Timer t;
    const bool done = fx::pump_until(*live, [&] { return fx::screen_has(live->screen(flood->id), "FLOOD DONE"); }, 240s);
    CHECK(done);
    std::printf("   live client saw the 20 MB flood end after %.0f ms\n", t.ms());
    CHECK(live->ping());
    // The stuck client wakes up: it gets the latest state, not a backlog.
    Timer catchup;
    std::string d = fx::verify(*stuck, flood->id, f);
    CHECK_MSG(d.empty(), d);
    std::printf("   stuck client caught up in %.0f ms\n", catchup.ms());
    d = fx::verify(*live, flood->id, f);
    CHECK_MSG(d.empty(), d);
}

void test_killed_clients() {
    check::phase("client processes killed mid-stream", 300);
    fx::Fixture f("flow-kill", g_child);
    auto c = f.client();
    if (!c) return;
    auto flood = c->create_session(f.spec({"flood", std::to_string(60 << 20)}, 90, 25));
    auto echo = c->create_session(f.spec({"echo"}, 90, 25));
    CHECK(flood && echo);
    if (!flood || !echo) return;
    CHECK(c->attach(flood->id, 90, 25, Attach_NoResize).has_value());
    const char* modes[] = {"read", "stall", "spam", "read", "spam", "stall", "read", "spam"};
    int round = 0;
    for (const char* mode : modes) {
        const uint64_t target = std::string(mode) == "spam" ? echo->id : flood->id;
        std::string err;
        auto p = Process::spawn({g_client_child, f.address, std::to_string(target), mode}, &err);
        CHECK_MSG(p != nullptr, err);
        if (!p) continue;
        // Let it get going, then kill it without warning.
        std::this_thread::sleep_for(std::chrono::milliseconds(300 + 150 * (round++ % 4)));
        p->kill();
        int code = 0;
        CHECK(p->wait_for(10s, &code));
        // The server is fine and so is this client.
        CHECK(c->ping());
        std::vector<ClientEvent> evs;
        c->dispatch(evs);
        for (const ClientEvent& e : evs) CHECK_MSG(e.kind != ClientEvent::Kind::Disconnected, e.text);
        std::string d = fx::verify(*c, flood->id, f);
        CHECK_MSG(d.empty(), std::string(mode) + ": " + d);
    }
    // Nobody is left attached but us.
    auto list = c->list_sessions();
    CHECK(list.has_value());
    if (list)
        for (const SessionInfo& s : *list)
            if (s.id == flood->id) CHECK_EQ(s.clients, 1u);
    CHECK(c->attach(echo->id, 90, 25).has_value());
    std::string d = fx::verify(*c, echo->id, f);
    CHECK_MSG(d.empty(), d);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: test_flow <mux_child> <mux_client_child>\n");
        return 2;
    }
    g_child = std::filesystem::absolute(argv[1]).string();
    g_client_child = std::filesystem::absolute(argv[2]).string();
    check::start_watchdog("test_flow");
    test_flood_fairness();
    test_stalled_client();
    test_killed_clients();
    return check::finish("test_flow");
}
