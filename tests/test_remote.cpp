// Remote attach over the real system ssh: `ssh host bromux proxy` as the
// transport, with the same oracle as the local tests -- the remote server
// records every session (tee files), and each client's screen must equal,
// cell for cell, a bropty Terminal replaying the recording fetched back over
// ssh. Clients churn: attach, resize, detach, and their ssh processes are
// killed under them (a dropped connection), then they reconnect.
//
// Skipped (exit 77) unless the environment names the remote:
//   BROMUX_TEST_SSH         [user@]host
//   BROMUX_TEST_SSH_BROMUX  the bromux executable on the remote
//   BROMUX_TEST_SSH_CHILD   mux_child on the remote
//   BROMUX_TEST_SSH_ARGS    optional extra ssh arguments, space separated
#include "mux_fixture.h"
#include "vtgen.h"

#include <bromux/stream.h>
#include <bromux/tee.h>

#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

using namespace bromux;
using namespace std::chrono_literals;

namespace {

SshTarget g_target;
std::string g_child;
std::string g_name;
std::string g_tee_dir;
std::string g_local_dir;

std::string env(const char* k) {
    const char* v = std::getenv(k);
    return v ? v : "";
}

// Run a command on the remote; its stdout.
std::string remote(const std::vector<std::string>& cmd, bool* ok = nullptr) {
    std::vector<std::string> argv{g_target.ssh_program};
    argv.insert(argv.end(), g_target.ssh_args.begin(), g_target.ssh_args.end());
    argv.push_back("-T");
    argv.push_back(g_target.host);
    argv.insert(argv.end(), cmd.begin(), cmd.end());
    std::string err;
    auto s = spawn_stream(argv, &err);
    if (ok) *ok = s != nullptr;
    CHECK_MSG(s != nullptr, err);
    if (!s) return {};
    std::string out;
    char buf[1 << 16];
    for (size_t n; (n = s->read(buf, sizeof buf)) > 0;) out.append(buf, n);
    return out;
}

std::unique_ptr<Client> connect(const std::string& who) {
    ConnectOptions o;
    o.client_name = who;
    o.timeout = 30s;
    std::string err;
    auto c = Client::connect_ssh(g_target, o, &err);
    CHECK_MSG(c != nullptr, who + ": " + err);
    return c;
}

// Fetch a session's recording from the remote into a local file.
std::string fetch_tee(uint64_t id) {
    const std::string name = "session-" + std::to_string(id) + ".tee";
    const std::string data = remote({"cat", g_tee_dir + "/" + name});
    const std::string local = g_local_dir + "/" + name;
    std::ofstream f(local, std::ios::binary | std::ios::trunc);
    f.write(data.data(), std::streamsize(data.size()));
    return local;
}

void check_screen(Client& c, uint64_t id, const std::string& who) {
    std::string err;
    const bool synced = c.sync(id, &err);
    CHECK_MSG(synced, who + ": sync: " + err);
    if (!synced) return;
    const ScreenModel* m = c.screen(id);
    CHECK(m != nullptr);
    if (!m) return;
    const std::string d = oracle::compare_with_tee(*m, fetch_tee(id));
    CHECK_MSG(d.empty(), who + " session " + std::to_string(id) + ": " + d);
}

void check_history(Client& c, uint64_t id) {
    std::string err;
    auto h = c.fetch_history(id, 0, 100000, &err);
    CHECK_MSG(h.has_value(), err);
    if (!h) return;
    TeeHeader th;
    std::vector<TeeRecord> recs;
    CHECK(read_tee(fetch_tee(id), th, recs, &err));
    if (recs.size() < h->feed_seq) {
        CHECK_MSG(false, "tee shorter than the history's feed_seq");
        return;
    }
    auto t = replay_tee(th, recs, size_t(h->feed_seq));
    const std::string d = oracle::compare_history(*h, *t);
    CHECK_MSG(d.empty(), "history: " + d);
}

SessionSpec spec(std::vector<std::string> args, int cols, int rows) {
    SessionSpec s;
    s.command = g_child;
    s.args = std::move(args);
    s.cols = uint16_t(cols);
    s.rows = uint16_t(rows);
    s.scrollback_rows = 2000;
    return s;
}

void test_basics() {
    check::phase("remote basics", 180);
    auto c = connect("basics");
    if (!c) return;
    CHECK(c->server_info().pid != 0);
    // Round trip through ssh.
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 10; ++i) CHECK(c->ping());
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("   ping round trip over ssh: %.1f ms\n", ms / 10);
    auto echo = c->create_session(spec({"echo"}, 80, 24));
    CHECK(echo.has_value());
    if (!echo) return;
    CHECK(c->attach(echo->id, 80, 24).has_value());
    c->send_text(echo->id, "over-ssh");
    CHECK(fx::pump_until(*c, [&] { return fx::screen_has(c->screen(echo->id), "[over-ssh]"); }, 15s));
    check_screen(*c, echo->id, "basics");
    CHECK(c->close_session(echo->id));
}

