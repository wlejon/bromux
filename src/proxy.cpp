// `bromux proxy`: the remote end of `ssh host bromux proxy`. A byte relay
// (brolink's): the protocol runs end to end between the client and the
// server, so the proxy needs no knowledge of it (and adds no state to lose
// on a reconnect).
#include "bromux/client.h"

#include <brolink/proxy.h>

namespace bromux {

int run_proxy(const ConnectOptions& options, std::string* err) {
    brolink::ProxyOptions po;
    po.name = "bromux proxy";
    return brolink::run_proxy([&](std::string* e) { return connect_or_start(options, e); }, po, err);
}

}  // namespace bromux
