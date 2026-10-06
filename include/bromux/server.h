#pragma once
// The bromux server: owns PTY sessions and their emulator state, and serves
// clients over local IPC (paths.h names the endpoint).
//
// One thread runs everything: the event loop (Unix socket + poll on POSIX,
// named pipes + an I/O completion port on Windows), the sessions' PTY output
// (bropty's reader threads only wake the loop) and frame generation. Each
// loop turn gives every session with pending output a bounded slice (bytes
// and time), round robin, so a flood in one session cannot starve the
// others; frames are state diffs against what each client was last sent,
// paced per client and bounded by an acknowledgement window, so a slow
// client sees fewer, larger frames instead of slowing anything down.

#include "bromux/protocol.h"

#include <bropty/graphics.h>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace bromux {

struct ServerOptions {
    // Endpoint to listen on (paths.h: server_address()).
    std::string address;
    // Exit once there has been no client and no running session for this
    // long. Negative: never.
    std::chrono::milliseconds idle_exit{30000};
    // Default OSC 52 policy for new sessions (SetPolicy changes it per session).
    ClipboardPolicy clipboard_policy{ClipboardPolicy::WriteOnly};
    // Frame bytes a client may have unacknowledged before frames pause for it.
    size_t frame_window_bytes{1u << 20};
    // Frames to one client are at least this far apart: a change after a
    // quiet spell goes out at once, a stream of changes coalesces (and a
    // flood costs at most 1000 / interval frames a second per client).
    std::chrono::milliseconds min_frame_interval{8};
    // Longest a frame is held back while the program has synchronized
    // output (?2026) on.
    std::chrono::milliseconds sync_output_timeout{150};
    // PTY output one session may consume per loop turn.
    size_t session_slice_bytes{256u << 10};
    std::chrono::microseconds session_slice_time{3000};
    // Events queued per client attachment before newer ones are dropped
    // (and an EventsDropped event reports how many).
    size_t event_queue_limit{2048};
    // Debugging / testing: when set, every session records everything that
    // changed its terminal to <tee_dir>/session-<id>.tee (tee.h), so a bropty
    // Terminal can replay it to any FrameMsg::feed_seq.
    std::string tee_dir;
    // Diagnostics sink (default: none).
    std::function<void(std::string_view)> log;
    // Decodes compressed inline images for the sessions' terminals: PNG for
    // kitty f=100, any format for iTerm2 (bropty TerminalHost::decode_image,
    // whose contract it follows). Unset, those images are refused; raw RGB /
    // RGBA, zlib-compressed kitty data and sixel need no decoder. Called on
    // the server's thread.
    std::function<bool(std::string_view data, const bropty::ImageLimits& limits, bropty::DecodedImage& out)>
        decode_image;
    // How often a session's foreground process (bropty
    // IPtyProcess::foreground_process) is checked while clients that read it
    // are attached: shortly after input or output, at most every
    // foreground_gap, and every foreground_idle otherwise.
    std::chrono::milliseconds foreground_gap{250};
    std::chrono::milliseconds foreground_idle{3000};
};

class Server {
public:
    explicit Server(ServerOptions options);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Bind the endpoint. False (with `err`) when it cannot; `in_use` is set
    // when another live server already owns it.
    bool start(std::string* err = nullptr, bool* in_use = nullptr);
    // Serve until stop(), a KillServer message or the idle timeout. Returns
    // after every session has been torn down.
    void run();
    // Thread-safe.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace bromux
