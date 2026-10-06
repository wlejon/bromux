// A session's foreground process (EventKind::Foreground, minor 1): what owns
// the terminal now, as bropty's IPtyProcess::foreground_process answers it.
// Asking costs a few system calls on POSIX and a process snapshot (~1 ms) on
// Windows, so it is asked only while a client that reads the answer is
// attached: shortly after input or output (a program starting or ending
// usually comes with both), then twice more as things settle, and rarely
// otherwise.
#include "server_impl.h"

#include <algorithm>
#include <iterator>

namespace bromux::detail {

namespace {

// A program started by a keystroke is running a moment later, not at once.
constexpr std::chrono::milliseconds kSettle{60};
// After activity, checks at these delays catch a change that came later with
// nothing printed (a program that starts silently).
constexpr std::chrono::milliseconds kTrail[] = {std::chrono::milliseconds(500), std::chrono::milliseconds(1500)};

}  // namespace

bool ServerSession::wants_foreground() const {
    if (!pty || !running) return false;
    return std::any_of(attachments.begin(), attachments.end(), [](const Attachment* a) { return a->minor1(); });
}

EventMsg ServerSession::foreground_event() const {
    EventMsg e;
    e.session = id;
    e.kind = EventKind::Foreground;
    if (fg) {
        e.x = fg->pid;
        e.a = fg->name;
        e.b = fg->path;
        e.c = fg->command_line;
    }
    return e;
}

void ServerSession::poll_foreground(Clock::time_point now, bool force) {
    const ServerOptions& o = core.options();
    if (feed_seq != fg_seen_seq) {
        fg_seen_seq = feed_seq;
        fg_activity = true;
    }
    if (!wants_foreground()) {
        // Nobody reads it: start over (with a check) when someone does.
        fg_due = {};
        return;
    }
    if (fg_activity) {
        fg_activity = false;
        fg_trail = int(std::size(kTrail));
        const Clock::time_point due = std::max(now + kSettle, fg_last + o.foreground_gap);
        if (fg_due == Clock::time_point{} || due < fg_due) fg_due = due;
    }
    if (!fg_known) force = true;
    if (fg_due == Clock::time_point{}) fg_due = fg_last + o.foreground_idle;
    if (!force && now < fg_due) return;

    std::optional<bropty::ProcessInfo> info = pty->foreground_process();
    fg_last = now;
    fg_due = now + o.foreground_idle;
    if (fg_trail > 0) {
        fg_due = now + kTrail[std::size(kTrail) - size_t(fg_trail)];
        --fg_trail;
    }
    const bool changed = !fg_known || info != fg;
    fg = std::move(info);
    fg_known = true;
    if (changed) push_event(foreground_event(), 1);
}

}  // namespace bromux::detail
