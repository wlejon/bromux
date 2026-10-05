// A program for sessions to run in tests, behaving the same everywhere:
//   vt SEED N [CHUNK [DELAY_MS]]  random VT stream (vtgen) in chunks, then hold
//   flood BYTES                   colored lines as fast as possible, then hold
//   lines N [DELAY_MS]            N numbered lines, then hold
//   echo                          raw mode; report each input chunk as [escaped]; 'q' quits
//   osc                           on a key: title / cwd / bell / notification / OSC 133 / OSC 52 / link
//   clipread                      raw mode; on a key, an OSC 52 query; prints the answer as GOT:<base64>
//   size                          print SIZE cols rows now and whenever input arrives
//   exit CODE                     print bye and exit with CODE
// "hold" = wait for input to end (the session closing).
#include "no_dialogs.h"
#include "vtgen.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace {

void out(const std::string& s) {
#if defined(_WIN32)
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    size_t off = 0;
    while (off < s.size()) {
        DWORD n = 0;
        if (!WriteFile(h, s.data() + off, DWORD(s.size() - off), &n, nullptr) || n == 0) std::exit(3);
        off += n;
    }
#else
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = ::write(1, s.data() + off, s.size() - off);
        if (n <= 0) std::exit(3);
        off += size_t(n);
    }
#endif
}

size_t in(char* buf, size_t n) {
#if defined(_WIN32)
    DWORD got = 0;
    if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), buf, DWORD(n), &got, nullptr)) return 0;
    return got;
#else
    ssize_t r = ::read(0, buf, n);
    return r > 0 ? size_t(r) : 0;
#endif
}

void setup_console(bool raw) {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
    HANDLE o = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD m = 0;
    GetConsoleMode(o, &m);
    SetConsoleMode(o, m | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);
    if (raw) {
        HANDLE i = GetStdHandle(STD_INPUT_HANDLE);
        SetConsoleMode(i, ENABLE_VIRTUAL_TERMINAL_INPUT);
    }
#else
    if (raw) {
        termios t{};
        if (tcgetattr(0, &t) == 0) {
            cfmakeraw(&t);
            tcsetattr(0, TCSANOW, &t);
        }
    }
#endif
}

void hold() {
    char buf[256];
    while (in(buf, sizeof buf) > 0) {
    }
}

void size_report() {
    int cols = 0, rows = 0;
#if defined(_WIN32)
    CONSOLE_SCREEN_BUFFER_INFO bi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &bi)) {
        cols = bi.srWindow.Right - bi.srWindow.Left + 1;
        rows = bi.srWindow.Bottom - bi.srWindow.Top + 1;
    }
#else
    winsize ws{};
    if (ioctl(0, TIOCGWINSZ, &ws) == 0) {
        cols = ws.ws_col;
        rows = ws.ws_row;
    }
#endif
    out("SIZE " + std::to_string(cols) + " " + std::to_string(rows) + "\r\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const std::string mode = argv[1];
    if (mode == "crash") {
        // Faults before setting any error mode of its own: whether a fault
        // dialog appears is decided by what it inherited from the server.
        std::fputs("crashing\r\n", stdout);
        std::fflush(stdout);
#if defined(_WIN32)
        RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
#else
        std::signal(SIGSEGV, SIG_DFL);
        std::raise(SIGSEGV);
#endif
        return 0;
    }
    check::no_error_dialogs();
    auto arg = [&](int i, long long def) { return argc > i ? std::atoll(argv[i]) : def; };

    if (mode == "vt") {
        setup_console(false);
        const uint64_t seed = uint64_t(arg(2, 1));
        const int n = int(arg(3, 1000));
        const size_t chunk = size_t(arg(4, 4096));
        const int delay = int(arg(5, 0));
        std::string s = vtgen::stream(seed, n, 80, 24);
        for (size_t off = 0; off < s.size(); off += chunk) {
            out(s.substr(off, chunk));
            if (delay) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
        hold();
        return 0;
    }
    if (mode == "flood") {
        setup_console(false);
        const long long total = arg(2, 1 << 20);
        long long sent = 0;
        std::string buf;
        for (long long i = 0; sent < total; ++i) {
            buf.clear();
            for (int k = 0; k < 64 && sent + static_cast<long long>(buf.size()) < total; ++k, ++i) {
                buf += "\x1b[3" + std::to_string(i % 8) + "mflood " + std::to_string(i) +
                       " the quick brown fox jumps over the lazy dog\x1b[0m\r\n";
            }
            out(buf);
            sent += static_cast<long long>(buf.size());
        }
        out("FLOOD DONE\r\n");
        hold();
        return 0;
    }
    if (mode == "lines") {
        setup_console(false);
        const long long n = arg(2, 100);
        const int delay = int(arg(3, 0));
        for (long long i = 0; i < n; ++i) {
            out("line " + std::to_string(i) + "\r\n");
            if (delay) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
        out("LINES DONE\r\n");
        hold();
        return 0;
    }
    if (mode == "echo") {
        setup_console(true);
        out("READY\r\n");
        char buf[4096];
        for (;;) {
            size_t n = in(buf, sizeof buf);
            if (n == 0) return 0;
            std::string s(buf, n);
            std::string esc;
            for (unsigned char c : s) {
                if (c == 0x1b) esc += "\\e";
                else if (c == '\r') esc += "\\r";
                else if (c == '\n') esc += "\\n";
                else if (c < 0x20 || c == 0x7f) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\x%02x", c);
                    esc += b;
                } else esc.push_back(char(c));
            }
            out("[" + esc + "]\r\n");
            if (s == "q") return 0;
        }
    }
    if (mode == "osc") {
        setup_console(true);
        out("OSC READY\r\n");
        char key[64];
        if (in(key, sizeof key) == 0) return 1;  // a key starts it
        out("\x1b]0;the title\x07");
        out("\x1b]7;file://host/some/dir\x1b\\");
        out("\x07");
        out("\x1b]9;notify body\x07");
        out("\x1b]777;notify;Title;Body\x07");
        // ("\x07" "cmd": a hex escape is greedy, "\x07cmd" would be "\x7c" "md".)
        out("\x1b]133;A\x07$ \x1b]133;B\x07" "cmd\r\n\x1b]133;C\x07output\r\n\x1b]133;D;0\x07");
        out("\x1b]52;c;aGVsbG8gY2xpcGJvYXJk\x07");  // "hello clipboard"
        out("\x1b]8;id=x;https://example.com\x1b\\link\x1b]8;;\x1b\\\r\n");
        out("OSC DONE\r\n");
        hold();
        return 0;
    }
    if (mode == "clipread") {
        setup_console(true);
        out("CLIP READY\r\n");
        char buf[1024];
        if (in(buf, sizeof buf) == 0) return 1;  // a key starts the query
        out("\x1b]52;c;?\x07");
        std::string got;
        while (got.find('\x07') == std::string::npos && got.find("\x1b\\") == std::string::npos) {
            size_t n = in(buf, sizeof buf);
            if (n == 0) return 1;
            got.append(buf, n);
        }
        size_t semi = got.rfind(';');
        size_t end = got.find_first_of("\x07\x1b", semi);
        out("GOT:" + got.substr(semi + 1, end - semi - 1) + "\r\n");
        hold();
        return 0;
    }
    if (mode == "size") {
        setup_console(true);
        size_report();
        char buf[256];
        while (in(buf, sizeof buf) > 0) size_report();
        return 0;
    }
    if (mode == "exit") {
        setup_console(false);
        out("bye\r\n");
        return int(arg(2, 0));
    }
    return 2;
}
