// `bromux proxy`: the remote end of `ssh host bromux proxy`. A byte relay:
// the protocol runs end to end between the client and the server, so the
// proxy needs no knowledge of it (and adds no state to lose on a reconnect).
#include "bromux/client.h"

#include <atomic>
#include <memory>
#include <thread>

namespace bromux {

int run_proxy(const ConnectOptions& options, std::string* err) {
    std::shared_ptr<Stream> server(connect_or_start(options, err));
    if (!server) return 1;
    std::shared_ptr<Stream> io(stdio_stream());

    // stdin -> server on its own thread (detached: a blocked stdin read must
    // not hold up exit once the server side has gone).
    std::thread([server, io] {
        std::unique_ptr<char[]> buf(new char[64u << 10]);
        for (;;) {
            size_t n = io->read(buf.get(), 64u << 10);
            if (n == 0 || !server->write(std::string_view(buf.get(), n))) break;
        }
        server->shutdown();  // ends the other direction too
    }).detach();

    // server -> stdout here.
    std::unique_ptr<char[]> buf(new char[64u << 10]);
    for (;;) {
        size_t n = server->read(buf.get(), 64u << 10);
        if (n == 0 || !io->write(std::string_view(buf.get(), n))) break;
    }
    server->shutdown();
    return 0;
}

}  // namespace bromux
