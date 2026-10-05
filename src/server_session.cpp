// Sessions: spawning, the terminal's events, size policy, exit and close.
#include "bromux/paths.h"
#include "server_impl.h"

#include <algorithm>

namespace bromux::detail {

namespace {
constexpr int kMaxCols = 4096;
constexpr int kMaxRows = 2048;
constexpr uint32_t kMaxScrollback = 1000000;
constexpr size_t kMaxEventBytes = 16u << 20;
}  // namespace

std::string base64_encode(std::string_view data) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        uint32_t v = (uint32_t(uint8_t(data[i])) << 16) | (uint32_t(uint8_t(data[i + 1])) << 8) | uint8_t(data[i + 2]);
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(tbl[(v >> 6) & 63]);
        out.push_back(tbl[v & 63]);
    }
    if (i < data.size()) {
        uint32_t v = uint32_t(uint8_t(data[i])) << 16;
        if (i + 1 < data.size()) v |= uint32_t(uint8_t(data[i + 1])) << 8;
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(i + 1 < data.size() ? tbl[(v >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

// ---- ServerSession ----------------------------------------------------------------

SessionInfo ServerSession::info() const {
    SessionInfo i;
    i.id = id;
    i.meta = meta;
    i.command = spec.windows_command_line.empty() ? spec.command : spec.windows_command_line;
    i.pid = pty ? pty->pid() : 0;
    i.running = running;
    i.exit_code = exit_code;
    const bropty::Terminal& term_ref = term->terminal();
    i.cols = uint16_t(term_ref.cols());
    i.rows = uint16_t(term_ref.rows());
    i.clients = uint32_t(attachments.size());
    i.created_ms = created_ms;
    i.title = term_ref.title();
    i.cwd = term_ref.cwd();
    i.resize_policy = resize_policy;
    i.clipboard_policy = clipboard_policy;
    return i;
}

void ServerSession::feed(std::string_view bytes) {
    ++feed_seq;
    tee.data(bytes);
    term->feed(bytes);
}

void ServerSession::resize(int cols, int rows) {
    cols = std::clamp(cols, 1, kMaxCols);
    rows = std::clamp(rows, 1, kMaxRows);
    if (cols == t().cols() && rows == t().rows()) return;
    ++feed_seq;
    tee.resize(cols, rows);
    term->resize(cols, rows);
}

void ServerSession::push_event(EventMsg ev) {
    ev.session = id;
    const bool bell = ev.kind == EventKind::Bell;
    std::string msg;
    for (Attachment* a : attachments) {
        if (bell && a->last_event_bell && !a->events.empty()) continue;  // coalesce bell storms
        if (msg.empty()) msg = encode(ev);
        if (a->events.size() >= core.options().event_queue_limit || a->events_bytes + msg.size() > kMaxEventBytes) {
            ++a->events_dropped;
            continue;
        }
        a->events_bytes += msg.size();
        a->events.push_back(msg);
        a->last_event_bell = bell;
    }
}

void ServerSession::bell() {
    EventMsg e;
    e.kind = EventKind::Bell;
    push_event(std::move(e));
}

void ServerSession::title_changed(std::string_view title) {
    EventMsg e;
    e.kind = EventKind::Title;
    e.a = std::string(title);
    push_event(std::move(e));
    core.notify(NotifyKind::Changed, *this);
}

void ServerSession::icon_name_changed(std::string_view name) {
    EventMsg e;
    e.kind = EventKind::IconName;
    e.a = std::string(name);
    push_event(std::move(e));
}

void ServerSession::cwd_changed(std::string_view uri) {
    EventMsg e;
    e.kind = EventKind::Cwd;
    e.a = std::string(uri);
    push_event(std::move(e));
    core.notify(NotifyKind::Changed, *this);
}

void ServerSession::clipboard_write(std::string_view selection, std::string_view data) {
    if (clipboard_policy == ClipboardPolicy::Deny) return;
    EventMsg e;
    e.kind = EventKind::ClipboardWrite;
    e.a = std::string(selection);
    e.b = std::string(data);
    push_event(std::move(e));
}

// OSC 52 queries are answered asynchronously: the most recently active
// client that may type into the session is asked (ClipboardRequest), and its
// ClipboardData reply is written to the program as the OSC 52 answer.
std::optional<std::string> ServerSession::clipboard_read(std::string_view selection) {
    if (clipboard_policy != ClipboardPolicy::ReadWrite) return std::nullopt;
    Attachment* best = nullptr;
    for (Attachment* a : attachments)
        if (!a->read_only() && (!best || a->activity > best->activity)) best = a;
    if (!best) return std::nullopt;
    ClipRequest r;
    r.token = core.next_token();
    r.conn = best->conn->id;
    r.at = Clock::now();
    r.selection = selection.empty() ? std::string("s0") : std::string(selection);
    ClipboardRequestMsg m;
    m.session = id;
    m.token = r.token;
    m.selection = r.selection;
    clip_requests.push_back(std::move(r));
    core.send(*best->conn, m);
    return std::nullopt;
}

void ServerSession::notification(std::string_view title, std::string_view body) {
    EventMsg e;
    e.kind = EventKind::Notification;
    e.a = std::string(title);
    e.b = std::string(body);
    push_event(std::move(e));
}

void ServerSession::progress(int state, int value) {
    EventMsg e;
    e.kind = EventKind::Progress;
    e.x = state;
    e.y = value;
    push_event(std::move(e));
}

void ServerSession::semantic_mark(char kind, std::string_view params) {
    EventMsg e;
    e.kind = EventKind::SemanticMark;
    e.x = uint8_t(kind);
    e.a = std::string(params);
    push_event(std::move(e));
}

void ServerSession::apc(std::string_view payload) {
    EventMsg e;
    e.kind = EventKind::Apc;
    e.a = std::string(payload);
    push_event(std::move(e));
}

void ServerSession::resized_by_application(int cols, int rows) {
    EventMsg e;
    e.kind = EventKind::ResizedByApp;
    e.x = cols;
    e.y = rows;
    push_event(std::move(e));
}

// ---- ServerCore: session management ---------------------------------------------------

ServerSession* ServerCore::create_session(const SessionSpec& spec, std::string& err) {
    auto s = std::make_unique<ServerSession>(*this);
    s->id = next_session_++;
    s->spec = spec;
    s->meta = spec.meta;
    s->created_ms = unix_time_ms();
    s->resize_policy = spec.resize_policy;
    s->clipboard_policy = opt_.clipboard_policy;

    bropty::TerminalOptions to;
    to.cols = std::clamp<int>(spec.cols, 1, kMaxCols);
    to.rows = std::clamp<int>(spec.rows, 1, kMaxRows);
    to.scrollback_rows = std::min(spec.scrollback_rows, kMaxScrollback);
    s->term = std::make_unique<bropty::Session>(to);
    s->term->set_delegate(s.get());

    bropty::PtyConfig pc;
    pc.command = spec.command;
    pc.args = spec.args;
    pc.windows_command_line = spec.windows_command_line;
    pc.cwd = spec.cwd;
    pc.inherit_env = spec.inherit_env;
    pc.env = spec.env;
    pc.env.emplace_back("BROMUX", opt_.address);
    pc.env.emplace_back("BROMUX_SESSION", std::to_string(s->id));
    pc.env_unset = spec.env_unset;
    pc.size = bropty::PtySize{to.cols, to.rows, 0, 0};

    std::shared_ptr<bropty::IPtyProcess> pty = bropty::create_pty();
    std::weak_ptr<Waker> weak = waker_;
    pty->set_wakeup([weak] {
        if (auto w = weak.lock()) w->wake();
    });
    if (!pty->spawn(pc)) {
        err = pty->last_error();
        if (err.empty()) err = "could not start the program";
        return nullptr;
    }
    s->term->attach_pty(pty);
    s->pty = std::move(pty);

    if (!opt_.tee_dir.empty()) {
        TeeHeader h;
        h.cols = to.cols;
        h.rows = to.rows;
        h.scrollback_rows = uint32_t(to.scrollback_rows);
        std::string path = opt_.tee_dir + "/session-" + std::to_string(s->id) + ".tee";
        if (!s->tee.open(path, h)) log("cannot open tee file " + path);
    }
    ServerSession* raw = s.get();
    sessions_.emplace(raw->id, std::move(s));
    idle_ = false;
    log("session " + std::to_string(raw->id) + " started (pid " + std::to_string(raw->pty->pid()) + ")");
    notify(NotifyKind::Added, *raw);
    return raw;
}

void ServerCore::close_session(uint64_t id) {
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return;
    ServerSession& s = *it->second;
    std::vector<Attachment*> atts = s.attachments;
    for (Attachment* a : atts) detach(*a->conn, id, DetachReason::SessionClosed, true);
    notify(NotifyKind::Removed, s);
    s.tee.close();
    reap(std::move(s.pty));
    log("session " + std::to_string(id) + " closed");
    sessions_.erase(it);
}

void ServerCore::session_exited(ServerSession& s) {
    s.running = false;
    s.exit_code = s.pty ? s.pty->exit_code().value_or(-1) : -1;
    EventMsg e;
    e.kind = EventKind::Exited;
    e.x = s.exit_code;
    s.push_event(std::move(e));
    log("session " + std::to_string(s.id) + " exited with " + std::to_string(s.exit_code));
    notify(NotifyKind::Changed, s);
    if (s.spec.remove_on_exit) {
        // Deliver what the program printed last before the session goes.
        const auto now = Clock::now();
        for (Attachment* a : s.attachments) {
            flush_events(*a);
            send_frame(*a, now);
        }
        close_session(s.id);
    }
}

void ServerCore::apply_resize_policy(ServerSession& s) {
    if (s.resize_policy == ResizePolicy::Fixed) return;
    const Attachment* latest = nullptr;
    int cols = 0;
    int rows = 0;
    for (const Attachment* a : s.attachments) {
        if (!a->sizes()) continue;
        switch (s.resize_policy) {
        case ResizePolicy::Latest:
            if (!latest || a->activity > latest->activity) latest = a;
            break;
        case ResizePolicy::Smallest:
            cols = cols ? std::min(cols, a->cols) : a->cols;
            rows = rows ? std::min(rows, a->rows) : a->rows;
            break;
        case ResizePolicy::Largest:
            cols = std::max(cols, a->cols);
            rows = std::max(rows, a->rows);
            break;
        case ResizePolicy::Fixed:
            break;
        }
    }
    if (latest) {
        cols = latest->cols;
        rows = latest->rows;
    }
    if (cols > 0 && rows > 0) s.resize(cols, rows);
}

void ServerCore::notify(NotifyKind kind, ServerSession& s) {
    std::string msg;
    for (auto& [id, c] : conns_) {
        if (c->closing || !c->hello || !(c->flags & Hello_WatchSessions)) continue;
        if (msg.empty()) {
            SessionNotifyMsg m;
            m.kind = kind;
            m.info = s.info();
            msg = encode(m);
        }
        send_raw(*c, msg);
    }
}

}  // namespace bromux::detail
