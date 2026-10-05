#include "bromux/paths.h"

#include <chrono>

namespace bromux {

bool valid_server_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 64) return false;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                        c == '.' || c == '-';
        if (!ok) return false;
    }
    return name != "." && name != "..";
}

uint64_t unix_time_ms() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count());
}

}  // namespace bromux
