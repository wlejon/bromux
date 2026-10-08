#include "bromux/paths.h"

#include <cstdlib>

namespace bromux {

std::string server_address(std::string_view name, std::string* err) {
    if (name.empty()) name = kDefaultServerName;
    if (!valid_server_name(name)) {
        if (err) *err = "invalid server name '" + std::string(name) + "'";
        return {};
    }
    if (name == kDefaultServerName) {
        if (const char* e = std::getenv("BROMUX_SOCKET"); e && *e) return e;
    }
    return brolink::local_address(kAppName, name, err);
}

}  // namespace bromux
