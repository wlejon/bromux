#pragma once
// Server internals shared by server*.cpp. Everything here runs on the loop
// thread except Waker::wake() (bropty reader threads) and stop().

#include "bromux/codec.h"
#include "bromux/protocol.h"
#include "bromux/server.h"
#include "bromux/tee.h"
#include "event_loop.h"

#include <bropty/session.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bromux::detail {

using Clock = std::chrono::steady_clock;

class ServerCore;
struct ServerSession;
struct Conn;

// Lets PTY reader threads wake the loop without outliving it.
struct Waker {
    std::mutex mu;
    EventLoop* loop{nullptr};
    void wake() {
        std::lock_guard<std::mutex> lk(mu);
        if (loop) loop->wake();
    }
};

// One client's view of one session.
struct Attachment {
    Conn* conn{nullptr};
    ServerSession* session{nullptr};
    uint32_t flags{0};
    int cols{0};
    int rows{0};
    uint64_t activity{0};  // ServerCore::activity_ at its last input / resize / attach

    // What this client has been sent (the server's model of its ScreenModel).
    int sent_cols{-1};
    int sent_rows{-1};
    std::vector<uint64_t> sent_key;  // ServerSession::row_key of each row it has
    bropty::CursorState sent_cursor{};
    bool cursor_sent{false};
    ModeState sent_modes{};
    bool modes_sent{false};
    std::string sent_title;
    std::string sent_icon;
    std::string sent_cwd;
    bool text_sent{false};
    uint64_t sent_palette{0};
    bool palette_sent{false};
    uint64_t sent_history_first{0};
    uint64_t sent_history{0};
    uint64_t sent_epoch{0};
    bool history_sent{false};
    uint64_t sent_version{UINT64_MAX};  // session feed_seq the last frame showed

    // Flow control: frames sent but not yet acknowledged.
    uint64_t frame_seq{0};
    std::deque<std::pair<uint64_t, size_t>> inflight;
    size_t inflight_bytes{0};
    Clock::time_point last_frame{};
    bool force_frame{false};
    std::vector<uint32_t> sync_reqs;

    // Events waiting to be written (bounded).
    std::deque<std::string> events;
    size_t events_bytes{0};
    uint64_t events_dropped{0};
    bool last_event_bell{false};

    [[nodiscard]] bool read_only() const noexcept { return (flags & Attach_ReadOnly) != 0; }
    [[nodiscard]] bool sizes() const noexcept {
        return !(flags & (Attach_ReadOnly | Attach_NoResize)) && cols > 0 && rows > 0;
    }
};

struct Conn {
    ConnId id{0};
    bool hello{false};
    bool closing{false};
    std::string name;
    uint32_t flags{0};
    wire::MessageSplitter splitter;
    std::map<uint64_t, std::unique_ptr<Attachment>> attachments;  // by session id
};

struct ServerSession final : bropty::TerminalHost {
    explicit ServerSession(ServerCore& core) : core(core) {}
    ServerCore& core;

    uint64_t id{0};
    SessionSpec spec;
    Pairs meta;
    uint64_t created_ms{0};
    std::unique_ptr<bropty::Session> term;
    std::shared_ptr<bropty::IPtyProcess> pty;
    bool running{true};
    int exit_code{-1};
    ResizePolicy resize_policy{ResizePolicy::Latest};
    ClipboardPolicy clipboard_policy{ClipboardPolicy::WriteOnly};

    // State version: +1 per PTY output chunk fed and per resize (tee.h).
    uint64_t feed_seq{0};
    TeeWriter tee;
    bool has_pending{false};      // PTY output left over after this turn's slice
    bool sync_active{false};      // ?2026 seen on, holding frames since sync_since
    Clock::time_point sync_since{};

    // The screen as encoded rows, refreshed lazily. Rows are known by the
    // terminal's row stamps (content serials: a row keeps its stamp while it
    // scrolls), so an unchanged row is never re-encoded or compared.
    RowEncoder encoder;
    std::vector<std::string> row_enc;
    std::vector<uint64_t> row_stamp;  // Terminal::row_stamp of each row encoded
    std::vector<uint64_t> row_key;    // what frames compare: the stamp, 0 = a blank row
    std::string blank_enc;
    int enc_cols{-1};
    int enc_rows{-1};
    uint64_t enc_version{UINT64_MAX};
    uint64_t palette_hash{0};

    std::vector<Attachment*> attachments;

