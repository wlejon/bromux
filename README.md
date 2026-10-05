# bromux

A terminal multiplexer as a reusable C++20 library. A server process owns PTY sessions and runs their terminal emulators. Terminals and UIs attach to it as clients: they receive a screen snapshot and then incremental updates, and send input and resizes. When a UI quits, crashes or restarts, its sessions keep running, and reattaching restores everything.

bromux was built for the bro terminal, but it does not depend on bro or bronze; any terminal can embed the client. It builds on [bropty](../bropty), which provides the PTY and the emulator.

- **Server-side emulation.** The server runs a bropty `Terminal` per session. Clients receive *state* (rows, cursor, modes, title, palette), never a byte replay. Reattaching sends one snapshot. Updates are dirty-row diffs computed per client, with scroll detection. Scrollback is fetched on demand.
- **Many clients per session**, under a resize policy (latest / smallest / largest / fixed). Clients can also attach read-only or no-resize.
- **Forwarded events:** title, cwd (OSC 7), bell, notifications (OSC 9/777), progress, OSC 133 marks, and OSC 52 clipboard writes and queries behind a per-session policy.
- **Flow control:**
  - A flooding session gets a bounded slice per loop turn, so it cannot starve the others.
  - A slow or stalled client gets fewer, larger frames: each frame is a diff to the latest state, under an acknowledgement window, so it is never sent a backlog.
  - A dead client costs nothing.
- **Local IPC:**
  - Unix sockets: mode 0600 in a 0700 directory, with peer credentials checked.
  - Windows named pipes: a DACL that admits only the current user, remote clients rejected, and the server owner verified by the client.
- **Remote attach:** `ssh host bromux proxy` relays bytes to the remote host's local server. Security is ssh's; bromux adds no crypto and no libssh.
- **Daemon lifecycle:**
  - The first client starts the server, which runs detached.
  - The server exits after being idle (no clients and no running sessions).
  - Several servers starting at once end with exactly one owner. A crashed server's endpoint is reclaimed.
- **Its own event loop:** poll plus a self-pipe on POSIX, an I/O completion port on Windows. No libuv, no Asio, no other dependencies.
- **A documented protocol:** versioned and length-prefixed. See [docs/protocol.md](docs/protocol.md).

## Build

```bash
cmake -B build                      # Windows: Visual Studio generator
cmake --build build --config Release
ctest --test-dir build -C Release

cmake -G Ninja -B build-release -DCMAKE_BUILD_TYPE=Release   # Linux / macOS
cmake --build build-release && ctest --test-dir build-release
```

bropty is found as `../bropty`; set `-DBROPTY_DIR=<path>` to use another checkout. If the parent project already defines a `bropty` target, that target is used. The build produces:

- `bromux`, a static library;
- `bromux` (target `bromux_cli`), the server, proxy and command-line client.

## Command line

```
bromux server [-L name|--socket addr] [--daemon] [--idle-exit SEC] [--log FILE]
              [--tee-dir DIR] [--clipboard deny|write|readwrite]
bromux proxy  [-L name|--socket addr]              # the remote end of ssh
bromux ls | new | attach | kill | kill-server  [target] ...
bromux version
target: -L name | --socket addr | --ssh [user@]host [--remote-bromux PATH] [--ssh-arg ARG]...
```

- `bromux new -- htop` starts a session and prints its id.
- `bromux attach 1` shows it in the current terminal. Ctrl-] detaches.
- `bromux ls --ssh me@box` lists the sessions on `box`.

## Embedding the client

```cpp
#include <bromux/client.h>

bromux::ConnectOptions opt;          // default server; auto-started when not running
std::string err;
auto c = bromux::Client::connect(opt, &err);
// or: bromux::Client::connect_ssh({.host = "me@box"}, opt, &err);

bromux::SessionSpec spec;            // empty command: the user's shell
spec.meta = {{"name", "build"}};
auto info = c->create_session(spec);
c->attach(info->id, 120, 40);

std::vector<bromux::ClientEvent> events;
for (;;) {
    c->wait(std::chrono::milliseconds(16));
    events.clear();
    c->dispatch(events);             // applies frames, acknowledges them
    const bromux::ScreenModel* m = c->screen(info->id);
    // m->row(y) is a bropty::RowView; m->cursor(), m->modes(), m->title() ...
}
```

- Input is sent with `send_key`, `send_text`, `paste`, `send_mouse` and `focus`.
- `resize` changes the client's size.
- `fetch_history(session, start, count)` reads scrollback.
- `put_blob` / `get_blob` store opaque client state, such as a layout, on the server.

`Client` is not thread-safe. It is meant to be driven from the UI thread; a background thread only reads the connection.

## Endpoints

| Platform | Default endpoint |
|----------|------------------|
| POSIX | `$XDG_RUNTIME_DIR/bromux/<name>.sock`, else `${TMPDIR:-/tmp}/bromux-<uid>/<name>.sock` |
| Windows | `\\.\pipe\bromux-<user SID>-<name>` |

The default name is `default`. `-L name` selects another server; `BROMUX_SOCKET` overrides the default server's address. A server started by a client is `$BROMUX_EXE`, else a `bromux` beside the client's executable, else `bromux` on `PATH`. A daemonized server logs to `<runtime dir>/<name>.log` unless `--log` says otherwise.

## Tests

Every test is a real ctest that fails in Release builds; none depends on `assert`. The main oracle compares each client's reconstructed screen cell by cell with a bropty `Terminal` replaying the session's recording up to the frame's `feed_seq`. It runs under random attach, detach, resize, policy changes, clients that vanish or stall, floods, and real shells and programs.

| Test | Covers |
|------|--------|
| test_wire | primitives, framing, every message, fuzzed input |
| test_codec | row and screen codec against random terminals; scroll ops; malformed input |
| test_server | sessions, input routing, resize policies, events, clipboard, meta/blobs, history, exit, protocol errors |
| test_oracle | random VT streams and a real shell under multi-client churn; vi (POSIX) |
| test_flow | flood fairness, a stalled client, client processes killed mid-stream |
| test_daemon | auto-start, persistence, idle exit, start races, a killed server, the proxy, the CLI |
| test_remote | the same oracle through real `ssh host bromux proxy` |

test_remote is skipped unless these variables name a remote host that has bromux built:

| Variable | Value |
|----------|-------|
| `BROMUX_TEST_SSH` | `user@host` |
| `BROMUX_TEST_SSH_BROMUX` | path to `bromux` on the remote |
| `BROMUX_TEST_SSH_CHILD` | path to the test helper `mux_child` on the remote |
| `BROMUX_TEST_SSH_ARGS` | optional extra ssh arguments |

`BROMUX_ORACLE_STEPS=N` multiplies the churn, and `BROMUX_TEST_LOG=1` prints the server log.
