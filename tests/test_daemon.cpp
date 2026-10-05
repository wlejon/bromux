// The daemon's lifecycle through the real executable: auto-start on first
// connect, sessions outliving their clients, idle exit, start races (one
// winner), a server killed hard (its endpoint reclaimed), the proxy end of
// remote attach run locally, and the CLI.
// argv[1]: bromux, argv[2]: mux_child.
#include "mux_fixture.h"

#include <bromux/stream.h>

using namespace bromux;
using namespace std::chrono_literals;

namespace {

std::string g_bromux;
std::string g_child;

ConnectOptions opts(const std::string& name, bool autostart, std::vector<std::string> server_args = {}) {
    ConnectOptions o;
    o.server_name = name;
    o.autostart = autostart;
    o.server_exe = g_bromux;
    o.server_args = std::move(server_args);
    o.client_name = "daemon-test";
    return o;
}

bool reachable(const std::string& name) {
    std::string err;
    return Client::connect(opts(name, false), &err) != nullptr;
}

// A probe is a connection, and a connection restarts the server's idle timer:
// waiting for an idle exit needs `every` longer than the idle timeout.
bool wait_unreachable(const std::string& name, std::chrono::milliseconds t,
                      std::chrono::milliseconds every = 100ms) {
    const auto until = std::chrono::steady_clock::now() + t;
    while (std::chrono::steady_clock::now() < until) {
        if (!reachable(name)) return true;
        std::this_thread::sleep_for(every);
    }
    return false;
}

bool wait_reachable(const std::string& name, std::chrono::milliseconds t) {
    const auto until = std::chrono::steady_clock::now() + t;
    while (std::chrono::steady_clock::now() < until) {
        if (reachable(name)) return true;
        std::this_thread::sleep_for(50ms);
    }
    return false;
}

SessionSpec lines(int n) {
    SessionSpec s;
    s.command = g_child;
    s.args = {"lines", std::to_string(n)};
    return s;
}

std::string run_capture(const std::vector<std::string>& argv) {
    std::string err;
    auto s = spawn_stream(argv, &err);
    CHECK_MSG(s != nullptr, err);
    if (!s) return {};
    std::string out;
    char buf[4096];
    for (size_t n; (n = s->read(buf, sizeof buf)) > 0;) out.append(buf, n);
    return out;
}

void cleanup_log(const std::string& name) {
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(runtime_dir()) / (name + ".log"), ec);
}

void test_autostart_persist_idle() {
    check::phase("auto-start, persistence, idle exit", 120);
    const std::string name = fx::unique_name("auto");
    std::string err;
    uint64_t server_pid = 0;
    uint64_t id = 0;
    {
        auto c = Client::connect(opts(name, true, {"--idle-exit", "2"}), &err);
        CHECK_MSG(c != nullptr, err);
        if (!c) return;
        server_pid = c->server_info().pid;
        CHECK(server_pid != 0 && server_pid != current_pid());
        auto info = c->create_session(lines(5));
        CHECK(info.has_value());
        if (!info) return;
        id = info->id;
        CHECK(c->attach(id, 80, 24).has_value());
        CHECK(fx::pump_until(*c, [&] { return fx::screen_has(c->screen(id), "LINES DONE"); }, 10s));
    }  // the client goes away (as a UI that quits or crashes)
    std::this_thread::sleep_for(2500ms);  // past the idle timeout: a running session keeps it alive
    {
        auto c = Client::connect(opts(name, false), &err);
        CHECK_MSG(c != nullptr, err);
        if (!c) return;
        CHECK_EQ(c->server_info().pid, server_pid);
        CHECK(c->attach(id, 80, 24).has_value());
        CHECK(fx::screen_has(c->screen(id), "line 4"));
        CHECK(fx::screen_has(c->screen(id), "LINES DONE"));
        CHECK(c->close_session(id));
    }
    // No sessions, no clients: it exits after the idle timeout.
    CHECK(wait_unreachable(name, 20s, 3000ms));
    cleanup_log(name);
}

void test_start_race() {
    check::phase("start race", 120);
    const std::string name = fx::unique_name("race");
    std::vector<std::unique_ptr<Process>> procs;
    for (int i = 0; i < 4; ++i) {
        std::string err;
        auto p = Process::spawn({g_bromux, "server", "-L", name, "--idle-exit", "-1"}, &err);
        CHECK_MSG(p != nullptr, err);
        if (p) procs.push_back(std::move(p));
    }
    CHECK(wait_reachable(name, 10s));
    // Every loser exits cleanly and quickly; one keeps serving.
    int exited = 0;
    const auto until = std::chrono::steady_clock::now() + 10s;
    while (exited < 3 && std::chrono::steady_clock::now() < until) {
        exited = 0;
        for (auto& p : procs) {
            int code = -1;
            if (p->wait_for(0ms, &code)) {
                ++exited;
                CHECK_EQ(code, 0);
            }
        }
        std::this_thread::sleep_for(50ms);
    }
    CHECK_EQ(exited, 3);
    std::string err;
    auto c = Client::connect(opts(name, false), &err);
    CHECK_MSG(c != nullptr, err);
    if (c) {
        c->kill_server();
        c->ping();
    }
    for (auto& p : procs) CHECK(p->wait_for(10s));
}