    // OSC 52 queries waiting for a client's ClipboardData; `request` is the
    // terminal's id for the answer (Terminal::answer_clipboard).
    struct ClipRequest {
        uint32_t token{0};
        uint64_t request{0};
        ConnId conn{0};
        Clock::time_point at{};
    };
    std::vector<ClipRequest> clip_requests;

    [[nodiscard]] bropty::Terminal& t() noexcept { return term->terminal(); }
    [[nodiscard]] SessionInfo info() const;
    void resize(int cols, int rows);
    // Bring row_enc / row_key up to date with the terminal.
    void refresh();
    void push_event(EventMsg ev);

    // bropty::TerminalHost (Session's delegate)
    void bell() override;
    void title_changed(std::string_view title) override;
    void icon_name_changed(std::string_view name) override;
    void cwd_changed(std::string_view uri) override;
    void clipboard_write(std::string_view selection, std::string_view data) override;
    bool clipboard_read_async(uint64_t request, std::string_view selection) override;
    void notification(std::string_view title, std::string_view body) override;
    void progress(int state, int value) override;
    void semantic_mark(char kind, std::string_view params) override;
    void apc(std::string_view payload) override;
    void resized_by_application(int cols, int rows) override;
};

class ServerCore final : public LoopHandler {
public:
    explicit ServerCore(ServerOptions options);
    ~ServerCore() override;

    bool start(std::string* err, bool* in_use);
    void run();
    void request_stop() {
        stop_requested_ = true;
        if (loop_) loop_->wake();
    }

    // LoopHandler
    void on_accept(ConnId id) override;
    void on_data(ConnId id, const char* data, size_t n) override;
    void on_closed(ConnId id) override;

    // ---- shared with the session / dispatch / frame code
    const ServerOptions& options() const noexcept { return opt_; }
    void log(std::string_view msg) const {
        if (opt_.log) opt_.log(msg);
    }
    template <class M>
    void send(Conn& c, const M& m) {
        loop_->write(c.id, encode(m));
    }
    void send_raw(Conn& c, std::string_view bytes) { loop_->write(c.id, bytes); }
    void send_error(Conn& c, uint32_t req, ErrorCode code, std::string message);
    void close_conn(Conn& c);
    uint64_t next_activity() noexcept { return ++activity_; }
    uint32_t next_token() noexcept { return next_token_++; }
    Conn* find_conn(ConnId id);

    // server_session.cpp
    ServerSession* create_session(const SessionSpec& spec, std::string& err);
    void close_session(uint64_t id);
    void apply_resize_policy(ServerSession& s);
    void notify(NotifyKind kind, ServerSession& s);
    void session_exited(ServerSession& s);

    // server_dispatch.cpp
    void handle(Conn& c, uint16_t type, std::string_view payload);
    void detach(Conn& c, uint64_t session, DetachReason reason, bool tell_client);

    // server_frames.cpp
    bool send_frame(Attachment& a, Clock::time_point now);
    void send_history(Conn& c, const FetchHistoryMsg& m);
    void flush_events(Attachment& a);

private:
    void pump_sessions();
    void flush_frames(Clock::time_point now);
    void expire_clipboard_requests(Clock::time_point now);
    bool idle_expired(Clock::time_point now);
    int next_timeout(Clock::time_point now) const;
    void reap(std::shared_ptr<bropty::IPtyProcess> pty);
    void shutdown();

    ServerOptions opt_;
    std::unique_ptr<EventLoop> loop_;
    std::shared_ptr<Waker> waker_;
    std::map<uint64_t, std::unique_ptr<ServerSession>> sessions_;
    std::unordered_map<ConnId, std::unique_ptr<Conn>> conns_;
    std::unordered_map<std::string, std::string> blobs_;
    size_t blob_bytes_{0};
    std::atomic<bool> stop_requested_{false};
    uint64_t next_session_{1};
    uint64_t activity_{0};
    uint32_t next_token_{1};
    uint64_t start_ms_{0};
    bool idle_{false};
    Clock::time_point idle_since_{};
    Clock::time_point next_deadline_{Clock::time_point::max()};
    struct Reaper {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::vector<Reaper> reapers_;

public:
    // Accessors used by the session and dispatch code.
    std::map<uint64_t, std::unique_ptr<ServerSession>>& sessions() noexcept { return sessions_; }
    std::unordered_map<ConnId, std::unique_ptr<Conn>>& conns() noexcept { return conns_; }
    std::unordered_map<std::string, std::string>& blobs() noexcept { return blobs_; }
    size_t& blob_bytes() noexcept { return blob_bytes_; }
    uint64_t start_ms() const noexcept { return start_ms_; }
};

}  // namespace bromux::detail
