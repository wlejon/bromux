// Windows event loop: overlapped named pipes on an I/O completion port.
//
// One pipe instance always waits in ConnectNamedPipe; when a client takes
// it, it becomes a connection and a fresh instance takes its place. Each
// connection has at most one read and one write in flight; their
// OVERLAPPEDs live in the connection, which is freed only once both have
// completed (cancellation included).
#include "event_loop.h"
#include "win_util.h"

#include <sddl.h>

#include <atomic>
#include <chrono>
#include <unordered_map>
#include <vector>

namespace bromux::detail {

namespace {

constexpr ULONG_PTR kPipeKey = 1;
constexpr ULONG_PTR kWakeKey = 2;
constexpr DWORD kReadSize = 64u << 10;
constexpr DWORD kPipeBuffer = 64u << 10;
constexpr auto kFlushTimeout = std::chrono::seconds(2);

class WinLoop final : public EventLoop {
public:
    explicit WinLoop(LoopHandler& h) : handler_(h) {
        iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    }

    ~WinLoop() override {
        close_listener();
        std::vector<Conn*> all;
        for (auto& [id, c] : conns_) all.push_back(c.get());
        for (Conn* c : all) begin_close(*c, false);
        // Every cancelled operation still completes; free nothing before that.
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (outstanding_ops() > 0 && std::chrono::steady_clock::now() < until) poll(50);
        free_dead();
        if (sd_) LocalFree(sd_);
        CloseHandle(iocp_);
    }

    bool listen(const std::string& address, std::string& err, bool& in_use) override {
        in_use = false;
        name_ = win::to_wide(address);
        std::vector<unsigned char> sid = win::current_user_sid();
        wchar_t* sid_str = nullptr;
        if (sid.empty() || !ConvertSidToStringSidW(reinterpret_cast<PSID>(sid.data()), &sid_str)) {
            err = "cannot determine the user's SID";
            return false;
        }
        // Protected DACL: full access for this user only.
        std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sid_str) + L")";
        LocalFree(sid_str);
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd_, nullptr)) {
            err = "cannot build the pipe's security descriptor: " + win::error_text(GetLastError());
            return false;
        }
        if (!new_instance(true, err, in_use)) return false;
        listening_ = true;
        return true;
    }

    void close_listener() override {
        listening_ = false;
        if (listen_pipe_ != INVALID_HANDLE_VALUE) {
            if (accept_.pending) CancelIoEx(listen_pipe_, &accept_.ov);
            else close_listen_pipe();
        }
    }

    void write(ConnId id, std::string_view data) override {
        Conn* c = find(id);
        if (!c || c->dead || data.empty()) return;
        c->out.append(data.data(), data.size());
        if (!c->wr.pending) start_write(*c);
    }

    size_t pending_output(ConnId id) const override {
        auto it = conns_.find(id);
        if (it == conns_.end()) return 0;
        return it->second->out.size() + it->second->inflight.size();
    }

    void close(ConnId id, bool flush_first) override {
        Conn* c = find(id);
        if (!c || c->dead) return;
        if (flush_first && (c->wr.pending || !c->out.empty())) {
            c->close_requested = true;
            c->close_deadline = std::chrono::steady_clock::now() + kFlushTimeout;
            return;
        }
        begin_close(*c, true);
    }

    void wake() override {
        if (!wake_pending_.exchange(true)) PostQueuedCompletionStatus(iocp_, 0, kWakeKey, nullptr);
    }

    void run_once(int timeout_ms) override {
        notify_closed();
        poll(timeout_ms < 0 ? INFINITE : DWORD(timeout_ms));
        expire_flushes();
        notify_closed();
    }

    size_t connections() const override {
        size_t n = 0;
        for (auto& [id, c] : conns_) n += c->dead ? 0 : 1;
        return n;
    }

