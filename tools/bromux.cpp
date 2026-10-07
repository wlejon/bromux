// bromux: the server, the proxy for remote attach, and a small CLI.
//
//   bromux server [-L name | --socket addr] [--daemon] [--idle-exit SEC]
//                 [--log FILE] [--tee-dir DIR] [--clipboard deny|write|readwrite]
//   bromux proxy  [-L name | --socket addr]          (the remote end of ssh)
//   bromux ls     [target]
//   bromux new    [target] [--name N] [--cwd DIR] [--size COLSxROWS] [-- command args...]
//   bromux attach [target] [--read-only] <session>
//   bromux kill   [target] <session>
//   bromux kill-server [target]
//   bromux version
// where target is -L name, --socket addr, or --ssh [user@]host
// [--remote-bromux PATH] (plus repeated --ssh-arg ARG) for a remote server.
#include "cli.h"

#include <bromux/client.h>
#include <bromux/paths.h>
#include <bromux/server.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifndef BROMUX_VERSION_STRING
#define BROMUX_VERSION_STRING "0.0.0"
#endif

namespace {

using namespace bromux;
using namespace bromux::cli;

int usage() {
    std::fprintf(stderr,
                 "usage: bromux <command> [options]\n"
                 "  server [-L name|--socket addr] [--daemon] [--idle-exit SEC] [--log FILE] [--tee-dir DIR]\n"
                 "         [--clipboard deny|write|readwrite]\n"
                 "  proxy  [-L name|--socket addr]\n"
                 "  ls     [target]\n"
                 "  new    [target] [--name N] [--cwd DIR] [--rm] [--size COLSxROWS] [-- command args...]\n"
                 "  attach [target] [--read-only] [session]\n"
                 "  kill   [target] <session>\n"
                 "  kill-server [target]\n"
                 "  version\n"
                 "target: -L name | --socket addr | --ssh [user@]host [--remote-bromux PATH] [--ssh-arg ARG]...\n");
    return 2;
}

Server* g_server = nullptr;

#if defined(_WIN32)
BOOL WINAPI on_console_ctrl(DWORD) {
    if (g_server) g_server->stop();
    return TRUE;
}
#else
void on_signal(int) {
    if (g_server) g_server->stop();
}

// Classic double fork: the caller's child returns at once (the spawner reaps
// it), the server runs in its own session with no controlling terminal.
void daemonize(const std::string& log_path) {
    pid_t pid = ::fork();
    if (pid < 0) std::exit(1);
    if (pid > 0) ::_exit(0);
    ::setsid();
    pid = ::fork();
    if (pid < 0) std::exit(1);
    if (pid > 0) ::_exit(0);
    if (::chdir("/") != 0) {
    }
    ::umask(077);
    int in = ::open("/dev/null", O_RDONLY);
    int out = log_path.empty() ? ::open("/dev/null", O_WRONLY)
                               : ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (in >= 0) ::dup2(in, 0);
    if (out >= 0) {
        ::dup2(out, 1);
        ::dup2(out, 2);
    }
    if (in > 2) ::close(in);
    if (out > 2) ::close(out);
}
#endif

int cmd_server(Args& a) {
    ServerOptions opt;
    opt.decode_image = image_decoder();
    std::string name;
    std::string log_path;
    bool daemon = false;
    while (!a.done()) {
        std::string s = a.next();
        if (s == "-L") name = a.value(s);
        else if (s == "--socket") opt.address = a.value(s);
        else if (s == "--daemon") daemon = true;
        else if (s == "--idle-exit")
            opt.idle_exit = std::chrono::milliseconds(static_cast<long long>(std::atof(a.value(s).c_str()) * 1000));
        else if (s == "--log") log_path = a.value(s);
        else if (s == "--tee-dir") opt.tee_dir = a.value(s);
        else if (s == "--clipboard") {
            std::string p = a.value(s);
            opt.clipboard_policy = p == "deny" ? ClipboardPolicy::Deny
                                   : p == "readwrite" ? ClipboardPolicy::ReadWrite
                                                      : ClipboardPolicy::WriteOnly;
        } else {
            std::fprintf(stderr, "bromux server: unknown option %s\n", s.c_str());
            return 2;
        }
    }
    if (a.failed()) return 2;
    std::string err;
    if (opt.address.empty()) opt.address = server_address(name, &err);
    if (opt.address.empty()) {
        std::fprintf(stderr, "bromux server: %s\n", err.c_str());
        return 1;
    }
    if (daemon && log_path.empty()) {
        std::string dir = runtime_dir();
        std::string sname = name.empty() ? std::string(kDefaultServerName) : name;
        if (!dir.empty() && valid_server_name(sname)) log_path = dir + "/" + sname + ".log";
    }
#if !defined(_WIN32)
    std::signal(SIGPIPE, SIG_IGN);
    if (daemon) {
        daemonize(log_path);
        std::signal(SIGHUP, SIG_IGN);
    }
    std::signal(SIGTERM, on_signal);
    std::signal(SIGINT, on_signal);
#else
    SetConsoleCtrlHandler(on_console_ctrl, TRUE);
    // A detached server has no one to answer a modal error box: a fault
    // must end it (WER still records it), not hang it and its sessions.
    if (daemon) SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    auto log_mu = std::make_shared<std::mutex>();
    std::shared_ptr<std::FILE> log_file;
    if (!log_path.empty()) {
#if !defined(_WIN32)
        int fd = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (fd >= 0) {
            log_file.reset(::fdopen(fd, "a"), [](std::FILE* f) {
                if (f) std::fclose(f);
            });
        }
#else
        log_file.reset(std::fopen(log_path.c_str(), "a"), [](std::FILE* f) {
            if (f) std::fclose(f);
        });
#endif
    }
    std::FILE* sink = log_file ? log_file.get() : (daemon ? nullptr : stderr);
    if (sink) {
        opt.log = [sink, log_file, log_mu](std::string_view msg) {
            std::lock_guard<std::mutex> lk(*log_mu);
            std::time_t t = std::time(nullptr);
            std::tm tm_buf{};
#if defined(_WIN32)
            localtime_s(&tm_buf, &t);
#else
            localtime_r(&t, &tm_buf);
#endif
            char ts[32];
            std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm_buf);
            std::fprintf(sink, "%s [%llu] %.*s\n", ts, (unsigned long long)current_pid(), int(msg.size()), msg.data());
            std::fflush(sink);
        };
    }
    Server server(opt);
    bool in_use = false;
    if (!server.start(&err, &in_use)) {
        if (in_use) {
            if (opt.log) opt.log("another server owns " + opt.address + "; exiting");
            return 0;  // lost a start race: the other server serves
        }
        std::fprintf(stderr, "bromux server: %s\n", err.c_str());
        if (opt.log) opt.log(err);
        return 1;
    }
    g_server = &server;
    server.run();
    g_server = nullptr;
    return 0;
}

