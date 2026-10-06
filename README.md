# bromux

[![CI](https://github.com/wlejon/bromux/actions/workflows/ci.yml/badge.svg)](https://github.com/wlejon/bromux/actions/workflows/ci.yml)

A terminal multiplexer as a reusable C++20 library. A server process owns PTY
sessions and runs their terminal emulators. Terminals and UIs attach to it as
clients: they receive a screen snapshot and incremental dirty-row diffs, and
send input and resizes. When a UI quits, crashes or restarts, its sessions keep
running, and reattaching restores the state.

In the [Bro ecosystem](https://github.com/wlejon/bro/blob/main/docs/ecosystem.md),
bromux provides session persistence and multiplexing for the `<terminal>`
element and [broterm](https://github.com/wlejon/broterm). It builds on
[bropty](https://github.com/wlejon/bropty) for terminal emulation and PTY
management, and [brosearch](https://github.com/wlejon/brosearch) for regex
search. Compressed inline images can optionally be decoded with
[broimage](https://github.com/wlejon/broimage). bromux does not depend on bro or
bronze; any terminal or GUI can embed the client library or use the CLI standalone.

## Platforms

Platform support is verified in continuous integration across GCC, Clang, and MSVC:

| Platform | Compiler | Local IPC | Event Loop | PTY Substrate |
|----------|----------|-----------|------------|---------------|
| **Linux** (x86-64, AArch64) | GCC 12+, Clang 16+ | Unix domain sockets (mode 0600 in 0700 dir, `SO_PEERCRED` verification) | `poll` + self-pipe | bropty POSIX PTY |
| **Windows** (x86-64) | MSVC 2022+ | Named Pipes (`\\.\pipe\bromux-<SID>-<name>`, restricted DACL, remote clients rejected) | I/O Completion Ports (IOCP) | bropty ConPTY |
| **macOS** (Apple Silicon, Intel) | Apple Clang | Unix domain sockets (mode 0600, `LOCAL_PEERCRED` verification) | `poll` + self-pipe | bropty POSIX PTY |

## Key features

- **Server-side emulation.** The server runs a bropty `Terminal` per session. Clients receive *state* (rows, cursor, modes, title, palette), never a raw byte replay. Reattaching sends one snapshot. Updates are dirty-row diffs computed per client, with scroll detection. Scrollback is fetched on demand.
- **Many clients per session**, under a resize policy (latest / smallest / largest / fixed). Clients can also attach read-only or no-resize.
- **Forwarded events:** title, cwd (OSC 7), bell, notifications (OSC 9 / 777), progress, OSC 133 marks, and OSC 52 clipboard writes and queries behind a per-session policy.
- **Flow control:**
  - A flooding session gets a bounded slice per loop turn, so it cannot starve others.
  - A slow or stalled client gets fewer, larger frames: each frame is a diff to the latest state, under an acknowledgement window, so it is never sent a backlog.
  - A dead client costs nothing.
- **Local IPC security:**
  - Unix sockets: mode 0600 in a 0700 directory, with peer credentials (`SO_PEERCRED` / `LOCAL_PEERCRED`) verified.
  - Windows named pipes: a DACL admitting only the current user SID, remote callers rejected via `PipeMode::RejectRemoteClients`, and the server process owner verified by the client.
- **Remote attach:** `ssh host bromux proxy` relays bytes to the remote host's local server. Security is ssh's; bromux adds no crypto and no libssh dependency.
- **Daemon lifecycle:**
  - The first client auto-starts the server, which runs detached.
  - The server exits cleanly after an idle timeout (no clients and no active sessions).
  - Race-free start: multiple simultaneous servers resolve to exactly one owner, and orphaned or crashed server sockets are safely reclaimed.
- **Self-contained event loop:** poll plus a self-pipe on POSIX, IOCP on Windows. No libuv, no Asio, no external dependencies.
- **Documented protocol:** Versioned and length-prefixed binary framing. See [docs/protocol.md](docs/protocol.md).

## Building and embedding

### Dependencies

bromux requires [bropty](https://github.com/wlejon/bropty) and, through it,
[brosearch](https://github.com/wlejon/brosearch). CMake resolves each sibling in this order:
1. An existing target already defined in a parent superbuild (e.g. `bro`).
2. Sibling checkouts beside the top-level project (`../bropty`, `../brosearch`, or `-DBROPTY_DIR=<path>` / `-DBROSEARCH_DIR=<path>`).
3. Flat vendored submodules under `third_party/` (`third_party/bropty`, `third_party/brosearch`).

Optionally, [broimage](https://github.com/wlejon/broimage) (with [bromath](https://github.com/wlejon/bromath)) can be provided at `../broimage` or via superbuild target to decode compressed Kitty and iTerm2 inline images.

### Standalone build

```bash
# Sibling layout (clone side by side):
git clone https://github.com/wlejon/brosearch
git clone https://github.com/wlejon/bropty
git clone https://github.com/wlejon/bromux

# Or single checkout with flat submodules:
git clone https://github.com/wlejon/bromux
cd bromux && git submodule update --init --recursive

# Linux / macOS (Ninja)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure -j 1

# Windows (MSVC / Visual Studio 2022)
cmake -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure -j 1
```

The build produces:
- `bromux`: Static library (CMake target `bromux::bromux` or `bromux`).
- `bromux_cli`: Server, SSH proxy, and command-line client executable (binary named `bromux`).

### Embedding in a CMake project

Consumers embed bromux by including its directory and linking against `bromux::bromux`:

```
my_project/
  third_party/
    bromux/
    bropty/
    brosearch/
```

In your `CMakeLists.txt`:

```cmake
add_subdirectory(third_party/bromux)

target_link_libraries(my_terminal PRIVATE bromux::bromux)
```

Configuration options:
- `BROMUX_BUILD_TESTS`: Build the test suite (default `ON` when top-level, `OFF` when embedded via `add_subdirectory`).
- `BROMUX_WITH_BROIMAGE`: Decode compressed inline images via broimage if found (default `ON`).
- `BROMUX_COVERAGE`: Build with gcov coverage instrumentation on GCC/Clang (default `OFF`).

## API overview

### Core headers

| Header | Role |
|--------|------|
| `bromux/client.h` | `Client`, `ConnectOptions`, `SessionSpec`, `ClientEvent`, `ScreenModel` |
| `bromux/server.h` | `Server`, `ServerConfig`, daemon lifecycle, session and client management |
| `bromux/stream.h` | `IStream`, local Unix domain socket, Windows named pipe, and SSH proxy streams |
| `bromux/screen_model.h`, `bromux/screen_source.h` | Client-side screen state and `bropty::RowSource` adapter for viewport, search, and selection |
| `bromux/protocol.h`, `bromux/wire.h` | Wire protocol messages, framing, and serialization |
| `bromux/tee.h` | Session input/output recording and verification replay |

### Embedding the client

```cpp
#include <bromux/client.h>
#include <iostream>

int main() {
    bromux::ConnectOptions opt;          // default server; auto-started if not running
    std::string err;
    auto c = bromux::Client::connect(opt, &err);
    if (!c) {
        std::cerr << "Connect failed: " << err << "\n";
        return 1;
    }
    // Remote connect alternative:
    // auto c = bromux::Client::connect_ssh({.host = "me@box"}, opt, &err);

    bromux::SessionSpec spec;            // empty command launches user's default shell
    spec.meta = {{"name", "build"}};
    auto info = c->create_session(spec);
    c->attach(info->id, 120, 40);

    std::vector<bromux::ClientEvent> events;
    for (;;) {
        c->wait(std::chrono::milliseconds(16));
        events.clear();
        c->dispatch(events);             // applies incoming frames, sends acknowledgements
        const bromux::ScreenModel* m = c->screen(info->id);
        if (m) {
            // m->row(y) provides a bropty::RowView; m->cursor(), m->modes(), m->title() ...
        }
    }
}
```

- **Input & Resizing:** Driven through `send_key`, `send_text`, `paste`, `send_mouse`, `focus`, and `resize`.
- **History Access:** `fetch_history(session, start, count)` reads scrollback asynchronously. Rows carry absolute identifiers surviving eviction (`ScreenModel::history_first_row()`, `history_epoch()`).
- **Terminal Views:** `view(session)` returns a `bropty::TerminalView` over the session: viewports, selection, copy, regex search, and hyperlinks function identically to a local `bropty::Terminal`.
- **Thread Safety:** `Client` is not thread-safe and is designed to run on the UI/render thread; internal stream readers operate asynchronously.

## Command line

```
bromux server [-L name|--socket addr] [--daemon] [--idle-exit SEC] [--log FILE]
              [--tee-dir DIR] [--clipboard deny|write|readwrite]
bromux proxy  [-L name|--socket addr]              # remote end invoked by ssh
bromux ls | new | attach | kill | kill-server  [target] ...
bromux version
target: -L name | --socket addr | --ssh [user@]host [--remote-bromux PATH] [--ssh-arg ARG]...
```

- `bromux new -- htop`: Spawns a session and outputs its numeric ID.
- `bromux attach 1`: Attaches to session 1 in the current terminal (`Ctrl-]` detaches).
- `bromux ls --ssh me@box`: Lists running sessions on remote host `box`.

## Endpoints

| Platform | Default endpoint |
|----------|------------------|
| POSIX | `$XDG_RUNTIME_DIR/bromux/<name>.sock`, fallback `${TMPDIR:-/tmp}/bromux-<uid>/<name>.sock` |
| Windows | `\\.\pipe\bromux-<user SID>-<name>` |

The default server name is `default`. Override with `-L name` or set `BROMUX_SOCKET`.

## Tests

Every test is a real ctest executable that fails in Release builds (no reliance on `assert()`).

### Test breakdown

| Test | Coverage |
|------|----------|
| `test_wire` | Binary primitives, framing, all message types, fuzzed input |
| `test_codec` | Row and screen diff codecs against random terminals, scroll operations, malformed packets |
| `test_source` | `ScreenSource` and `bropty::TerminalView` over remote streams, asynchronous history arrivals, epoch numbering, eviction |
| `test_server` | Session management, input routing, resize policies, forwarded events, clipboard policies, metadata/blobs, frame diffs, disconnects |
| `test_oracle` | Random VT streams and real shells under multi-client churn; vi session testing on POSIX |
| `test_flow` | Flood fairness across sessions, stalled client acknowledgement windows, client process abrupt kills |
| `test_daemon` | Daemon auto-start, background persistence, idle exit timeout, concurrent start races, crashed server cleanup, CLI and proxy operations |
| `test_remote` | End-to-end oracle stream verification over live `ssh host bromux proxy` |

### CI skips and environment requirements

- **`test_remote` skip conditions**:
  - `test_remote` is skipped by default (exits with code 77) unless the `BROMUX_TEST_SSH` environment variable specifies a target host:

    | Variable | Purpose |
    |----------|---------|
    | `BROMUX_TEST_SSH` | Target `[user@]host` |
    | `BROMUX_TEST_SSH_BROMUX` | Optional absolute path to `bromux` binary on remote host |
    | `BROMUX_TEST_SSH_CHILD` | Optional path to `mux_child` test helper on remote host |
    | `BROMUX_TEST_SSH_ARGS` | Optional extra ssh command-line arguments |

  - **Windows CI**: Skipped on Windows runners because the test driver manages session recordings via a POSIX shell (`mkdir`, `cat`, and `rm` under `/tmp`), which standard Windows runners lack.
  - **Linux and macOS CI**: Automatically enabled via `.github/ci/ssh-localhost.sh`, which configures an isolated localhost-only SSH key pair with forwarding and PTY disabled, runs the local `sshd`, and executes `test_remote` over loopback.
- **Process isolation**:
  - End-to-end server tests run serially (`-j 1` in CI) to ensure `process_census` can strictly verify that no orphaned daemon servers or child PTY processes are left running after test execution.

## License

MIT; see [LICENSE](LICENSE).
