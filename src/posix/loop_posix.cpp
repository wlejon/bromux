// POSIX event loop: poll() over a Unix-socket listener, its connections and
// a self-pipe for wake(). A `<socket>.lock` file held with flock() decides
// which server owns the address, so a stale socket left by a crashed server
// is reclaimed and two servers never fight over one.
#include "event_loop.h"
#include "posix_util.h"

#include <poll.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <atomic>
#include <chrono>
#include <unordered_map>
#include <vector>

namespace bromux::detail {

namespace {

constexpr size_t kReadSize = 64u << 10;
constexpr size_t kReadPerTurn = 1u << 20;  // per connection per turn
constexpr auto kFlushTimeout = std::chrono::seconds(2);

bool peer_is_us(int fd) {
#if defined(SO_PEERCRED)
    struct ucred cred {};
    socklen_t len = sizeof cred;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) return false;
    return cred.uid == ::getuid();
#else
    uid_t uid = 0;
    gid_t gid = 0;
    if (getpeereid(fd, &uid, &gid) != 0) return false;
    return uid == ::getuid();
#endif
}

class PosixLoop final : public EventLoop {
public:
    explicit PosixLoop(LoopHandler& h) : handler_(h) {
        int p[2];
        if (::pipe(p) == 0) {
            wake_r_ = p[0];
            wake_w_ = p[1];
            for (int fd : p) {
                posix::set_cloexec(fd);
                posix::set_nonblocking(fd);
            }
        }
        buf_.resize(kReadSize);
    }

    ~PosixLoop() override {
        close_listener();
        for (auto& [id, c] : conns_) ::close(c->fd);
        conns_.clear();
        if (lock_fd_ >= 0) ::close(lock_fd_);
        if (wake_r_ >= 0) ::close(wake_r_);
        if (wake_w_ >= 0) ::close(wake_w_);
    }

    bool listen(const std::string& address, std::string& err, bool& in_use) override {
        in_use = false;
        sockaddr_un sa{};
        sa.sun_family = AF_UNIX;
        if (address.size() >= sizeof sa.sun_path) {
            err = "socket path too long: " + address;
            return false;
        }
        std::memcpy(sa.sun_path, address.c_str(), address.size() + 1);
        // Ownership of the address: whoever holds the lock.
        const std::string lock_path = address + ".lock";
        lock_fd_ = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (lock_fd_ < 0) {
            err = "cannot open " + lock_path + ": " + posix::errno_text(errno);
            return false;
        }
        if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
            err = "another server owns " + address;
            in_use = true;
            ::close(lock_fd_);
            lock_fd_ = -1;
            return false;
        }
        ::unlink(address.c_str());  // stale: its server is gone (it held the lock)
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            err = "socket: " + posix::errno_text(errno);
            return false;
        }
        posix::set_cloexec(fd);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) {
            err = "cannot bind " + address + ": " + posix::errno_text(errno);
            ::close(fd);
            return false;
        }
        ::chmod(address.c_str(), 0600);
        if (::listen(fd, 64) != 0 || !posix::set_nonblocking(fd)) {
            err = "listen: " + posix::errno_text(errno);
            ::close(fd);
            ::unlink(address.c_str());
            return false;
        }
        listen_fd_ = fd;
        path_ = address;
        return true;
    }

    void close_listener() override {
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            ::unlink(path_.c_str());  // we hold the lock, so it is ours
        }
    }

    void write(ConnId id, std::string_view data) override {
        Conn* c = find(id);
        if (!c || c->dead || data.empty()) return;
        if (c->out.size() == c->head) {
            // Nothing queued: try the socket directly.
            ssize_t r = ::send(c->fd, data.data(), data.size(), posix::send_flags());
            if (r < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    begin_close(*c);
                    return;
                }
                r = 0;
            }
            data.remove_prefix(size_t(r));
            if (data.empty()) return;
        }
        c->out.append(data.data(), data.size());
    }

    size_t pending_output(ConnId id) const override {
        auto it = conns_.find(id);
        return it == conns_.end() ? 0 : it->second->out.size() - it->second->head;
    }

    void close(ConnId id, bool flush_first) override {
        Conn* c = find(id);
        if (!c || c->dead) return;
        if (flush_first && c->out.size() > c->head) {
            c->close_requested = true;
            c->close_deadline = std::chrono::steady_clock::now() + kFlushTimeout;
            return;
        }
        begin_close(*c);
    }

    void wake() override {
        if (!wake_pending_.exchange(true) && wake_w_ >= 0) {
            char b = 1;
            ssize_t r = ::write(wake_w_, &b, 1);
            (void)r;
        }
    }

    size_t connections() const override {
        size_t n = 0;
        for (auto& [id, c] : conns_) n += c->dead ? 0 : 1;
        return n;
    }

    void run_once(int timeout_ms) override {
        notify_closed();
        fds_.clear();
        ids_.clear();
        fds_.push_back(pollfd{wake_r_, POLLIN, 0});
        ids_.push_back(0);
        if (listen_fd_ >= 0) {
            fds_.push_back(pollfd{listen_fd_, POLLIN, 0});
            ids_.push_back(0);
        }
        const size_t first_conn = fds_.size();
        for (auto& [id, c] : conns_) {
            if (c->dead) continue;
            short ev = POLLIN;
            if (c->out.size() > c->head) ev |= POLLOUT;
            fds_.push_back(pollfd{c->fd, ev, 0});
            ids_.push_back(id);
        }
        int r = ::poll(fds_.data(), nfds_t(fds_.size()), timeout_ms);
        if (r > 0) {
            if (fds_[0].revents) {
                char b[64];
                while (::read(wake_r_, b, sizeof b) > 0) {
                }
                wake_pending_ = false;
            }
            if (listen_fd_ >= 0 && first_conn == 2 && fds_[1].revents) accept_all();
            for (size_t i = first_conn; i < fds_.size(); ++i) {
                if (!fds_[i].revents) continue;
                Conn* c = find(ids_[i]);
                if (!c || c->dead) continue;
                if (fds_[i].revents & POLLOUT) flush(*c);
                if (!c->dead && (fds_[i].revents & (POLLIN | POLLHUP | POLLERR))) read_from(*c);
            }
        }
        expire_flushes();
        free_dead();
        notify_closed();
    }