int cmd_proxy(Args& a) {
    Target t;
    if (!parse_target(a, t, nullptr) || a.failed()) return 2;
    if (t.remote) {
        std::fprintf(stderr, "bromux proxy: --ssh makes no sense here\n");
        return 2;
    }
#if !defined(_WIN32)
    std::signal(SIGPIPE, SIG_IGN);
#endif
    ConnectOptions o = t.options;
    o.server_exe = current_executable();
    std::string err;
    int rc = run_proxy(o, &err);
    if (rc != 0) std::fprintf(stderr, "bromux proxy: %s\n", err.c_str());
    return rc;
}

int cmd_ls(Args& a) {
    Target t;
    if (!parse_target(a, t, nullptr) || a.failed()) return 2;
    std::string err;
    auto c = connect_target(t, false, &err);
    if (!c) {
        std::fprintf(stderr, "bromux: %s\n", err.c_str());
        return 1;
    }
    auto list = c->list_sessions(&err);
    if (!list) {
        std::fprintf(stderr, "bromux: %s\n", err.c_str());
        return 1;
    }
    for (const SessionInfo& s : *list) {
        std::string name = s.meta_value("name");
        std::printf("%llu\t%s\t%ux%u\t%s\tclients=%u\t%s\t%s\n", (unsigned long long)s.id,
                    name.empty() ? "-" : name.c_str(), s.cols, s.rows,
                    s.running ? "running" : ("exited(" + std::to_string(s.exit_code) + ")").c_str(), s.clients,
                    s.command.empty() ? "(shell)" : s.command.c_str(), s.title.c_str());
    }
    return 0;
}