void test_crash_reclaim() {
    check::phase("server killed hard", 120);
    const std::string name = fx::unique_name("crash");
    std::string err;
    auto p = Process::spawn({g_bromux, "server", "-L", name, "--idle-exit", "-1"}, &err);
    CHECK_MSG(p != nullptr, err);
    if (!p) return;
    CHECK(wait_reachable(name, 10s));
    {
        auto c = Client::connect(opts(name, false), &err);
        if (c) {
            CHECK(c->create_session(lines(3)).has_value());
            // The client notices the server dying.
            p->kill();
            CHECK(p->wait_for(10s));
            std::vector<ClientEvent> evs;
            bool gone = fx::pump_until(*c, [&] {
                for (const ClientEvent& e : evs)
                    if (e.kind == ClientEvent::Kind::Disconnected) return true;
                return false;
            }, 10s, &evs);
            CHECK(gone);
            CHECK(!c->connected());
        }
    }
    // Nothing listens now (a stale socket file is not a server)...
    bool not_running = false;
    std::string addr = server_address(name);
    CHECK(connect_local(addr, &err, &not_running) == nullptr);
    CHECK(not_running);
    // ...and a new server takes the endpoint over.
    auto c = Client::connect(opts(name, true, {"--idle-exit", "1"}), &err);
    CHECK_MSG(c != nullptr, err);
    if (c) {
        auto list = c->list_sessions();
        CHECK(list && list->empty());
        c->kill_server();
        c->ping();
    }
    cleanup_log(name);
}

void test_local_proxy() {
    check::phase("proxy (remote attach, run locally)", 120);
    const std::string name = fx::unique_name("proxy");
    std::string err;
    // `bromux proxy` starts the server itself when needed.
    auto stream = spawn_stream({g_bromux, "proxy", "-L", name}, &err);
    CHECK_MSG(stream != nullptr, err);
    if (!stream) return;
    ConnectOptions o;
    o.client_name = "via-proxy";
    auto c = Client::connect_stream(std::move(stream), o, &err);
    CHECK_MSG(c != nullptr, err);
    if (!c) return;
    auto info = c->create_session(lines(30));
    CHECK(info.has_value());
    if (!info) return;
    CHECK(c->attach(info->id, 60, 15).has_value());
    CHECK(fx::pump_until(*c, [&] { return fx::screen_has(c->screen(info->id), "LINES DONE"); }, 10s));
    c->resize(info->id, 40, 10);
    CHECK(fx::pump_until(*c, [&] { return c->screen(info->id)->cols() == 40; }, 5s));
    auto h = c->fetch_history(info->id, 0, 1000);
    CHECK(h && h->rows.size() == h->history_rows && h->history_rows > 10);
    c->kill_server();
    c->ping();
    cleanup_log(name);
}

void test_cli() {
    check::phase("command line", 120);
    const std::string name = fx::unique_name("cli");
    std::string out = run_capture({g_bromux, "new", "-L", name, "--name", "clisession", "--", g_child, "lines", "3"});
    const uint64_t id = std::strtoull(out.c_str(), nullptr, 10);
    CHECK_MSG(id > 0, "bromux new printed: " + out);
    out = run_capture({g_bromux, "ls", "-L", name});
    CHECK_MSG(out.find("clisession") != std::string::npos, out);
    run_capture({g_bromux, "kill", "-L", name, std::to_string(id)});
    out = run_capture({g_bromux, "ls", "-L", name});
    CHECK_MSG(out.find("clisession") == std::string::npos, out);
    out = run_capture({g_bromux, "version"});
    CHECK(out.find("protocol " + std::to_string(kProtocolMajor) + ".") != std::string::npos);
    run_capture({g_bromux, "kill-server", "-L", name});
    CHECK(wait_unreachable(name, 10s));
    cleanup_log(name);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: test_daemon <bromux> <mux_child>\n");
        return 2;
    }
    g_bromux = std::filesystem::absolute(argv[1]).string();
    g_child = std::filesystem::absolute(argv[2]).string();
    check::start_watchdog("test_daemon");
    test_autostart_persist_idle();
    test_start_race();
    test_crash_reclaim();
    test_local_proxy();
    test_cli();
    return check::finish("test_daemon");
}