private:
    struct Conn {
        ConnId id{0};
        int fd{-1};
        std::string out;
        size_t head{0};
        bool dead{false};
        bool close_requested{false};
        std::chrono::steady_clock::time_point close_deadline{};
    };

    Conn* find(ConnId id) {
        auto it = conns_.find(id);
        return it == conns_.end() ? nullptr : it->second.get();
    }

    void accept_all() {
        for (;;) {
            int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) {
                if (errno == EINTR) continue;
                return;  // EAGAIN, or a transient error (EMFILE...): retry next turn
            }
            posix::set_cloexec(fd);
            if (!peer_is_us(fd) || !posix::set_nonblocking(fd)) {
                ::close(fd);
                continue;
            }
            posix::no_sigpipe(fd);
            auto c = std::make_unique<Conn>();
            c->id = next_id_++;
            c->fd = fd;
            ConnId id = c->id;
            conns_.emplace(id, std::move(c));
            handler_.on_accept(id);
        }
    }

    void read_from(Conn& c) {
        size_t total = 0;
        while (total < kReadPerTurn && !c.dead) {
            ssize_t n = ::recv(c.fd, buf_.data(), buf_.size(), 0);
            if (n > 0) {
                total += size_t(n);
                if (!c.close_requested) handler_.on_data(c.id, buf_.data(), size_t(n));
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            begin_close(c);  // EOF or error
            return;
        }
    }

    void flush(Conn& c) {
        while (c.out.size() > c.head) {
            ssize_t r = ::send(c.fd, c.out.data() + c.head, c.out.size() - c.head, posix::send_flags());
            if (r < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                begin_close(c);
                return;
            }
            c.head += size_t(r);
        }
        if (c.head == c.out.size()) {
            c.out.clear();
            c.head = 0;
            if (c.close_requested) begin_close(c);
        } else if (c.head > (1u << 20)) {
            c.out.erase(0, c.head);
            c.head = 0;
        }
    }

    void expire_flushes() {
        const auto now = std::chrono::steady_clock::now();
        for (auto& [id, c] : conns_)
            if (!c->dead && c->close_requested && now >= c->close_deadline) begin_close(*c);
    }

    void begin_close(Conn& c) {
        if (c.dead) return;
        c.dead = true;
        closed_.push_back(c.id);
    }

    void free_dead() {
        for (auto it = conns_.begin(); it != conns_.end();) {
            if (it->second->dead) {
                ::close(it->second->fd);
                it = conns_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void notify_closed() {
        while (!closed_.empty()) {
            std::vector<ConnId> ids;
            ids.swap(closed_);
            for (ConnId id : ids) handler_.on_closed(id);
        }
    }

    LoopHandler& handler_;
    int wake_r_{-1};
    int wake_w_{-1};
    std::atomic<bool> wake_pending_{false};
    int listen_fd_{-1};
    int lock_fd_{-1};
    std::string path_;
    std::unordered_map<ConnId, std::unique_ptr<Conn>> conns_;
    std::vector<ConnId> closed_;
    ConnId next_id_{1};
    std::vector<pollfd> fds_;
    std::vector<ConnId> ids_;
    std::vector<char> buf_;
};

}  // namespace

std::unique_ptr<EventLoop> EventLoop::create(LoopHandler& handler) { return std::make_unique<PosixLoop>(handler); }

}  // namespace bromux::detail
