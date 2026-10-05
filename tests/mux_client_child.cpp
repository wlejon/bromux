// A client process for tests to kill: attaches to a session and then
//   read   keeps dispatching (a live UI)
//   stall  never reads again (a hung UI)
//   spam   keeps writing large input messages while dispatching
// until it is killed. Prints ATTACHED once attached.
//   mux_client_child <address> <session> <mode>
#include "no_dialogs.h"

#include <bromux/client.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    check::no_error_dialogs();
    if (argc < 4) return 2;
    bromux::ConnectOptions o;
    o.address = argv[1];
    o.autostart = false;
    o.client_name = "victim";
    const uint64_t id = std::strtoull(argv[2], nullptr, 10);
    const std::string mode = argv[3];
    std::string err;
    auto c = bromux::Client::connect(o, &err);
    if (!c) {
        std::printf("connect: %s\n", err.c_str());
        return 1;
    }
    if (!c->attach(id, 77, 21, 0, &err)) {
        std::printf("attach: %s\n", err.c_str());
        return 1;
    }
    std::printf("ATTACHED\n");
    std::fflush(stdout);
    std::vector<bromux::ClientEvent> evs;
    const std::string blob(256u << 10, 'z');
    for (;;) {
        if (mode == "stall") {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        if (mode == "spam") c->paste(id, blob);
        c->wait(std::chrono::milliseconds(5));
        evs.clear();
        c->dispatch(evs);
        if (!c->connected()) return 0;
    }
}
