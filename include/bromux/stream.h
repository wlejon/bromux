#pragma once
// Blocking byte streams for clients and the proxy, and process spawning.
//
// A Stream is read by one thread and written by others (writes must be
// serialised by the caller); shutdown() from any thread unblocks a pending
// read. Kinds: a connection to a local server (Unix socket / named pipe),
// a child process's stdin/stdout (how remote attach runs `ssh host bromux
// proxy`), and this process's own stdin/stdout (the proxy's end).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bromux {

class Stream {
public:
    virtual ~Stream() = default;
    // Blocks until data arrives. Returns the bytes read; 0 at end of stream,
    // on error or after shutdown().
    virtual size_t read(char* buf, size_t n) = 0;
    // Writes everything (blocking). False when the stream is gone.
    virtual bool write(std::string_view data) = 0;
    // Unblocks read() and fails further I/O. Idempotent, any thread.
    virtual void shutdown() = 0;
    // Extra text for error messages (e.g. what an ssh child said on stderr).
    [[nodiscard]] virtual std::string diagnostics() const { return {}; }
};

// Connect to the server at `address` (paths.h). On failure returns null and
// sets `not_running` when nothing is listening there (as opposed to, say,
// an access check failing).
std::unique_ptr<Stream> connect_local(const std::string& address, std::string* err = nullptr,
                                      bool* not_running = nullptr);

// Spawn argv[0] (searched on PATH) with its stdin/stdout as the stream; its
// stderr is collected for diagnostics(). shutdown() closes its stdin and,
// if it has not exited within a moment, kills it.
std::unique_ptr<Stream> spawn_stream(const std::vector<std::string>& argv, std::string* err = nullptr);

// This process's stdin (read) and stdout (write).
std::unique_ptr<Stream> stdio_stream();

// Start argv detached from this process (no console, own session / process
// group, not in our job), not waiting for it. Used to auto-start a server.
bool spawn_detached(const std::vector<std::string>& argv, std::string* err = nullptr);

// A plain child process (stdio inherited), for tools and tests.
class Process {
public:
    virtual ~Process() = default;
    [[nodiscard]] virtual int64_t pid() const = 0;
    // Forceful kill (SIGKILL / TerminateProcess).
    virtual void kill() = 0;
    // Wait for exit; true with the exit code when it exited in time.
    virtual bool wait_for(std::chrono::milliseconds timeout, int* exit_code = nullptr) = 0;
    static std::unique_ptr<Process> spawn(const std::vector<std::string>& argv, std::string* err = nullptr);
};

}  // namespace bromux
