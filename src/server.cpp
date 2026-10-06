// Server lifecycle and the loop: I/O, session output, frames, idle exit.
#include "bromux/paths.h"
#include "server_impl.h"

#include <algorithm>

namespace bromux {

struct Server::Impl {
    explicit Impl(ServerOptions o) : core(std::move(o)) {}
    detail::ServerCore core;
};

Server::Server(ServerOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}
Server::~Server() = default;
bool Server::start(std::string* err, bool* in_use) { return impl_->core.start(err, in_use); }
void Server::run() { impl_->core.run(); }
void Server::stop() { impl_->core.request_stop(); }

namespace detail {

namespace {
constexpr size_t kReadChunk = 64u << 10;
constexpr auto kClipboardTimeout = std::chrono::seconds(10);
}  // namespace

ServerCore::ServerCore(ServerOptions options) : opt_(std::move(options)) { start_ms_ = unix_time_ms(); }

ServerCore::~ServerCore() {
    if (loop_) shutdown();
}

bool ServerCore::start(std::string* err, bool* in_use) {
    loop_ = EventLoop::create(*this);
    std::string e;
    bool busy = false;
    if (!loop_->listen(opt_.address, e, busy)) {
        if (err) *err = e;
        if (in_use) *in_use = busy;
        loop_.reset();
        return false;
    }
    waker_ = std::make_shared<Waker>();
    waker_->loop = loop_.get();
    log("listening on " + opt_.address);
    return true;
}

Conn* ServerCore::find_conn(ConnId id) {
    auto it = conns_.find(id);
    return it == conns_.end() ? nullptr : it->second.get();
}

void ServerCore::send_error(Conn& c, uint32_t req, ErrorCode code, std::string message) {
    ErrorMsg e;
    e.req = req;
    e.code = code;
    e.message = std::move(message);
    send(c, e);
}

void ServerCore::close_conn(Conn& c) {
    if (c.closing) return;
    c.closing = true;
    loop_->close(c.id, true);
}

void ServerCore::on_accept(ConnId id) {
    auto c = std::make_unique<Conn>();
    c->id = id;
    conns_.emplace(id, std::move(c));
    idle_ = false;
}

void ServerCore::on_data(ConnId id, const char* data, size_t n) {
    Conn* c = find_conn(id);
    if (!c || c->closing) return;
    c->splitter.feed(data, n);
    wire::MessageSplitter::Message m;
    while (!c->closing && c->splitter.next(m)) handle(*c, m.type, m.payload);
    if (c->splitter.error() && !c->closing) {
        send_error(*c, 0, ErrorCode::BadMessage, "malformed message framing");
        close_conn(*c);
    }
}

void ServerCore::on_closed(ConnId id) {
    auto it = conns_.find(id);
    if (it == conns_.end()) return;
    Conn& c = *it->second;
    c.closing = true;  // nothing more is written to it
    std::vector<uint64_t> ids;
    for (auto& [sid, a] : c.attachments) ids.push_back(sid);
    for (uint64_t sid : ids) detach(c, sid, DetachReason::Requested, false);
    conns_.erase(it);
}

void ServerCore::reap(std::shared_ptr<bropty::IPtyProcess> pty) {
    // Join reapers that have finished.
    for (size_t i = 0; i < reapers_.size();) {
        if (reapers_[i].done->load()) {
            reapers_[i].thread.join();
            reapers_[i] = std::move(reapers_.back());
            reapers_.pop_back();
        } else {
            ++i;
        }
    }
    if (!pty) return;
    // terminate() can take a few grace periods; never on the loop thread.
    Reaper r;
    r.done = std::make_shared<std::atomic<bool>>(false);
    r.thread = std::thread([p = std::move(pty), done = r.done]() mutable {
        p->terminate();
        p.reset();
        done->store(true);
    });
    reapers_.push_back(std::move(r));
}

// Give every session with output a bounded slice (bytes and time) per turn,
// so one flooding session cannot starve the rest or the clients' I/O. The
// Session reads and feeds; its feed tap (create_session) counts and records
// each chunk.
void ServerCore::pump_sessions() {
    std::vector<ServerSession*> exited;
    bropty::Session::UpdateBudget budget;
    budget.max_bytes = opt_.session_slice_bytes;
    budget.max_time = opt_.session_slice_time;
    budget.slice = kReadChunk;
    const Clock::time_point now = Clock::now();
    const uint64_t now_ms =
        uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
    for (auto& [id, sp] : sessions_) {
        ServerSession& s = *sp;
        s.has_pending = false;
        if (!s.pty) continue;
        s.term->update(budget);
        s.has_pending = s.term->has_pending_output();
        // Image animations run here, frames go out as the images change
        // (extras_due); nothing is due when nothing animates.
        if (now >= s.anim_due || s.t().images_version() != s.anim_seen) {
            const uint64_t wait = s.t().advance_animations(now_ms);
            s.anim_seen = s.t().images_version();
            s.anim_due = wait == UINT64_MAX ? Clock::time_point::max()
                                            : now + std::chrono::milliseconds(std::min<uint64_t>(wait, 60000));
        }
        s.poll_foreground(now, false);
        if (s.running && !s.has_pending && !s.pty->is_running() && s.pty->eof()) exited.push_back(&s);
    }
    for (auto& [id, sp] : sessions_) sp->tee.flush();
    for (ServerSession* s : exited) session_exited(*s);  // may close (erase) the session
}

void ServerCore::expire_clipboard_requests(Clock::time_point now) {
    for (auto& [id, s] : sessions_) {
        auto& v = s->clip_requests;
        for (auto it = v.begin(); it != v.end();) {
            if (now - it->at > kClipboardTimeout) {
                s->t().cancel_clipboard(it->request);
                it = v.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void ServerCore::flush_frames(Clock::time_point now) {
    next_deadline_ = Clock::time_point::max();
    auto defer = [this](Clock::time_point t) { next_deadline_ = std::min(next_deadline_, t); };
    for (auto& [id, sp] : sessions_) {
        ServerSession& s = *sp;
        if (s.attachments.empty()) continue;
        // Synchronized output (?2026): hold frames until the program ends the
        // update, at most sync_output_timeout.
        bool hold_sync = false;
        if (s.t().modes().synchronized_output) {
            if (!s.sync_active) {
                s.sync_active = true;
                s.sync_since = now;
            }
            if (now - s.sync_since < opt_.sync_output_timeout) {
                hold_sync = true;
                defer(s.sync_since + opt_.sync_output_timeout);
            } else {
                s.sync_since = now;  // timed out: show this state, then hold again
            }
        } else {
            s.sync_active = false;
        }
        for (Attachment* a : s.attachments) {
            if (a->conn->closing) continue;
            flush_events(*a);
            const bool force = a->force_frame;
            if (!force) {
                if (a->sent_version == s.feed_seq && !extras_due(*a)) continue;
                if (hold_sync) continue;
                if (a->inflight_bytes >= opt_.frame_window_bytes) continue;  // an Ack re-opens it
                if (loop_->pending_output(a->conn->id) >= opt_.frame_window_bytes) continue;
                // Pacing: the first change after a quiet spell goes out at
                // once (typing echo); a stream of changes coalesces.
                if (now - a->last_frame < opt_.min_frame_interval) {
                    defer(a->last_frame + opt_.min_frame_interval);
                    continue;
                }
            }
            send_frame(*a, now);
            if (force) {
                a->force_frame = false;
                for (uint32_t req : a->sync_reqs) {
                    SyncDoneMsg d;
                    d.req = req;
                    d.session = s.id;
                    d.feed_seq = s.feed_seq;
                    send(*a->conn, d);
                }
                a->sync_reqs.clear();
            }
        }
    }
}

bool ServerCore::idle_expired(Clock::time_point now) {
    if (opt_.idle_exit.count() < 0) return false;
    bool busy = loop_->connections() > 0;
    for (auto& [id, s] : sessions_) busy = busy || s->running;
    if (busy) {
        idle_ = false;
        return false;
    }
    if (!idle_) {
        idle_ = true;
        idle_since_ = now;
    }
    return now - idle_since_ >= opt_.idle_exit;
}

int ServerCore::next_timeout(Clock::time_point now) const {
    for (auto& [id, s] : sessions_)
        if (s->has_pending) return 0;
    Clock::time_point t = next_deadline_;
    if (idle_ && opt_.idle_exit.count() >= 0) t = std::min(t, idle_since_ + opt_.idle_exit);
    for (auto& [id, s] : sessions_) {
        t = std::min(t, s->anim_due);
        if (s->fg_due != Clock::time_point{} && s->wants_foreground()) t = std::min(t, s->fg_due);
    }
    // A safety net: PTY wakeups and I/O drive the loop, this only bounds a lost one.
    t = std::min(t, now + std::chrono::milliseconds(1000));
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t - now).count();
    return int(std::clamp<long long>(ms + 1, 0, 1000));
}

void ServerCore::run() {
    if (!loop_) return;
    Clock::time_point now = Clock::now();
    while (!stop_requested_) {
        loop_->run_once(next_timeout(now));
        pump_sessions();
        now = Clock::now();
        flush_frames(now);
        expire_clipboard_requests(now);
        if (idle_expired(now)) {
            log("idle: exiting");
            break;
        }
    }
    shutdown();
}

void ServerCore::shutdown() {
    if (!loop_) return;
    log("shutting down");
    loop_->close_listener();
    for (auto& [id, c] : conns_) {
        for (auto& [sid, a] : c->attachments) {
            flush_events(*a);
            DetachedMsg d;
            d.session = sid;
            d.reason = DetachReason::ServerShutdown;
            send(*c, d);
        }
        close_conn(*c);
    }
    // Let the goodbyes drain (bounded).
    const auto until = Clock::now() + std::chrono::milliseconds(500);
    while (loop_->connections() > 0 && Clock::now() < until) loop_->run_once(20);
    for (auto& [id, s] : sessions_) {
        for (Attachment* a : s->attachments) a->conn->attachments.erase(id);
        s->attachments.clear();
        reap(std::move(s->pty));
        s->tee.close();
    }
    {
        std::lock_guard<std::mutex> lk(waker_->mu);
        waker_->loop = nullptr;
    }
    for (Reaper& r : reapers_) r.thread.join();
    reapers_.clear();
    sessions_.clear();
    conns_.clear();
    loop_.reset();
}

}  // namespace detail
}  // namespace bromux
