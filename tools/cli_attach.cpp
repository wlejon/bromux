// `bromux attach`: a minimal interactive client that draws a session's
// ScreenModel onto the terminal it runs in and forwards keystrokes (as the
// raw bytes the host terminal encodes). Ctrl-] detaches. It exists for
// trying bromux out from any terminal (and over ssh); real UIs embed the
// Client library and render the model themselves.
#include "cli.h"

#include <bropty/cell.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace bromux::cli {

namespace {

constexpr char kDetachKey = 0x1d;  // Ctrl-]

class HostTerminal {
public:
    HostTerminal() {
#if defined(_WIN32)
        in_ = GetStdHandle(STD_INPUT_HANDLE);
        out_ = GetStdHandle(STD_OUTPUT_HANDLE);
        GetConsoleMode(in_, &in_mode_);
        GetConsoleMode(out_, &out_mode_);
        SetConsoleMode(in_, (in_mode_ | ENABLE_VIRTUAL_TERMINAL_INPUT) &
                                ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT));
        SetConsoleMode(out_, out_mode_ | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);
        in_cp_ = GetConsoleCP();
        out_cp_ = GetConsoleOutputCP();
        SetConsoleCP(CP_UTF8);
        SetConsoleOutputCP(CP_UTF8);
#else
        have_termios_ = tcgetattr(0, &saved_) == 0;
        if (have_termios_) {
            termios raw = saved_;
            cfmakeraw(&raw);
            tcsetattr(0, TCSANOW, &raw);
        }
#endif
        write("\x1b[?1049h\x1b[H\x1b[2J");
    }
    ~HostTerminal() { restore(); }
    void restore() {
        if (restored_.exchange(true)) return;
        write("\x1b[0m\x1b[?25h\x1b[?1049l");
#if defined(_WIN32)
        SetConsoleMode(in_, in_mode_);
        SetConsoleMode(out_, out_mode_);
        SetConsoleCP(in_cp_);
        SetConsoleOutputCP(out_cp_);
#else
        if (have_termios_) tcsetattr(0, TCSANOW, &saved_);
#endif
    }
    void size(int& cols, int& rows) const {
        cols = 80;
        rows = 24;
#if defined(_WIN32)
        CONSOLE_SCREEN_BUFFER_INFO bi;
        if (GetConsoleScreenBufferInfo(out_, &bi)) {
            cols = bi.srWindow.Right - bi.srWindow.Left + 1;
            rows = bi.srWindow.Bottom - bi.srWindow.Top + 1;
        }
#else
        winsize ws{};
        if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_row) {
            cols = ws.ws_col;
            rows = ws.ws_row;
        }
#endif
    }
    void write(const std::string& s) const {
#if defined(_WIN32)
        DWORD n = 0;
        WriteFile(out_, s.data(), DWORD(s.size()), &n, nullptr);
#else
        size_t off = 0;
        while (off < s.size()) {
            ssize_t r = ::write(1, s.data() + off, s.size() - off);
            if (r <= 0) break;
            off += size_t(r);
        }
#endif
    }
    // Blocking read of keyboard bytes; 0 at end of input.
    size_t read(char* buf, size_t n) const {
#if defined(_WIN32)
        DWORD got = 0;
        if (!ReadFile(in_, buf, DWORD(n), &got, nullptr)) return 0;
        return got;
#else
        ssize_t r = ::read(0, buf, n);
        return r > 0 ? size_t(r) : 0;
#endif
    }

private:
    std::atomic<bool> restored_{false};
#if defined(_WIN32)
    HANDLE in_{};
    HANDLE out_{};
    DWORD in_mode_{0};
    DWORD out_mode_{0};
    UINT in_cp_{0};
    UINT out_cp_{0};
#else
    termios saved_{};
    bool have_termios_{false};
#endif
};

void color(std::string& out, bropty::Color c, int base, int bright_base, int ext) {
    if (c.is_default()) {
        out += ";" + std::to_string(ext + 1);  // 39 / 49
    } else if (c.is_indexed()) {
        int i = c.index();
        if (i < 8) out += ";" + std::to_string(base + i);
        else if (i < 16) out += ";" + std::to_string(bright_base + i - 8);
        else out += ";" + std::to_string(ext) + ";5;" + std::to_string(i);
    } else {
        bropty::Rgb v = c.rgb_value();
        out += ";" + std::to_string(ext) + ";2;" + std::to_string(v.r) + ";" + std::to_string(v.g) + ";" +
               std::to_string(v.b);
    }
}

std::string sgr(const bropty::Style& s) {
    std::string out = "\x1b[0";
    if (s.has(bropty::Attr_Bold)) out += ";1";
    if (s.has(bropty::Attr_Dim)) out += ";2";
    if (s.has(bropty::Attr_Italic)) out += ";3";
    if (s.underline != bropty::Underline::None) out += ";4";
    if (s.has(bropty::Attr_Blink)) out += ";5";
    if (s.has(bropty::Attr_Inverse)) out += ";7";
    if (s.has(bropty::Attr_Invisible)) out += ";8";
    if (s.has(bropty::Attr_Strike)) out += ";9";
    color(out, s.fg, 30, 90, 38);
    color(out, s.bg, 40, 100, 48);
    return out + "m";
}

