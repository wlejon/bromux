#pragma once
// Client: one connection to a bromux server, local or through `ssh host
// bromux proxy`, holding a ScreenModel per attached session.
//
// Threading. An internal thread reads the connection and queues messages;
// set_wakeup() lets an embedding UI hear about them (the hook runs on that
// thread: post a wakeup to the UI loop, nothing more). Everything else --
// dispatch(), the request helpers, screen() -- belongs to one thread, the
// one that owns the models. Frames are applied and acknowledged inside
// dispatch(), so a UI that stops dispatching stops being sent frames (the
// server's flow control), and resumes with the latest state.
//
// Requests that have an answer come as blocking helpers (list_sessions,
// create_session, attach, ...) with the connection timeout; while one waits,
// everything else that arrives is applied and kept for the next dispatch().

#include "bromux/protocol.h"
#include "bromux/screen_model.h"
#include "bromux/screen_source.h"
#include "bromux/stream.h"

#include <bropty/view.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace bromux {

struct ConnectOptions {
    std::string server_name;  // empty: the default server (paths.h)
    std::string address;      // explicit endpoint; overrides server_name
    // Start the server when none is running: `<server_exe> server --daemon`.
    bool autostart{true};
    // Empty: $BROMUX_EXE, else a `bromux` executable beside this program,
    // else `bromux` on PATH.
    std::string server_exe;
    std::vector<std::string> server_args;  // extra arguments for an autostarted server
    std::chrono::milliseconds timeout{10000};
    std::string client_name{"bromux-client"};
    bool watch_sessions{false};  // receive SessionNotify events
    // The protocol minor this client says it speaks (tests pose as an older one).
    uint16_t protocol_minor{kProtocolMinor};
};

struct SshTarget {
    std::string host;  // [user@]host, or an ssh config alias
    std::vector<std::string> ssh_args;  // e.g. {"-p", "2222", "-C"}
    std::string ssh_program{"ssh"};
    std::string remote_bromux{"bromux"};  // the bromux executable on the remote
    std::string server_name;              // remote server name (empty: default)
};

struct ClientEvent {
    enum class Kind : uint8_t {
        Frame,             // a session's ScreenModel changed: `effects`
        History,           // history rows its view() / source() asked for arrived: redraw
        Event,             // terminal event: `event`
        ClipboardRequest,  // the program asked for the clipboard: answer_clipboard(session, token, ...)
        Detached,          // `detach_reason`; the session's model is gone
        SessionNotify,     // `notify`, `info` (ConnectOptions::watch_sessions)
        Error,             // `code`, `text` (errors not tied to a helper call)
        Disconnected,      // the connection is gone; `text` says why
    };
    Kind kind{Kind::Event};
    uint64_t session{0};
    ScreenModel::Effects effects;
    EventMsg event;
    uint32_t token{0};
    DetachReason detach_reason{DetachReason::Requested};
    NotifyKind notify{NotifyKind::Changed};
    SessionInfo info;
    ErrorCode code{ErrorCode::None};
    std::string text;
};

// Connect to the local server named by `options`, starting it when it is
// not running (and options.autostart). No handshake: a raw stream.
std::unique_ptr<Stream> connect_or_start(const ConnectOptions& options, std::string* err = nullptr);

// The proxy end of remote attach (`bromux proxy`): relays this process's
// stdin / stdout to the local server until either side closes. Returns the
// process exit status.
int run_proxy(const ConnectOptions& options, std::string* err = nullptr);

class Client {
public:
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Local server, started on demand.
    static std::unique_ptr<Client> connect(const ConnectOptions& options, std::string* err = nullptr);
    // Remote server: runs `ssh [args] -T host <remote_bromux> proxy [-L name]`.
    // The remote proxy starts the remote server on demand.
    static std::unique_ptr<Client> connect_ssh(const SshTarget& target, const ConnectOptions& options,
                                               std::string* err = nullptr);
    // Any stream that reaches a server (or a proxy).
    static std::unique_ptr<Client> connect_stream(std::unique_ptr<Stream> stream, const ConnectOptions& options,
                                                  std::string* err = nullptr);

    [[nodiscard]] bool connected() const;
    [[nodiscard]] const WelcomeMsg& server_info() const noexcept { return welcome_; }
    // The protocol minor the server speaks: what it can send and accept
    // (protocol.h: minor 1 adds Feed, foreground, commands, images ...).
    [[nodiscard]] uint16_t server_minor() const noexcept { return welcome_.minor; }

    void set_wakeup(std::function<void()> fn);
    // Apply what has arrived; append the resulting events. Returns how many
    // events were appended.
    size_t dispatch(std::vector<ClientEvent>& out);
    // Wait until something arrives (or the connection ends). False on timeout.
    bool wait(std::chrono::milliseconds timeout);