void test_churn() {
    check::phase("remote churn", 900);
    auto admin = connect("admin");
    if (!admin) return;
    std::vector<uint64_t> sessions;
    for (int i = 0; i < 2; ++i) {
        auto info = admin->create_session(spec({"vt", std::to_string(500 + i), "20000", "600", "8"}, 50 + 40 * i, 15 + 8 * i));
        CHECK(info.has_value());
        if (info) sessions.push_back(info->id);
    }
    auto lines = admin->create_session(spec({"lines", "3000", "1"}, 80, 24));
    CHECK(lines.has_value());
    if (lines) sessions.push_back(lines->id);
    if (sessions.size() != 3) return;

    struct Player {
        std::string name;
        std::unique_ptr<Client> client;
        std::map<uint64_t, bool> attached;
    };
    std::vector<Player> players(2);
    players[0].name = "alice";
    players[1].name = "bob";
    for (Player& p : players) p.client = connect(p.name);

    vtgen::Rng r(77);
    int screens = 0;
    for (int step = 0; step < 160; ++step) {
        Player& p = players[size_t(r.range(0, 1))];
        const uint64_t id = sessions[size_t(r.range(0, 2))];
        const int op = r.range(0, 99);
        if (!p.client) {
            p.client = connect(p.name);
            p.attached.clear();
            continue;
        }
        if (op < 30) {
            auto info = p.client->attach(id, r.range(20, 140), r.range(5, 50));
            CHECK(info.has_value());
            p.attached[id] = info.has_value();
        } else if (op < 50) {
            if (p.attached[id]) p.client->resize(id, r.range(20, 140), r.range(5, 50));
        } else if (op < 60) {
            if (p.attached[id]) {
                p.client->detach(id);
                p.attached[id] = false;
            }
        } else if (op < 66) {  // the connection drops: ssh dies under the client
            p.client.reset();
            continue;
        } else if (op < 85) {
            if (p.attached[id]) {
                check_screen(*p.client, id, p.name);
                ++screens;
            }
        }
        for (Player& q : players) {
            if (!q.client) continue;
            std::vector<ClientEvent> evs;
            q.client->dispatch(evs);
            for (const ClientEvent& e : evs) {
                if (e.kind == ClientEvent::Kind::Detached) q.attached[e.session] = false;
                CHECK_MSG(e.kind != ClientEvent::Kind::Disconnected, q.name + ": " + e.text);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(r.range(0, 30)));
    }
    // Final state (any synced point must agree; the line printer is also
    // waited out, so its whole history is compared).
    for (uint64_t id : sessions) {
        CHECK(admin->attach(id, 90, 30).has_value());
        if (id == lines->id) {
            const bool done =
                fx::pump_until(*admin, [&] { return fx::screen_has(admin->screen(id), "LINES DONE"); }, 120s);
            CHECK_MSG(done, fx::screen_dump(admin->screen(id)));
        }
        check_screen(*admin, id, "admin");
        check_history(*admin, id);
        ++screens;
    }
    std::printf("   %d screens compared over ssh\n", screens);
    for (uint64_t id : sessions) CHECK(admin->close_session(id));
}

}  // namespace

int main() {
    g_target.host = env("BROMUX_TEST_SSH");
    g_target.remote_bromux = env("BROMUX_TEST_SSH_BROMUX");
    g_child = env("BROMUX_TEST_SSH_CHILD");
    if (g_target.host.empty() || g_target.remote_bromux.empty() || g_child.empty()) {
        std::printf("test_remote: set BROMUX_TEST_SSH, BROMUX_TEST_SSH_BROMUX and BROMUX_TEST_SSH_CHILD; skipped\n");
        return 77;
    }
    {
        std::istringstream a(env("BROMUX_TEST_SSH_ARGS"));
        for (std::string w; a >> w;) g_target.ssh_args.push_back(w);
        g_target.ssh_args.push_back("-oBatchMode=yes");
    }
    check::start_watchdog("test_remote");
    g_name = fx::unique_name("remote");
    g_target.server_name = g_name;
    g_tee_dir = "/tmp/bromux-test-" + g_name;
    g_local_dir = (std::filesystem::temp_directory_path() / ("bromux-" + g_name)).string();
    std::filesystem::create_directories(g_local_dir);

    // A remote server that records its sessions (the proxy would autostart
    // one, but without recordings).
    bool ok = false;
    remote({"mkdir", "-p", g_tee_dir, "&&", g_target.remote_bromux, "server", "--daemon", "-L", g_name, "--tee-dir",
            g_tee_dir, "--idle-exit", "20"},
           &ok);
    if (ok) {
        test_basics();
        test_churn();
        if (auto c = connect("cleanup")) {
            c->kill_server();
            c->ping();
        }
    }
    remote({"rm", "-rf", g_tee_dir});
    std::error_code ec;
    std::filesystem::remove_all(g_local_dir, ec);
    return check::finish("test_remote");
}
