// `bromux attach`: a minimal interactive client that draws a session's
// ScreenModel onto the terminal it runs in and forwards keystrokes (as the
// raw bytes the host terminal encodes). Ctrl-] detaches. It exists for
// trying bromux out from any terminal (and over ssh); real UIs embed the
// Client library and render the model themselves.
#include "cli.h"

#include <bropty/cell.h>
#include <bropty/view.h>
#include <bromux/screen_source.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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
        write("\x1b[?1049h\x1b[?1000h\x1b[?1002h\x1b[?1006h\x1b[H\x1b[2J");
    }
    ~HostTerminal() { restore(); }
    void restore() {
        if (restored_.exchange(true)) return;
        write("\x1b[0m\x1b[?25h\x1b[?1006l\x1b[?1002l\x1b[?1000l\x1b[?1049l");
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

void draw(const HostTerminal& host, Client& client, uint64_t sid, bool full) {
    ScreenModel* m = client.screen(sid);
    if (!m) return;
    bropty::TerminalView* view = client.view(sid);
    if (!view) return;

    int hcols = 0, hrows = 0;
    host.size(hcols, hrows);
    const int rows = std::min(m->rows(), hrows);
    const int cols = std::min(m->cols(), hcols);
    std::string out = "\x1b[?25l";

    const bool at_bottom = view->at_bottom();
    const int64_t top = view->top_row();

    if (!at_bottom) {
        view->source().request_rows(top, std::min(top + rows, m->screen_top_row()));
    }

    for (int y = 0; y < rows; ++y) {
        if (!full && at_bottom && !m->row_dirty(y)) continue;
        out += "\x1b[" + std::to_string(y + 1) + ";1H";
        bropty::RowView row = at_bottom ? m->row(y) : view->source().row_at(top + y);
        uint32_t cur_style = UINT32_MAX;
        for (int x = 0; x < cols; ++x) {
            const bropty::Cell& c = (x < row.cols) ? row[x] : bropty::Cell{};
            if (c.wide() == bropty::Wide::SpacerTail) continue;
            if (c.wide() == bropty::Wide::Lead && x + 1 >= cols) break;  // half a wide char does not fit
            if (c.style != cur_style) {
                cur_style = c.style;
                out += sgr(m->style(c.style));
            }
            if (c.is_empty() || c.wide() == bropty::Wide::SpacerHead) {
                out.push_back(' ');
            } else {
                for (char32_t cp : row.cluster(x)) bropty::append_utf8(out, cp);
            }
        }
        out += "\x1b[0m\x1b[K";
    }

    if (!at_bottom) {
        int64_t lines_above = m->screen_top_row() - top;
        std::string tag = " [SCROLLBACK: " + std::to_string(lines_above) + " lines above | 'q' or type to exit] ";
        if (cols > int(tag.size())) {
            out += "\x1b[1;" + std::to_string(cols - int(tag.size()) + 1) + "H";
            out += "\x1b[7;1m" + tag + "\x1b[0m";
        }
    } else {
        const bropty::CursorState& cur = m->cursor();
        out += "\x1b[" + std::to_string(cur.row + 1) + ";" + std::to_string(cur.col + 1) + "H";
        if (cur.visible) out += "\x1b[?25h";
    }
    host.write(out);
}

struct InputQueue {
    std::mutex mu;
    std::deque<std::string> queue;

    void push(std::string s) {
        std::lock_guard<std::mutex> lk(mu);
        queue.push_back(std::move(s));
    }

    std::vector<std::string> drain() {
        std::lock_guard<std::mutex> lk(mu);
        std::vector<std::string> out(queue.begin(), queue.end());
        queue.clear();
        return out;
    }
};

struct ParsedSgrMouse {
    int button = 0;
    int col = 0;
    int row = 0;
    char type = 0; // 'M' or 'm'
    size_t length = 0;
};

bool parse_sgr_mouse(std::string_view s, ParsedSgrMouse& out) {
    if (s.size() < 6 || !s.starts_with("\x1b[<")) return false;
    size_t i = 3;
    int b = 0, x = 0, y = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        b = b * 10 + (s[i] - '0');
        ++i;
    }
    if (i >= s.size() || s[i] != ';') return false;
    ++i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        x = x * 10 + (s[i] - '0');
        ++i;
    }
    if (i >= s.size() || s[i] != ';') return false;
    ++i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        y = y * 10 + (s[i] - '0');
        ++i;
    }
    if (i >= s.size() || (s[i] != 'M' && s[i] != 'm')) return false;
    out.button = b;
    out.col = x;
    out.row = y;
    out.type = s[i];
    out.length = i + 1;
    return true;
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
    if (!parse_target(a, t, extra) || a.failed()) {
        std::fprintf(stderr, "usage: bromux attach [target] [--read-only] [session]\n");
        return 2;
    }
    std::string err;
    std::shared_ptr<Client> c(connect_target(t, true, &err));
    if (!c) {
        std::fprintf(stderr, "bromux: %s\n", err.c_str());
        return 1;
    }
    auto list = c->list_sessions(&err);
    if (!list) {
        std::fprintf(stderr, "bromux: %s\n", err.c_str());
        return 1;
    }

    auto get_name = [](const SessionInfo& s) -> std::string {
        for (const auto& p : s.meta) {
            if (p.first == "name") return p.second;
        }
        return {};
    };

    uint64_t sid = 0;
    if (!id.empty()) {
        char* end = nullptr;
        unsigned long long num = std::strtoull(id.c_str(), &end, 10);
        if (end != id.c_str() && *end == '\0') {
            for (const auto& s : *list) {
                if (s.id == num) {
                    sid = s.id;
                    break;
                }
            }
            if (sid == 0) {
                std::fprintf(stderr, "bromux: session %llu not found\n", num);
                return 1;
            }
        } else {
            for (const auto& s : *list) {
                if (get_name(s) == id) {
                    sid = s.id;
                    break;
                }
            }
            if (sid == 0) {
                SessionSpec spec;
                spec.meta.emplace_back("name", id);
                spec.remove_on_exit = true;
                auto info = c->create_session(spec, &err);
                if (!info) {
                    std::fprintf(stderr, "bromux: %s\n", err.c_str());
                    return 1;
                }
                sid = info->id;
            }
        }
    } else {
        const SessionInfo* best = nullptr;
        for (const auto& s : *list) {
            if (!s.running) continue;
            if (!best || s.created_ms > best->created_ms) {
                best = &s;
            }
        }
        if (best) {
            sid = best->id;
        } else {
            SessionSpec spec;
            spec.meta.emplace_back("name", "main");
            spec.remove_on_exit = true;
            auto info = c->create_session(spec, &err);
            if (!info) {
                std::fprintf(stderr, "bromux: %s\n", err.c_str());
                return 1;
            }
            sid = info->id;
        }
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
    auto input_queue = std::make_shared<InputQueue>();
    // Keyboard -> session. Detached: a blocked console read must not hold up exit.
    // The main loop is woken for each read, so a key goes to the session at
    // once rather than at the loop's next timeout.
    std::thread([host, quit, input_queue, c] {
        char buf[4096];
        while (!*quit) {
            size_t n = host->read(buf, sizeof buf);
            if (n == 0) break;
            input_queue->push(std::string(buf, n));
            c->poke();
        }
        *quit = true;
    }).detach();

    std::string reason = "detached";
    int exit_code = 0;
    bool full = true;
    bropty::CursorState drawn_cursor;
    drawn_cursor.row = -1;  // nothing drawn yet
    std::vector<ClientEvent> evs;
    while (!*quit || c->screen(sid)) {
        c->wait(std::chrono::milliseconds(20));
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
            if (e.kind == ClientEvent::Kind::History) {
                full = true;
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

        auto inputs = input_queue->drain();
        for (const auto& raw : inputs) {
            if (raw.find(kDetachKey) != std::string::npos) {
                done = true;
                break;
            }

            ParsedSgrMouse psm;
            if (parse_sgr_mouse(raw, psm)) {
                ScreenModel* sm = c->screen(sid);
                bool tracking = sm && sm->modes().mouse_tracking != bropty::MouseTracking::None;
                if (tracking) {
                    bropty::MouseEvent ev;
                    ev.col = std::max(0, psm.col - 1);
                    ev.row = std::max(0, psm.row - 1);
                    ev.action = (psm.type == 'M' ? bropty::MouseAction::Press : bropty::MouseAction::Release);
                    int btn = psm.button & ~32;
                    if (btn == 64) ev.button = bropty::MouseButton::WheelUp;
                    else if (btn == 65) ev.button = bropty::MouseButton::WheelDown;
                    else if (btn == 66) ev.button = bropty::MouseButton::WheelLeft;
                    else if (btn == 67) ev.button = bropty::MouseButton::WheelRight;
                    else if ((btn & 3) == 0) ev.button = bropty::MouseButton::Left;
                    else if ((btn & 3) == 1) ev.button = bropty::MouseButton::Middle;
                    else if ((btn & 3) == 2) ev.button = bropty::MouseButton::Right;
                    else ev.button = bropty::MouseButton::None;
                    if (psm.button & 32) ev.action = bropty::MouseAction::Motion;
                    if (!read_only) c->send_mouse(sid, ev);
                } else {
                    int btn = psm.button & ~32;
                    if (btn == 64) {
                        if (auto* v = c->view(sid)) {
                            v->scroll_by(-3);
                            full = true;
                        }
                    } else if (btn == 65) {
                        if (auto* v = c->view(sid)) {
                            v->scroll_by(3);
                            full = true;
                        }
                    }
                }
            } else if (raw == "\x1b[5~") {
                if (auto* v = c->view(sid)) {
                    v->scroll_by(-int64_t(rows));
                    full = true;
                }
            } else if (raw == "\x1b[6~") {
                if (auto* v = c->view(sid)) {
                    v->scroll_by(int64_t(rows));
                    full = true;
                }
            } else {
                auto* v = c->view(sid);
                if (v && !v->at_bottom()) {
                    if (raw == "\x1b" || raw == "q" || raw == "Q") {
                        v->scroll_to_bottom();
                        full = true;
                    } else {
                        v->scroll_to_bottom();
                        full = true;
                        if (!read_only) c->send_raw(sid, raw);
                    }
                } else {
                    if (!read_only) c->send_raw(sid, raw);
                }
            }
        }

        // Drawn when something changed: an idle session writes nothing (a
        // host terminal would otherwise repaint at the loop's rate).
        if (ScreenModel* m = c->screen(sid)) {
            // (Scrolled back, rows come in from history as they arrive.)
            bropty::TerminalView* v = c->view(sid);
            bool changed = full || (v && !v->at_bottom());
            for (int y = 0; !changed && y < m->rows(); ++y) changed = m->row_dirty(y);
            const bropty::CursorState& cur = m->cursor();
            if (cur.row != drawn_cursor.row || cur.col != drawn_cursor.col || cur.visible != drawn_cursor.visible)
                changed = true;
            if (changed) {
                draw(*host, *c, sid, full);
                drawn_cursor = cur;
            }
            m->clear_dirty();
            full = false;
        }
        if (done) break;
    }
    *quit = true;
    c->detach(sid);
    host->restore();
    std::fprintf(stderr, "[bromux: %s]\n", reason.c_str());
    std::fflush(stderr);
    std::_Exit(exit_code);  // the keyboard thread may sit in a read; nothing left to clean up
}

}  // namespace bromux::cli
