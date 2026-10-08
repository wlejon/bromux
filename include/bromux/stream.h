#pragma once
// Blocking byte streams for clients and the proxy, and process spawning:
// brolink's (brolink/stream.h), under bromux's names.
//
// A Stream is read by one thread and written by others (writes must be
// serialised by the caller); shutdown() from any thread unblocks a pending
// read. Kinds: a connection to a local server (Unix socket / named pipe),
// a child process's stdin/stdout (how remote attach runs `ssh host bromux
// proxy`), and this process's own stdin/stdout (the proxy's end).

#include <brolink/stream.h>

namespace bromux {

using brolink::Process;
using brolink::Stream;
// connect_local(address, err, not_running): the server at `address` (paths.h).
using brolink::connect_local;
using brolink::spawn_detached;
using brolink::spawn_stream;
using brolink::stdio_stream;

}  // namespace bromux
