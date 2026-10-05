// The oracle under churn: sessions running random VT streams and real shells
// while several clients resize, detach and reattach, switch resize
// policies, stall, and drop their connections without a word. At random
// checkpoints (mid-output included) every attached client's screen must
// equal, cell for cell, a bropty Terminal fed the session's recorded PTY
// output up to the state the client shows; at the end, history must too.
// argv[1]: the mux_child helper. BROMUX_ORACLE_STEPS scales the run.
#include "mux_fixture.h"
#include "vtgen.h"

#include <cstdlib>
#include <map>

#if !defined(_WIN32)
#include <unistd.h>
#endif

using namespace bromux;
using namespace std::chrono_literals;

namespace {

std::string g_child;
int g_mismatches = 0;

struct Player {
    std::string name;
    std::unique_ptr<Client> client;
    std::map<uint64_t, bool> attached;
    int stall{0};  // turns left without dispatching
};

int g_checkpoints = 0;

void check_screen(Client& c, const std::string& who, uint64_t id, const fx::Fixture& f, const char* when) {
    std::string d = fx::verify(c, id, f);
    ++g_checkpoints;
    if (!d.empty()) ++g_mismatches;
    CHECK_MSG(d.empty(), std::string(when) + " [" + who + " session " + std::to_string(id) + "]: " + d);
}

void check_screen(Player& p, uint64_t id, const fx::Fixture& f, const char* when) {
    check_screen(*p.client, p.name, id, f, when);
}

void check_history(Client& c, uint64_t id, const fx::Fixture& f) {
    std::string err;
    auto h = c.fetch_history(id, 0, 100000, &err);
    CHECK_MSG(h.has_value(), err);
    if (!h) return;
    TeeHeader th;
    std::vector<TeeRecord> recs;
    CHECK(read_tee(f.tee(id), th, recs, &err));
    auto t = replay_tee(th, recs, size_t(h->feed_seq));
    std::string d = oracle::compare_history(*h, *t);
    CHECK_MSG(d.empty(), "history of session " + std::to_string(id) + ": " + d);
}

// Random churn over `sessions` with `players`, for `steps` turns.
void churn(fx::Fixture& f, std::vector<Player>& players, const std::vector<uint64_t>& sessions, int steps,
           uint64_t seed, const std::function<void(int)>& each_step = {}) {
    vtgen::Rng r(seed);
    for (int step = 0; step < steps; ++step) {
        if (each_step) each_step(step);
        Player& p = players[size_t(r.range(0, int(players.size()) - 1))];
        const uint64_t id = sessions[size_t(r.range(0, int(sessions.size()) - 1))];
        const int op = r.range(0, 99);
        if (!p.client) {
            p.client = f.client(p.name);
            p.attached.clear();
            continue;
        }
        if (op < 25) {  // attach (or re-attach: a fresh full frame)
            auto info = p.client->attach(id, r.range(10, 160), r.range(3, 60), r.chance(15) ? Attach_NoResize : 0);
            CHECK(info.has_value());
            p.attached[id] = info.has_value();
        } else if (op < 45) {  // resize
            if (p.attached[id]) p.client->resize(id, r.range(10, 160), r.range(3, 60));
        } else if (op < 55) {  // detach
            if (p.attached[id]) {
                p.client->detach(id);
                p.attached[id] = false;
            }
        } else if (op < 60) {  // policy
            const ResizePolicy pol[] = {ResizePolicy::Latest, ResizePolicy::Smallest, ResizePolicy::Largest,
                                        ResizePolicy::Fixed};
            p.client->set_policy(id, pol[r.range(0, 3)], std::nullopt);
        } else if (op < 65) {  // vanish without detaching (a crashed UI)
            p.client.reset();
            p.attached.clear();
            continue;
        } else if (op < 72) {  // stall: stop reading for a while
            p.stall = r.range(2, 10);
        } else if (op < 90) {  // checkpoint
            if (p.attached[id]) check_screen(p, id, f, "checkpoint");
        } else {  // input that changes nothing on screen: focus reports
            if (p.attached[id]) p.client->focus(id, r.chance(50));
        }
        // Everyone else keeps up (or not, when stalled).
        for (Player& q : players) {
            if (!q.client) continue;
            if (q.stall > 0) {
                --q.stall;
                continue;
            }
            std::vector<ClientEvent> evs;
            q.client->dispatch(evs);
            for (const ClientEvent& e : evs) {
                if (e.kind == ClientEvent::Kind::Detached) q.attached[e.session] = false;
                CHECK_MSG(e.kind != ClientEvent::Kind::Error || e.code == ErrorCode::NotAttached ||
                              e.code == ErrorCode::ReadOnly,
                          q.name + ": " + e.text);
                CHECK_MSG(e.kind != ClientEvent::Kind::Disconnected, q.name + ": " + e.text);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(r.range(0, 15)));
    }
}

int steps_scale() {
    const char* s = std::getenv("BROMUX_ORACLE_STEPS");
    return s ? std::max(1, std::atoi(s)) : 1;
}

void test_vt_churn() {
    check::phase("random VT streams under churn", 600);
    fx::Fixture f("oracle-vt", g_child);
    auto admin = f.client("admin");
    if (!admin) return;
    std::vector<uint64_t> sessions;
    for (int i = 0; i < 3; ++i) {
        // Each program trickles a random stream for several seconds.
        SessionSpec s = f.spec({"vt", std::to_string(1000 + i), "40000", "600", "6"}, 40 + 30 * i, 12 + 6 * i);
        auto info = admin->create_session(s);
        CHECK(info.has_value());
        if (info) sessions.push_back(info->id);
    }
    if (sessions.empty()) return;
    std::vector<Player> players(3);
    for (size_t i = 0; i < players.size(); ++i) {
        players[i].name = "p" + std::to_string(i);
        players[i].client = f.client(players[i].name);
    }
    churn(f, players, sessions, 700 * steps_scale(), 42);

    check::phase("random VT streams: final state", 120);
    // Let the programs finish their streams, then everything must agree.
    std::this_thread::sleep_for(500ms);
    for (Player& p : players) {
        if (!p.client) p.client = f.client(p.name);
        for (uint64_t id : sessions) {
            auto info = p.client->attach(id, 80, 24);
            CHECK(info.has_value());
            check_screen(p, id, f, "final");
        }
    }
    for (uint64_t id : sessions) check_history(*players[0].client, id, f);
}

// A real shell producing a lot of real output while clients churn.
void test_shell_churn() {
    check::phase("real shell under churn", 600);
    fx::Fixture f("oracle-shell", g_child);
    auto admin = f.client("admin");
    if (!admin) return;
    SessionSpec s;
    s.cols = 100;
    s.rows = 30;
    s.scrollback_rows = 3000;
#if defined(_WIN32)
    s.command = "cmd.exe";
    const std::string work =
        "for /L %i in (1,1,1500) do @echo [3%i line %i of output with some text to wrap around the screen edge\r";
    const std::string done = "echo SHELL-DONE\r";
#else
    s.command = "/bin/sh";
    const std::string work =
        "i=0; while [ $i -lt 1500 ]; do printf '\\033[3%dmline %d\\033[0m of output with some text to wrap around "
        "the screen edge\\n' $((i % 8)) $i; i=$((i+1)); done\r";
    const std::string done = "echo SHELL-DONE\r";
#endif
    auto info = admin->create_session(s);
    CHECK(info.has_value());
    if (!info) return;
    const uint64_t id = info->id;
    std::vector<Player> players(2);
    for (size_t i = 0; i < players.size(); ++i) {
        players[i].name = "s" + std::to_string(i);
        players[i].client = f.client(players[i].name);
        CHECK(players[i].client->attach(id, 100, 30).has_value());
        players[i].attached[id] = true;
    }
    int typed = 0;
    churn(f, players, {id}, 300 * steps_scale(), 7, [&](int step) {
        // Commands go in through whichever player is attached.
        if (step % 60 != 5 || typed >= 3) return;
        for (Player& p : players) {
            if (p.client && p.attached[id]) {
                p.client->send_raw(id, work);
                ++typed;
                return;
            }
        }
    });
    check::phase("real shell: final state", 180);
    Player& p = players[0];
    if (!p.client) p.client = f.client(p.name);
    CHECK(p.client->attach(id, 100, 30).has_value());
    p.client->send_raw(id, done);
    const bool finished = fx::pump_until(*p.client, [&] {
        const ScreenModel* m = p.client->screen(id);
        if (!m) return false;
        int count = 0;
        for (int y = 0; y < m->rows(); ++y) count += m->row(y).text().find("SHELL-DONE") != std::string::npos;
        return count >= 2;  // the command line and its output
    }, 120s);
    CHECK_MSG(finished, fx::screen_dump(p.client->screen(id)));
    check_screen(p, id, f, "shell final");
    check_history(*p.client, id, f);
}

#if !defined(_WIN32)
// A full-screen program when the machine has one (vi), driven and resized.
void test_fullscreen_program() {
    const char* candidates[] = {"/usr/bin/vim", "/usr/bin/vi", "/bin/vi"};
    std::string vi;
    for (const char* c : candidates)
        if (::access(c, X_OK) == 0) {
            vi = c;
            break;
        }
    if (vi.empty()) {
        std::printf("   (no vi here; skipped)\n");
        return;
    }
    check::phase("full-screen program", 120);
    fx::Fixture f("oracle-vi", g_child);
    auto c = f.client();
    if (!c) return;
    SessionSpec s;
    s.command = vi;
    s.args = {"-u", "NONE", "-N"};
    s.cols = 90;
    s.rows = 30;
    auto info = c->create_session(s);
    CHECK(info.has_value());
    if (!info) return;
    const uint64_t id = info->id;
    CHECK(c->attach(id, 90, 30).has_value());
    std::this_thread::sleep_for(500ms);
    c->send_raw(id, "i");
    for (int i = 0; i < 60; ++i) c->send_raw(id, "hello from vi line " + std::to_string(i) + "\r");
    c->send_raw(id, "\x1b");
    CHECK(fx::pump_until(*c, [&] { return fx::screen_has(c->screen(id), "hello from vi line 59"); }, 20s));
    const int sizes[][2] = {{60, 20}, {120, 40}, {33, 9}, {90, 30}};
    for (auto& sz : sizes) {
        c->resize(id, sz[0], sz[1]);
        std::this_thread::sleep_for(300ms);  // vi redraws for the new size
        check_screen(*c, "vi", id, f, "after resize");
    }
    c->send_raw(id, ":q!\r");
}
#endif

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: test_oracle <mux_child>\n");
        return 2;
    }
    g_child = std::filesystem::absolute(argv[1]).string();
    check::start_watchdog("test_oracle");
    test_vt_churn();
    test_shell_churn();
#if !defined(_WIN32)
    test_fullscreen_program();
#endif
    std::printf("   %d screens compared, %d mismatches\n", g_checkpoints, g_mismatches);
    return check::finish("test_oracle");
}
