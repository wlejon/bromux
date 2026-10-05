// Windows streams: a named-pipe client connection and this process's stdio.
#include "bromux/stream.h"
#include "win_util.h"

#include <atomic>
#include <mutex>

namespace bromux {

namespace {

// Overlapped named-pipe client. read() and write() each wait on their own
// event plus a shared stop event, so shutdown() unblocks either.
class PipeStream final : public Stream {
public:
    explicit PipeStream(HANDLE h) : h_(h) {
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        rd_ev_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        wr_ev_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    ~PipeStream() override {
        shutdown();
        CloseHandle(h_);
        CloseHandle(stop_);
        CloseHandle(rd_ev_);
        CloseHandle(wr_ev_);
    }

    size_t read(char* buf, size_t n) override {
        if (stopped_) return 0;
        OVERLAPPED ov{};
        ov.hEvent = rd_ev_;
        ResetEvent(rd_ev_);
        DWORD got = 0;
        if (!ReadFile(h_, buf, DWORD(n > 0x7FFFFFFF ? 0x7FFFFFFF : n), nullptr, &ov)) {
            if (GetLastError() != ERROR_IO_PENDING) return 0;
            if (!wait(ov)) return 0;
        }
        if (!GetOverlappedResult(h_, &ov, &got, TRUE)) return 0;
        return got;
    }

    bool write(std::string_view data) override {
        while (!data.empty()) {
            if (stopped_) return false;
            OVERLAPPED ov{};
            ov.hEvent = wr_ev_;
            ResetEvent(wr_ev_);
            DWORD put = 0;
            const DWORD chunk = DWORD(data.size() > (1u << 20) ? (1u << 20) : data.size());
            if (!WriteFile(h_, data.data(), chunk, nullptr, &ov)) {
                if (GetLastError() != ERROR_IO_PENDING) return false;
                if (!wait(ov)) return false;
            }
            if (!GetOverlappedResult(h_, &ov, &put, TRUE) || put == 0) return false;
            data.remove_prefix(put);
        }
        return true;
    }

    void shutdown() override {
        stopped_ = true;
        SetEvent(stop_);
    }

private:
    // Wait for `ov` or the stop event; on stop, cancel and reap the I/O.
    bool wait(OVERLAPPED& ov) {
        HANDLE hs[2] = {ov.hEvent, stop_};
        DWORD r = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
        if (r == WAIT_OBJECT_0) return true;
        CancelIoEx(h_, &ov);
        DWORD dummy = 0;
        GetOverlappedResult(h_, &ov, &dummy, TRUE);
        return false;
    }

    HANDLE h_;
    HANDLE stop_;
    HANDLE rd_ev_;
    HANDLE wr_ev_;
    std::atomic<bool> stopped_{false};
};

// Synchronous handles (this process's stdin / stdout).
class HandleStream final : public Stream {
public:
    HandleStream(HANDLE in, HANDLE out) : in_(in), out_(out) {}
    size_t read(char* buf, size_t n) override {
        if (stopped_) return 0;
        DWORD got = 0;
        if (!ReadFile(in_, buf, DWORD(n > 0x7FFFFFFF ? 0x7FFFFFFF : n), &got, nullptr)) return 0;
        return got;
    }
    bool write(std::string_view data) override {
        while (!data.empty()) {
            if (stopped_) return false;
            DWORD put = 0;
            const DWORD chunk = DWORD(data.size() > (1u << 20) ? (1u << 20) : data.size());
            if (!WriteFile(out_, data.data(), chunk, &put, nullptr) || put == 0) return false;
            data.remove_prefix(put);
        }
        return true;
    }
    void shutdown() override { stopped_ = true; }

private:
    HANDLE in_;
    HANDLE out_;
    std::atomic<bool> stopped_{false};
};

}  // namespace

std::unique_ptr<Stream> connect_local(const std::string& address, std::string* err, bool* not_running) {
    if (not_running) *not_running = false;
    const std::wstring name = win::to_wide(address);
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 50; ++attempt) {
        h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (h != INVALID_HANDLE_VALUE) break;
        DWORD e = GetLastError();
        if (e == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(name.c_str(), 200);
            continue;
        }
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
            if (not_running) *not_running = true;
            if (err) *err = "no server is listening at " + address;
        } else if (err) {
            *err = "cannot connect to " + address + ": " + win::error_text(e);
        }
        return nullptr;
    }
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = "the server at " + address + " stays busy";
        return nullptr;
    }
    // Refuse a pipe served by another user (a squatter on our pipe name).
    ULONG pid = 0;
    bool same_user = false;
    if (GetNamedPipeServerProcessId(h, &pid)) {
        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (proc) {
            same_user = !win::current_user_sid().empty() && win::process_user_sid(proc) == win::current_user_sid();
            CloseHandle(proc);
        }
    }
    if (!same_user) {
        CloseHandle(h);
        if (err) *err = "the process serving " + address + " does not run as this user; refusing it";
        return nullptr;
    }
    return std::make_unique<PipeStream>(h);
}

std::unique_ptr<Stream> stdio_stream() {
    return std::make_unique<HandleStream>(GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE));
}

}  // namespace bromux