    // The model of an attached session, or null.
    [[nodiscard]] ScreenModel* screen(uint64_t session);
    // The model with its history as a bropty::RowSource, and a bropty
    // TerminalView over it (viewport, selection, search, links, frames): made
    // on first use, kept up to date by dispatch(), fetching the history rows
    // they need (ClientEvent::History says some arrived). Null when the
    // session is not attached; both go with the model (detach, re-attach).
    [[nodiscard]] ScreenSource* source(uint64_t session);
    [[nodiscard]] bropty::TerminalView* view(uint64_t session);

    // ---- fire and forget ------------------------------------------------------------
    void send_key(uint64_t session, const bropty::KeyEvent& ev);
    void send_text(uint64_t session, std::string_view text);
    void paste(uint64_t session, std::string_view text);
    void send_mouse(uint64_t session, const bropty::MouseEvent& ev);
    void focus(uint64_t session, bool focused);
    void send_raw(uint64_t session, std::string_view bytes);
    // Bytes into the session's terminal as if its program wrote them
    // (FeedMsg). False, and nothing sent, when the server predates it.
    bool feed(uint64_t session, std::string_view bytes);
    void resize(uint64_t session, int cols, int rows, int cell_width = 0, int cell_height = 0);
    void detach(uint64_t session);
    void set_meta(uint64_t session, std::string_view key, std::string_view value);
    void erase_meta(uint64_t session, std::string_view key);
    void set_policy(uint64_t session, std::optional<ResizePolicy> resize, std::optional<ClipboardPolicy> clipboard);
    void put_blob(std::string_view key, std::string_view data);
    void answer_clipboard(uint64_t session, uint32_t token, bool ok, std::string_view data);
    void kill_server();

    // ---- request / reply (blocking, bounded by the connection timeout) ---------------
    std::optional<std::vector<SessionInfo>> list_sessions(std::string* err = nullptr);
    std::optional<SessionInfo> create_session(const SessionSpec& spec, std::string* err = nullptr);
    // Attach and wait for the session's first frame (the model is complete on return).
    std::optional<SessionInfo> attach(uint64_t session, int cols, int rows, uint32_t flags = 0,
                                      std::string* err = nullptr);
    bool close_session(uint64_t session, std::string* err = nullptr);
    // History rows from absolute row `start` (clamped to what the session
    // holds; ScreenModel::history_first_row()), at most `count`. view()
    // fetches what it shows by itself.
    std::optional<HistoryChunk> fetch_history(uint64_t session, uint64_t start, uint32_t count,
                                              std::string* err = nullptr);
    std::optional<std::string> get_blob(std::string_view key, std::string* err = nullptr);
    // Returns once the session's model shows the server's current state of it.
    bool sync(uint64_t session, std::string* err = nullptr);
    bool ping(std::string* err = nullptr);

private:
    Client();
    struct Reply {
        uint16_t type{0};
        std::string payload;
    };
    bool start(std::unique_ptr<Stream> stream, const ConnectOptions& options, std::string* err);
    void reader_main();
    template <class M>
    void send(const M& m) {
        send_bytes(encode(m));
    }
    void send_bytes(const std::string& bytes);
    uint32_t next_req() noexcept { return next_req_++; }
    bool await(uint32_t req, Reply& out, std::string* err);
    void process(uint16_t type, std::string_view payload, std::vector<ClientEvent>& out);
    void pump(std::vector<ClientEvent>& out);
    bool reply_error(const Reply& r, std::string* err);
    bool history_failed(uint32_t req);  // a ScreenSource's request was refused

    std::unique_ptr<Stream> stream_;
    std::thread reader_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::pair<uint16_t, std::string>> inbox_;
    bool eof_{false};
    std::string eof_reason_;
    std::function<void()> wakeup_;
    std::mutex write_mu_;
    bool write_failed_{false};

    // Owner-thread state.
    std::chrono::milliseconds timeout_{10000};
    WelcomeMsg welcome_;
    bool welcomed_{false};
    bool disconnect_reported_{false};
    uint32_t next_req_{1};
    std::set<uint32_t> waiting_;
    std::map<uint32_t, Reply> replies_;
    std::vector<ClientEvent> held_;  // events that arrived while a helper waited
    std::map<uint64_t, std::unique_ptr<ScreenModel>> screens_;
    // Per session, over its model (declared after screens_: destroyed first).
    struct Mirror {
        std::unique_ptr<ScreenSource> source;
        std::unique_ptr<bropty::TerminalView> view;  // observes source: destroyed first
    };
    std::map<uint64_t, Mirror> mirrors_;
    std::map<uint32_t, uint64_t> history_reqs_;  // a ScreenSource's FetchHistory -> session
};

}  // namespace bromux