int cmd_new(Args& a) {
    Target t;
    SessionSpec spec;
    auto extra = [&](const std::string& s) {
        if (s == "--name") spec.meta.emplace_back("name", a.value(s));
        else if (s == "--cwd") spec.cwd = a.value(s);
        else if (s == "--rm" || s == "--remove-on-exit") spec.remove_on_exit = true;
        else if (s == "--size") {
            int c = 0, r = 0;
            if (std::sscanf(a.value(s).c_str(), "%dx%d", &c, &r) != 2 || c <= 0 || r <= 0) return false;
            spec.cols = uint16_t(c);
            spec.rows = uint16_t(r);
        } else if (s == "--") {
            if (!a.done()) spec.command = a.next();
            while (!a.done()) spec.args.push_back(a.next());
        } else {
            return false;
        }
        return true;
    };
    if (!parse_target(a, t, extra) || a.failed()) return 2;
    std::string err;
    auto c = connect_target(t, true, &err);
    if (!c) {
        std::fprintf(stderr, "bromux: %s\n", err.c_str());
        return 1;
    }
    auto info = c->create_session(spec, &err);
    if (!info) {
        std::fprintf(stderr, "bromux: %s\n", err.c_str());
        return 1;
    }
    std::printf("%llu\n", (unsigned long long)info->id);
    return 0;
}

int cmd_kill(Args& a) {
    Target t;
    std::string id;
    auto extra = [&](const std::string& s) {
        if (!s.empty() && s[0] != '-' && id.empty()) {
            id = s;
            return true;
        }
        return false;
    };
    if (!parse_target(a, t, extra) || a.failed() || id.empty()) return usage();
    std::string err;
    auto c = connect_target(t, false, &err);
    if (!c || !c->close_session(std::strtoull(id.c_str(), nullptr, 10), &err)) {
        std::fprintf(stderr, "bromux: %s\n", err.c_str());
        return 1;
    }
    return 0;
}

int cmd_kill_server(Args& a) {
    Target t;
    if (!parse_target(a, t, nullptr) || a.failed()) return 2;
    std::string err;
    auto c = connect_target(t, false, &err);
    if (!c) {
        std::fprintf(stderr, "bromux: %s\n", err.c_str());
        return 1;
    }
    c->kill_server();
    c->ping();  // returns once the server has hung up (or answered first)
    return 0;
}

}  // namespace

namespace bromux::cli {

bool parse_target(Args& a, Target& t, const std::function<bool(const std::string&)>& extra) {
    while (!a.done()) {
        std::string s = a.next();
        if (s == "-L") t.options.server_name = a.value(s);
        else if (s == "--socket") t.options.address = a.value(s);
        else if (s == "--ssh") {
            t.remote = true;
            t.ssh.host = a.value(s);
        } else if (s == "--remote-bromux") t.ssh.remote_bromux = a.value(s);
        else if (s == "--ssh-arg") t.ssh.ssh_args.push_back(a.value(s));
        else if (!extra || !extra(s)) {
            std::fprintf(stderr, "bromux: unexpected argument %s\n", s.c_str());
            return false;
        }
    }
    t.ssh.server_name = t.options.server_name;
    t.options.client_name = "bromux-cli";
    return true;
}

std::unique_ptr<Client> connect_target(const Target& t, bool autostart, std::string* err) {
    ConnectOptions o = t.options;
    o.autostart = autostart;
    if (t.remote) return Client::connect_ssh(t.ssh, o, err);
    return Client::connect(o, err);
}

}  // namespace bromux::cli

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    std::string cmd = argv[1];
    bromux::cli::Args a(argc, argv, 2);
    if (cmd == "server") return cmd_server(a);
    if (cmd == "proxy") return cmd_proxy(a);
    if (cmd == "ls" || cmd == "list") return cmd_ls(a);
    if (cmd == "new") return cmd_new(a);
    if (cmd == "attach") return bromux::cli::cmd_attach(a);
    if (cmd == "kill") return cmd_kill(a);
    if (cmd == "kill-server") return cmd_kill_server(a);
    if (cmd == "version" || cmd == "--version") {
        std::printf("bromux %s (protocol %u.%u)\n", BROMUX_VERSION_STRING, unsigned(kProtocolMajor),
                    unsigned(kProtocolMinor));
        return 0;
    }
    return usage();
}
