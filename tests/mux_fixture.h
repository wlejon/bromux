#pragma once
// An in-process server on a private endpoint with session recordings
// (tee files) for the oracle, plus helpers to drive clients.

#include "check.h"
#include "oracle.h"

#include <bromux/client.h>
#include <bromux/paths.h>
#include <bromux/server.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace fx {

using namespace std::chrono_literals;

inline std::string unique_name(const char* test) {
    return std::string(test) + "-" + std::to_string(bromux::current_pid()) + "-" +
           std::to_string(bromux::unix_time_ms() % 100000);
}

inline std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "%08X", unsigned(v));
    return b;
}

struct Fixture {
    std::string name;
    std::string address;
    std::string tee_dir;
    std::string child;  // the mux_child helper
    std::unique_ptr<bromux::Server> server;
    std::thread thread;

    Fixture(const char* test, std::string child_path, bromux::ServerOptions opt = {}) : child(std::move(child_path)) {
        name = unique_name(test);
        std::string err;
        address = bromux::server_address(name, &err);
        CHECK_MSG(!address.empty(), err);
        tee_dir = (std::filesystem::temp_directory_path() / ("bromux-" + name)).string();
        std::filesystem::create_directories(tee_dir);
        opt.address = address;
        opt.tee_dir = tee_dir;
        opt.idle_exit = std::chrono::milliseconds(-1);
        if (std::getenv("BROMUX_TEST_LOG"))
            opt.log = [](std::string_view m) { std::printf("   [server] %.*s\n", int(m.size()), m.data()); };
        server = std::make_unique<bromux::Server>(opt);
        bool in_use = false;
        const bool started = server->start(&err, &in_use);
        CHECK_MSG(started, err);
        if (started) thread = std::thread([this] { server->run(); });
    }
    ~Fixture() {
        // A session program that crashed or never started (0xC0000142: its
        // DLLs failed to initialise) fails the test.
        if (thread.joinable()) {
            if (auto c = client("fixture-teardown"))
                if (auto list = c->list_sessions())
                    for (const bromux::SessionInfo& s : *list)
                        CHECK_MSG(s.running || !check::is_crash_status(s.exit_code),
                                  "session " + std::to_string(s.id) + " (" + s.command + ") died with status 0x" +
                                      hex(uint32_t(s.exit_code)));
        }
        server->stop();
        if (thread.joinable()) thread.join();
        server.reset();
        std::error_code ec;
        std::filesystem::remove_all(tee_dir, ec);
    }

    std::unique_ptr<bromux::Client> client(const std::string& client_name = "test", bool watch = false) {
        bromux::ConnectOptions o;
        o.address = address;
        o.autostart = false;
        o.client_name = client_name;
        o.watch_sessions = watch;
        std::string err;
        auto c = bromux::Client::connect(o, &err);
        CHECK_MSG(c != nullptr, "connect: " + err);
        return c;
    }

    bromux::SessionSpec spec(std::vector<std::string> args, int cols = 80, int rows = 24) const {
        bromux::SessionSpec s;
        s.command = child;
        s.args = std::move(args);
        s.cols = uint16_t(cols);
        s.rows = uint16_t(rows);
        s.scrollback_rows = 2000;
        return s;
    }

    std::string tee(uint64_t session) const { return tee_dir + "/session-" + std::to_string(session) + ".tee"; }
};

// Dispatch until `pred` holds (checked after every dispatch) or the timeout.
inline bool pump_until(bromux::Client& c, const std::function<bool()>& pred, std::chrono::milliseconds timeout,
                       std::vector<bromux::ClientEvent>* events = nullptr) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    std::vector<bromux::ClientEvent> evs;
    for (;;) {
        evs.clear();
        c.dispatch(evs);
        if (events) events->insert(events->end(), evs.begin(), evs.end());
        if (pred()) return true;
        if (std::chrono::steady_clock::now() >= until) return false;
        c.wait(20ms);
    }
}

inline bool screen_has(const bromux::ScreenModel* m, std::string_view text) {
    if (!m) return false;
    for (int y = 0; y < m->rows(); ++y)
        if (m->row(y).text().find(text) != std::string::npos) return true;
    return false;
}

inline std::string screen_dump(const bromux::ScreenModel* m) {
    std::string s;
    if (!m) return "(no screen)";
    for (int y = 0; y < m->rows(); ++y) s += "   |" + m->row(y).text() + "\n";
    return s;
}

// Sync the session's model to the server's state and compare it with the
// session's recording replayed to the same point.
inline std::string verify(bromux::Client& c, uint64_t session, const Fixture& f) {
    std::string err;
    if (!c.sync(session, &err)) return "sync: " + err;
    const bromux::ScreenModel* m = c.screen(session);
    if (!m) return "no screen";
    return oracle::compare_with_tee(*m, f.tee(session));
}

}  // namespace fx