private:
    enum class OpKind : uint8_t { Accept, Read, Write };
    struct Conn;
    struct Op {
        OVERLAPPED ov{};
        OpKind kind{OpKind::Read};
        Conn* conn{nullptr};
        bool pending{false};
    };
    struct Conn {
        ConnId id{0};
        HANDLE h{INVALID_HANDLE_VALUE};
        Op rd;
        Op wr;
        std::unique_ptr<char[]> rbuf;
        std::string out;       // queued
        std::string inflight;  // being written
        bool dead{false};      // closing: no more callbacks, freed when ops finish
        bool close_requested{false};
        std::chrono::steady_clock::time_point close_deadline{};
    };

    Conn* find(ConnId id) {
        auto it = conns_.find(id);
        return it == conns_.end() ? nullptr : it->second.get();
    }

    size_t outstanding_ops() const {
        size_t n = accept_.pending ? 1 : 0;
        for (auto& [id, c] : conns_) n += size_t(c->rd.pending) + size_t(c->wr.pending);
        return n;
    }

    void close_listen_pipe() {
        if (listen_pipe_ != INVALID_HANDLE_VALUE) CloseHandle(listen_pipe_);
        listen_pipe_ = INVALID_HANDLE_VALUE;
    }

    bool new_instance(bool first, std::string& err, bool& in_use) {
        SECURITY_ATTRIBUTES sa{sizeof sa, sd_, FALSE};
        DWORD open_mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
        HANDLE h = CreateNamedPipeW(name_.c_str(), open_mode,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                    PIPE_UNLIMITED_INSTANCES, kPipeBuffer, kPipeBuffer, 0, &sa);
        if (h == INVALID_HANDLE_VALUE) {
            DWORD e = GetLastError();
            if (first && (e == ERROR_ACCESS_DENIED || e == ERROR_PIPE_BUSY)) in_use = true;
            err = "cannot create pipe " + win::to_utf8(name_) + ": " + win::error_text(e);
            return false;
        }
        if (!CreateIoCompletionPort(h, iocp_, kPipeKey, 0)) {
            err = "CreateIoCompletionPort: " + win::error_text(GetLastError());
            CloseHandle(h);
            return false;
        }
        listen_pipe_ = h;
        accept_ = Op{};
        accept_.kind = OpKind::Accept;
        accept_.pending = true;
        if (!ConnectNamedPipe(h, &accept_.ov)) {
            DWORD e = GetLastError();
            if (e == ERROR_PIPE_CONNECTED) {
                // A client got in between create and connect: complete it by hand.
                accept_.ov.Internal = 0;
                PostQueuedCompletionStatus(iocp_, 0, kPipeKey, &accept_.ov);
            } else if (e != ERROR_IO_PENDING) {
                accept_.pending = false;
                err = "ConnectNamedPipe: " + win::error_text(e);
                close_listen_pipe();
                return false;
            }
        }
        return true;
    }

    void poll(DWORD timeout) {
        OVERLAPPED_ENTRY entries[64];
        ULONG n = 0;
        if (!GetQueuedCompletionStatusEx(iocp_, entries, 64, &n, timeout, FALSE)) return;
        for (ULONG i = 0; i < n; ++i) {
            const OVERLAPPED_ENTRY& e = entries[i];
            if (e.lpCompletionKey == kWakeKey) {
                wake_pending_ = false;
                continue;
            }
            if (!e.lpOverlapped) continue;
            Op* op = CONTAINING_RECORD(e.lpOverlapped, Op, ov);
            const bool ok = e.lpOverlapped->Internal == 0;  // NTSTATUS STATUS_SUCCESS
            complete(*op, ok, e.dwNumberOfBytesTransferred);
        }
        free_dead();
    }

    void complete(Op& op, bool ok, DWORD bytes) {
        op.pending = false;
        switch (op.kind) {
        case OpKind::Accept: return accepted(ok);
        case OpKind::Read: return read_done(*op.conn, ok, bytes);
        case OpKind::Write: return write_done(*op.conn, ok, bytes);
        }
    }

    void accepted(bool ok) {
        HANDLE h = listen_pipe_;
        listen_pipe_ = INVALID_HANDLE_VALUE;
        if (!listening_) {
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            return;
        }
        // Replace the instance first, so the next client never finds no pipe.
        std::string err;
        bool in_use = false;
        if (!new_instance(false, err, in_use)) listening_ = false;
        if (!ok) {
            CloseHandle(h);
            return;
        }
        auto c = std::make_unique<Conn>();
        c->id = next_id_++;
        c->h = h;
        c->rd.kind = OpKind::Read;
        c->rd.conn = c.get();
        c->wr.kind = OpKind::Write;
        c->wr.conn = c.get();
        c->rbuf = std::make_unique<char[]>(kReadSize);
        Conn& ref = *c;
        conns_.emplace(c->id, std::move(c));
        handler_.on_accept(ref.id);
        if (!ref.dead) start_read(ref);
    }

    void start_read(Conn& c) {
        c.rd.ov = OVERLAPPED{};
        c.rd.pending = true;
        if (!ReadFile(c.h, c.rbuf.get(), kReadSize, nullptr, &c.rd.ov) && GetLastError() != ERROR_IO_PENDING) {
            c.rd.pending = false;
            begin_close(c, true);
        }
    }

    void read_done(Conn& c, bool ok, DWORD bytes) {
        if (c.dead) return maybe_free(c);
        if (!ok || bytes == 0) return begin_close(c, true);
        if (!c.close_requested) handler_.on_data(c.id, c.rbuf.get(), bytes);
        if (!c.dead) start_read(c);
    }

    void start_write(Conn& c) {
        c.inflight.swap(c.out);
        c.out.clear();
        c.wr.ov = OVERLAPPED{};
        c.wr.pending = true;
        if (!WriteFile(c.h, c.inflight.data(), DWORD(c.inflight.size()), nullptr, &c.wr.ov) &&
            GetLastError() != ERROR_IO_PENDING) {
            c.wr.pending = false;
            begin_close(c, true);
        }
    }

    void write_done(Conn& c, bool ok, DWORD bytes) {
        if (c.dead) return maybe_free(c);
        if (!ok) return begin_close(c, true);
        if (bytes < c.inflight.size()) {
            c.out.insert(0, c.inflight, bytes, std::string::npos);
        }
        c.inflight.clear();
        if (!c.out.empty()) start_write(c);
        else if (c.close_requested) begin_close(c, true);
    }

    void expire_flushes() {
        const auto now = std::chrono::steady_clock::now();
        std::vector<Conn*> late;
        for (auto& [id, c] : conns_)
            if (!c->dead && c->close_requested && now >= c->close_deadline) late.push_back(c.get());
        for (Conn* c : late) begin_close(*c, true);
    }

    void begin_close(Conn& c, bool notify) {
        if (c.dead) return;
        c.dead = true;
        if (notify) closed_.push_back(c.id);
        if (c.rd.pending || c.wr.pending) CancelIoEx(c.h, nullptr);
        maybe_free(c);
    }

    // A dead connection is freed once both operations have finished, and only
    // between completions (never under a callback that still holds it).
    void maybe_free(Conn& c) {
        if (!c.rd.pending && !c.wr.pending) dead_.push_back(c.id);
    }

    void free_dead() {
        for (ConnId id : dead_) {
            auto it = conns_.find(id);
            if (it == conns_.end()) continue;
            Conn& c = *it->second;
            if (c.rd.pending || c.wr.pending) continue;
            DisconnectNamedPipe(c.h);
            CloseHandle(c.h);
            conns_.erase(it);
        }
        dead_.clear();
    }

    void notify_closed() {
        while (!closed_.empty()) {
            std::vector<ConnId> ids;
            ids.swap(closed_);
            for (ConnId id : ids) handler_.on_closed(id);
        }
    }

    LoopHandler& handler_;
    HANDLE iocp_{nullptr};
    std::wstring name_;
    PSECURITY_DESCRIPTOR sd_{nullptr};
    HANDLE listen_pipe_{INVALID_HANDLE_VALUE};
    Op accept_;
    bool listening_{false};
    std::unordered_map<ConnId, std::unique_ptr<Conn>> conns_;
    std::vector<ConnId> closed_;
    std::vector<ConnId> dead_;
    ConnId next_id_{1};
    std::atomic<bool> wake_pending_{false};
};

}  // namespace

std::unique_ptr<EventLoop> EventLoop::create(LoopHandler& handler) { return std::make_unique<WinLoop>(handler); }

}  // namespace bromux::detail