void draw(const HostTerminal& host, const ScreenModel& m, bool full) {
    int hcols = 0, hrows = 0;
    host.size(hcols, hrows);
    const int rows = std::min(m.rows(), hrows);
    const int cols = std::min(m.cols(), hcols);
    std::string out = "\x1b[?25l";
    for (int y = 0; y < rows; ++y) {
        if (!full && !m.row_dirty(y)) continue;
        out += "\x1b[" + std::to_string(y + 1) + ";1H";
        bropty::RowView row = m.row(y);
        uint32_t cur_style = UINT32_MAX;
        for (int x = 0; x < cols; ++x) {
            const bropty::Cell& c = row[x];
            if (c.wide() == bropty::Wide::SpacerTail) continue;
            if (c.wide() == bropty::Wide::Lead && x + 1 >= cols) break;  // half a wide char does not fit
            if (c.style != cur_style) {
                cur_style = c.style;
                out += sgr(m.style(c.style));
            }
            if (c.is_empty() || c.wide() == bropty::Wide::SpacerHead) {
                out.push_back(' ');
            } else {
                for (char32_t cp : row.cluster(x)) bropty::append_utf8(out, cp);
            }
        }
        out += "\x1b[0m\x1b[K";
    }
    const bropty::CursorState& cur = m.cursor();
    out += "\x1b[" + std::to_string(cur.row + 1) + ";" + std::to_string(cur.col + 1) + "H";
    if (cur.visible) out += "\x1b[?25h";
    host.write(out);
}

}  // namespace

int cmd_attach(Args& a) {
    Target t;
    std::string id;
    bool read_only = false;
    auto extra = [&](const std::string& s) {
        if (s == "--read-only") read_only = true;
        else if (!s.empty() && s[0] != '-' && id.empty()) id = s;
        else return false;
        return true;
    };
    if (!parse_target(a, t, extra) || a.failed() || id.empty()) {
        std::fprintf(stderr, "usage: bromux attach [target] [--read-only] <session>\n");
        return 2;
    }
    const uint64_t sid = std::strtoull(id.c_str(), nullptr, 10);
    std::string err;
    std::shared_ptr<Client> c(connect_target(t, false, &err));
    if (!c) {
        std::fprintf(stderr, "bromux: %s\n", err.c_str());
        return 1;
    }
    auto host = std::make_shared<HostTerminal>();
    int cols = 0, rows = 0;
    host->size(cols, rows);
    if (!c->attach(sid, cols, rows, read_only ? Attach_ReadOnly : 0, &err)) {
        host->restore();
        std::fprintf(stderr, "bromux: %s\n", err.c_str());
        return 1;
    }
    auto quit = std::make_shared<std::atomic<bool>>(false);
    // Keyboard -> session. Detached: a blocked console read must not hold up exit.
    std::thread([c, host, quit, sid, read_only] {
        char buf[4096];
        while (!*quit) {
            size_t n = host->read(buf, sizeof buf);
            if (n == 0) break;
            std::string bytes(buf, n);
            size_t k = bytes.find(kDetachKey);
            if (k != std::string::npos) bytes.resize(k);
            if (!bytes.empty() && !read_only) c->send_raw(sid, bytes);
            if (k != std::string::npos) break;
        }
        *quit = true;
        c->detach(sid);
    }).detach();

    std::string reason = "detached";
    int exit_code = 0;
    bool full = true;
    std::vector<ClientEvent> evs;
    while (!*quit || c->screen(sid)) {
        c->wait(std::chrono::milliseconds(50));
        evs.clear();
        c->dispatch(evs);
        bool done = false;
        for (const ClientEvent& e : evs) {
            if (e.session != sid && e.kind != ClientEvent::Kind::Disconnected) continue;
            if (e.kind == ClientEvent::Kind::Detached) done = true;
            if (e.kind == ClientEvent::Kind::Disconnected) {
                reason = e.text;
                done = true;
            }
            if (e.kind == ClientEvent::Kind::Event && e.event.kind == EventKind::Exited) {
                reason = "the program exited (" + std::to_string(e.event.x) + ")";
                exit_code = static_cast<int>(e.event.x);
                done = true;
            }
            if (e.kind == ClientEvent::Kind::Event && e.event.kind == EventKind::Bell) host->write("\a");
        }
        int nc = 0, nr = 0;
        host->size(nc, nr);
        if (nc != cols || nr != rows) {
            cols = nc;
            rows = nr;
            full = true;
            host->write("\x1b[0m\x1b[H\x1b[2J");
            c->resize(sid, cols, rows);
        }
        if (ScreenModel* m = c->screen(sid)) {
            draw(*host, *m, full);
            m->clear_dirty();
            full = false;
        }
        if (done) break;
    }
    *quit = true;
    host->restore();
    std::fprintf(stderr, "[bromux: %s]\n", reason.c_str());
    std::fflush(stderr);
    std::_Exit(exit_code);  // the keyboard thread may sit in a read; nothing left to clean up
}

}  // namespace bromux::cli
