#pragma once
// The server's event loop: a listener plus nonblocking connections, all
// callbacks on the thread calling run_once(). POSIX: poll() over a Unix
// socket listener, its connections and a self-pipe for wake(). Windows:
// overlapped named-pipe I/O on an I/O completion port; wake() posts a packet.
//
// Writes never block: they queue, and the loop drains the queue as the peer
// reads. pending_output() lets the caller apply its own flow control.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace bromux::detail {

using ConnId = uint64_t;

class LoopHandler {
public:
    virtual ~LoopHandler() = default;
    // A client connected (and passed the platform's credential check).
    virtual void on_accept(ConnId id) = 0;
    virtual void on_data(ConnId id, const char* data, size_t n) = 0;
    // The connection is gone (peer closed, error, or after close()). Exactly
    // once per accepted connection; the id is dead afterwards.
    virtual void on_closed(ConnId id) = 0;
};

class EventLoop {
public:
    virtual ~EventLoop() = default;

    // Bind and listen. `in_use` is set when a live server already owns the
    // address; a stale endpoint left by a dead server is reclaimed.
    virtual bool listen(const std::string& address, std::string& err, bool& in_use) = 0;
    // Stop accepting and remove the endpoint.
    virtual void close_listener() = 0;

    virtual void write(ConnId id, std::string_view data) = 0;
    [[nodiscard]] virtual size_t pending_output(ConnId id) const = 0;
    // Close a connection: at once, or once its queued output has been
    // written (bounded by the peer staying alive). on_closed follows.
    virtual void close(ConnId id, bool flush_first) = 0;

    // Wake run_once() from any thread. Cheap and coalescing.
    virtual void wake() = 0;
    // Wait up to timeout_ms (negative: forever) for I/O or wake(), and
    // dispatch what happened.
    virtual void run_once(int timeout_ms) = 0;
    // Number of live connections.
    [[nodiscard]] virtual size_t connections() const = 0;

    static std::unique_ptr<EventLoop> create(LoopHandler& handler);
};

}  // namespace bromux::detail
