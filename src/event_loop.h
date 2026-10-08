#pragma once
// The server's event loop: brolink's (brolink/loop.h). A listener plus
// nonblocking connections, all callbacks on the thread calling run_once();
// writes queue, and pending_output() lets the server apply its own flow
// control.

#include <brolink/loop.h>

namespace bromux::detail {

using brolink::ConnId;
using brolink::EventLoop;
using brolink::LoopHandler;

}  // namespace bromux::detail
