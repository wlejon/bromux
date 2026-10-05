#pragma once
// Where a server listens, per user.
//
// POSIX: a Unix socket <dir>/<name>.sock, where <dir> is
// $XDG_RUNTIME_DIR/bromux when that is set, else ${TMPDIR:-/tmp}/bromux-<uid>.
// The directory is created 0700 and must be owned by the user and not
// group/world accessible, or it is refused (someone else could otherwise
// plant a socket there). The server additionally checks every client's
// credentials (SO_PEERCRED / getpeereid) and the client checks the server's.
//
// Windows: a named pipe \\.\pipe\bromux-<user SID>-<name>, created with
// FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_REJECT_REMOTE_CLIENTS and a DACL that
// grants only the user's SID; the client checks that the process serving
// the pipe runs as the same user before speaking (no pipe squatting).
//
// $BROMUX_SOCKET, when set, overrides the address for the default name.

#include <cstdint>
#include <string>
#include <string_view>

namespace bromux {

inline constexpr std::string_view kDefaultServerName = "default";

// Names are 1..64 of [A-Za-z0-9_.-].
[[nodiscard]] bool valid_server_name(std::string_view name) noexcept;

// The endpoint of server `name` (empty = the default). Empty on failure
// (`err` says why).
[[nodiscard]] std::string server_address(std::string_view name, std::string* err = nullptr);

// Per-user directory for server logs (created if missing).
[[nodiscard]] std::string runtime_dir(std::string* err = nullptr);

// Absolute path of the running executable.
[[nodiscard]] std::string current_executable();
[[nodiscard]] uint64_t current_pid();
[[nodiscard]] uint64_t unix_time_ms();

}  // namespace bromux
