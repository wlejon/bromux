#include "bromux/paths.h"
#include "posix_util.h"

#include <sys/stat.h>
#include <sys/un.h>

#include <climits>
#include <cstdlib>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace bromux {

namespace {

// Create `dir` 0700 if missing; refuse one that is not a private directory of ours.
bool ensure_private_dir(const std::string& dir, std::string* err) {
    if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        if (err) *err = "cannot create " + dir + ": " + posix::errno_text(errno);
        return false;
    }
    struct stat st {};
    if (::lstat(dir.c_str(), &st) != 0) {
        if (err) *err = "cannot stat " + dir + ": " + posix::errno_text(errno);
        return false;
    }
    if (!S_ISDIR(st.st_mode) || st.st_uid != ::getuid()) {
        if (err) *err = dir + " is not a directory owned by this user; refusing it";
        return false;
    }
    if ((st.st_mode & 077) != 0) {
        if (err) *err = dir + " is accessible to other users; refusing it";
        return false;
    }
    return true;
}

std::string base_dir(std::string* err) {
    if (const char* x = std::getenv("XDG_RUNTIME_DIR"); x && *x) {
        struct stat st {};
        if (::stat(x, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == ::getuid()) {
            std::string dir = std::string(x) + "/bromux";
            if (ensure_private_dir(dir, err)) return dir;
            return {};
        }
    }
    std::string tmp = "/tmp";
    if (const char* t = std::getenv("TMPDIR"); t && *t) tmp = t;
    while (tmp.size() > 1 && tmp.back() == '/') tmp.pop_back();
    std::string dir = tmp + "/bromux-" + std::to_string(::getuid());
    if (!ensure_private_dir(dir, err)) return {};
    return dir;
}

}  // namespace

std::string server_address(std::string_view name, std::string* err) {
    if (name.empty()) name = kDefaultServerName;
    if (!valid_server_name(name)) {
        if (err) *err = "invalid server name '" + std::string(name) + "'";
        return {};
    }
    if (name == kDefaultServerName) {
        if (const char* e = std::getenv("BROMUX_SOCKET"); e && *e) return e;
    }
    std::string dir = base_dir(err);
    if (dir.empty()) return {};
    std::string path = dir + "/" + std::string(name) + ".sock";
    if (path.size() >= sizeof(sockaddr_un::sun_path)) {
        if (err) *err = "socket path too long: " + path;
        return {};
    }
    return path;
}

std::string runtime_dir(std::string* err) { return base_dir(err); }

std::string current_executable() {
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    buf.resize(std::strlen(buf.c_str()));
    char real[PATH_MAX];
    if (::realpath(buf.c_str(), real)) return real;
    return buf;
#else
    char buf[PATH_MAX];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return {};
    return std::string(buf, size_t(n));
#endif
}

uint64_t current_pid() { return uint64_t(::getpid()); }

}  // namespace bromux
