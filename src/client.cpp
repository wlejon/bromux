// Client: connection setup, the reader thread, dispatch.
#include "bromux/client.h"

#include "bromux/paths.h"

#include <cstdlib>
#include <filesystem>

namespace bromux {

namespace {

std::string default_server_exe() {
    if (const char* e = std::getenv("BROMUX_EXE"); e && *e) return e;
    std::error_code ec;
    std::filesystem::path self = current_executable();
#if defined(_WIN32)
    const char* exe_name = "bromux.exe";
#else
    const char* exe_name = "bromux";
#endif
    if (!self.empty()) {
        if (self.filename() == exe_name) return self.string();
        std::filesystem::path beside = self.parent_path() / exe_name;
        if (std::filesystem::exists(beside, ec)) return beside.string();
    }
    return "bromux";
}

}  // namespace

Client::Client() = default;

Client::~Client() {
    if (stream_) stream_->shutdown();
    if (reader_.joinable()) reader_.join();
}

std::unique_ptr<Client> Client::connect(const ConnectOptions& options, std::string* err) {
    std::unique_ptr<Stream> s = connect_or_start(options, err);
    if (!s) return nullptr;
    return connect_stream(std::move(s), options, err);
}

std::unique_ptr<Stream> connect_or_start(const ConnectOptions& options, std::string* err) {
    std::string address = options.address;
    if (address.empty()) {
        address = server_address(options.server_name, err);
        if (address.empty()) return nullptr;
    }
    std::string e;
    bool not_running = false;
    std::unique_ptr<Stream> s = connect_local(address, &e, &not_running);
    if (!s && not_running && options.autostart) {
        std::vector<std::string> argv{options.server_exe.empty() ? default_server_exe() : options.server_exe,
                                      "server", "--daemon"};
        if (!options.address.empty()) {
            argv.push_back("--socket");
            argv.push_back(options.address);
        } else if (!options.server_name.empty()) {
            argv.push_back("-L");
            argv.push_back(options.server_name);
        }
        argv.insert(argv.end(), options.server_args.begin(), options.server_args.end());
        std::string se;
        if (!spawn_detached(argv, &se)) {
            if (err) *err = "cannot start the server (" + argv[0] + "): " + se;
            return nullptr;
        }
        // Wait for it to listen. Two clients racing here both connect to
        // whichever server won the endpoint.
        const auto deadline = std::chrono::steady_clock::now() + options.timeout;
        auto delay = std::chrono::milliseconds(5);
        while (!s && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(delay);
            delay = std::min(delay * 2, std::chrono::milliseconds(100));
            s = connect_local(address, &e, &not_running);
            if (!s && !not_running) break;
        }
        if (!s && e.empty()) e = "the server did not come up in time";
    }
    if (!s && err) *err = e;
    return s;
}

std::unique_ptr<Client> Client::connect_ssh(const SshTarget& target, const ConnectOptions& options,
                                            std::string* err) {
    std::vector<std::string> argv{target.ssh_program};
    argv.insert(argv.end(), target.ssh_args.begin(), target.ssh_args.end());
    argv.push_back("-T");  // no remote pty: the channel carries the protocol verbatim
    argv.push_back(target.host);
    argv.push_back(target.remote_bromux);
    argv.push_back("proxy");
    if (!target.server_name.empty()) {
        argv.push_back("-L");
        argv.push_back(target.server_name);
    }
    std::unique_ptr<Stream> s = spawn_stream(argv, err);
    if (!s) return nullptr;
    return connect_stream(std::move(s), options, err);
}

std::unique_ptr<Client> Client::connect_stream(std::unique_ptr<Stream> stream, const ConnectOptions& options,
                                               std::string* err) {
    std::unique_ptr<Client> c(new Client());
    if (!c->start(std::move(stream), options, err)) return nullptr;
    return c;
}

bool Client::start(std::unique_ptr<Stream> stream, const ConnectOptions& options, std::string* err) {
    stream_ = std::move(stream);
    timeout_ = options.timeout;
    reader_ = std::thread([this] { reader_main(); });
    HelloMsg h;
    h.client_name = options.client_name;
    h.flags = options.watch_sessions ? uint32_t(Hello_WatchSessions) : 0u;
    h.minor = options.protocol_minor;
    send(h);
    const auto deadline = std::chrono::steady_clock::now() + timeout_;
    std::vector<ClientEvent> evs;
    while (!welcomed_) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            if (err) *err = "no answer from the server";
            return false;
        }
        wait(left);
        pump(evs);
        for (const ClientEvent& e : evs) {
            if (e.kind == ClientEvent::Kind::Error || e.kind == ClientEvent::Kind::Disconnected) {
                if (err) {
                    *err = e.text;
                    std::string diag = stream_->diagnostics();
                    if (!diag.empty()) *err += " (" + diag + ")";
                }
                return false;
            }
        }
        held_.insert(held_.end(), evs.begin(), evs.end());
        evs.clear();
    }
    return true;
}

void Client::reader_main() {
    wire::MessageSplitter splitter;
    std::vector<char> buf(64u << 10);
    std::string reason = "the server closed the connection";
    for (;;) {
        size_t n = stream_->read(buf.data(), buf.size());
        if (n == 0) break;
        splitter.feed(buf.data(), n);
        wire::MessageSplitter::Message m;
        bool any = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            while (splitter.next(m)) {
                inbox_.emplace_back(m.type, std::string(m.payload));
                any = true;
            }
        }
        if (splitter.error()) {
            reason = "malformed data from the server";
            break;
        }
        if (any) {
            cv_.notify_all();
            std::function<void()> wake;
            {
                std::lock_guard<std::mutex> lk(mu_);
                wake = wakeup_;
            }
            if (wake) wake();
        }
    }
    std::function<void()> wake;
    {
        std::lock_guard<std::mutex> lk(mu_);
        eof_ = true;
        std::string diag = stream_->diagnostics();
        eof_reason_ = diag.empty() ? reason : reason + ": " + diag;
        wake = wakeup_;
    }
    cv_.notify_all();
    if (wake) wake();
}

void Client::send_bytes(const std::string& bytes) {
    std::lock_guard<std::mutex> lk(write_mu_);
    if (write_failed_) return;
    if (!stream_->write(bytes)) {
        write_failed_ = true;
        stream_->shutdown();  // the reader notices and reports the disconnect
    }
}

bool Client::connected() const {
    std::lock_guard<std::mutex> lk(mu_);
    return !eof_;
}

void Client::set_wakeup(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(mu_);
    wakeup_ = std::move(fn);
}

bool Client::wait(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(mu_);
    const bool woken = cv_.wait_for(lk, timeout, [this] { return !inbox_.empty() || eof_ || poked_; });
    poked_ = false;
    return woken;
}

void Client::poke() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        poked_ = true;
    }
    cv_.notify_all();
}

ScreenModel* Client::screen(uint64_t session) {
    auto it = screens_.find(session);
    return it == screens_.end() ? nullptr : it->second.get();
}

ScreenSource* Client::source(uint64_t session) {
    auto mit = mirrors_.find(session);
    if (mit != mirrors_.end()) return mit->second.source.get();
    ScreenModel* m = screen(session);
    if (!m) return nullptr;
    // History requests go out without waiting; process() routes the answers.
    auto fetch = [this, session](int64_t start, uint32_t count) -> uint32_t {
        if (disconnect_reported_) return 0;
        FetchHistoryMsg f;
        f.req = next_req();
        f.session = session;
        f.start = uint64_t(start);
        f.count = count;
        history_reqs_[f.req] = session;
        send(f);
        return f.req;
    };
    Mirror& mr = mirrors_[session];
    mr.source = std::make_unique<ScreenSource>(*m, std::move(fetch));
    return mr.source.get();
}

bool Client::history_failed(uint32_t req) {
    auto hit = history_reqs_.find(req);
    if (hit == history_reqs_.end()) return false;
    auto mit = mirrors_.find(hit->second);
    if (mit != mirrors_.end()) mit->second.source->history_failed(req);
    history_reqs_.erase(hit);
    return true;
}

bropty::TerminalView* Client::view(uint64_t session) {
    ScreenSource* s = source(session);
    if (!s) return nullptr;
    Mirror& mr = mirrors_[session];
    if (!mr.view) mr.view = std::make_unique<bropty::TerminalView>(*s);
    return mr.view.get();
}

size_t Client::dispatch(std::vector<ClientEvent>& out) {
    const size_t before = out.size();
    out.insert(out.end(), std::make_move_iterator(held_.begin()), std::make_move_iterator(held_.end()));
    held_.clear();
    pump(out);
    return out.size() - before;
}

void Client::pump(std::vector<ClientEvent>& out) {
    std::deque<std::pair<uint16_t, std::string>> batch;
    bool eof = false;
    std::string reason;
    {
        std::lock_guard<std::mutex> lk(mu_);
        batch.swap(inbox_);
        eof = eof_;
        reason = eof_reason_;
    }
    for (auto& [type, payload] : batch) process(type, payload, out);
    if (eof && !disconnect_reported_) {
        disconnect_reported_ = true;
        ClientEvent e;
        e.kind = ClientEvent::Kind::Disconnected;
        e.text = reason;
        out.push_back(std::move(e));
    }
}

void Client::process(uint16_t type, std::string_view payload, std::vector<ClientEvent>& out) {
    auto malformed = [&] {
        ClientEvent e;
        e.kind = ClientEvent::Kind::Error;
        e.code = ErrorCode::BadMessage;
        e.text = std::string("malformed ") + msg_type_name(type) + " from the server";
        out.push_back(std::move(e));
    };
    // Replies to a waiting helper are stashed for it.
    auto stash = [&](uint32_t req) {
        if (!waiting_.count(req)) return false;
        replies_[req] = Reply{type, std::string(payload)};
        return true;
    };
    switch (MsgType(type)) {
    case MsgType::Welcome: {
        WelcomeMsg w;
        if (!decode(payload, w)) return malformed();
        welcome_ = std::move(w);
        welcomed_ = true;
        return;
    }
    case MsgType::Frame: {
        FrameMsg f;
        if (!decode(payload, f)) return malformed();
        ScreenModel* m = screen(f.session);
        if (!m) return;  // a frame that crossed a Detach
        ClientEvent e;
        e.kind = ClientEvent::Kind::Frame;
        e.session = f.session;
        std::string why;
        if (!m->apply(f, &why, &e.effects)) {
            ClientEvent err;
            err.kind = ClientEvent::Kind::Error;
            err.code = ErrorCode::BadMessage;
            err.session = f.session;
            err.text = "bad frame: " + why;
            out.push_back(std::move(err));
            return;
        }
        AckMsg ack;
        ack.session = f.session;
        ack.frame_seq = f.frame_seq;
        send(ack);
        auto mit = mirrors_.find(f.session);
        if (mit != mirrors_.end()) mit->second.source->frame_applied(e.effects);
        out.push_back(std::move(e));
        return;
    }
    case MsgType::Event: {
        ClientEvent e;
        e.kind = ClientEvent::Kind::Event;
        if (!decode(payload, e.event)) return malformed();
        e.session = e.event.session;
        out.push_back(std::move(e));
        return;
    }
    case MsgType::ClipboardRequest: {
        ClipboardRequestMsg m;
        if (!decode(payload, m)) return malformed();
        ClientEvent e;
        e.kind = ClientEvent::Kind::ClipboardRequest;
        e.session = m.session;
        e.token = m.token;
        e.text = std::move(m.selection);
        out.push_back(std::move(e));
        return;
    }
    case MsgType::Detached: {
        DetachedMsg m;
        if (!decode(payload, m)) return malformed();
        mirrors_.erase(m.session);
        screens_.erase(m.session);
        ClientEvent e;
        e.kind = ClientEvent::Kind::Detached;
        e.session = m.session;
        e.detach_reason = m.reason;
        out.push_back(std::move(e));
        return;
    }
    case MsgType::SessionNotify: {
        SessionNotifyMsg m;
        if (!decode(payload, m)) return malformed();
        ClientEvent e;
        e.kind = ClientEvent::Kind::SessionNotify;
        e.session = m.info.id;
        e.notify = m.kind;
        e.info = std::move(m.info);
        out.push_back(std::move(e));
        return;
    }
    case MsgType::Attached: {
        AttachedMsg m;
        if (!decode(payload, m)) return malformed();
        // A fresh model: the server starts this attachment with a full frame.
        mirrors_.erase(m.info.id);
        screens_[m.info.id] = std::make_unique<ScreenModel>();
        stash(m.req);
        return;
    }
    case MsgType::Error: {
        ErrorMsg m;
        if (!decode(payload, m)) return malformed();
        if (m.req && stash(m.req)) return;
        if (m.req && history_failed(m.req)) return;
        ClientEvent e;
        e.kind = ClientEvent::Kind::Error;
        e.code = m.code;
        e.text = m.message.empty() ? error_code_name(m.code) : m.message;
        out.push_back(std::move(e));
        return;
    }
    case MsgType::History: {
        wire::Reader r(payload);
        const uint32_t req = r.u32();
        if (!r.ok()) return malformed();
        auto hit = history_reqs_.find(req);
        if (hit == history_reqs_.end()) {
            stash(req);  // fetch_history's
            return;
        }
        const uint64_t session = hit->second;
        history_reqs_.erase(hit);
        auto mit = mirrors_.find(session);
        if (mit == mirrors_.end()) return;  // the source went (detached) meanwhile
        HistoryMsg h;
        HistoryChunk chunk;
        if (!decode(payload, h) || !decode_history(h, chunk)) {
            mit->second.source->history_failed(req);
            return malformed();
        }
        mit->second.source->history_arrived(req, chunk);
        ClientEvent e;
        e.kind = ClientEvent::Kind::History;
        e.session = session;
        out.push_back(std::move(e));
        return;
    }
    case MsgType::Ok:
    case MsgType::SessionList:
    case MsgType::SessionCreated:
    case MsgType::Blob:
    case MsgType::SyncDone:
    case MsgType::Pong: {
        // Every reply starts with its u32 request id.
        wire::Reader r(payload);
        uint32_t req = r.u32();
        if (!r.ok()) return malformed();
        stash(req);
        return;
    }
    default:
        return;  // a newer server's message: ignored (protocol.h)
    }
}

bool Client::await(uint32_t req, Reply& out, std::string* err) {
    waiting_.insert(req);
    const auto deadline = std::chrono::steady_clock::now() + timeout_;
    std::vector<ClientEvent> evs;
    for (;;) {
        pump(evs);
        held_.insert(held_.end(), std::make_move_iterator(evs.begin()), std::make_move_iterator(evs.end()));
        evs.clear();
        auto it = replies_.find(req);
        if (it != replies_.end()) {
            out = std::move(it->second);
            replies_.erase(it);
            waiting_.erase(req);
            return true;
        }
        if (disconnect_reported_) {
            if (err) *err = "disconnected from the server";
            waiting_.erase(req);
            return false;
        }
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            if (err) *err = "timed out waiting for the server";
            waiting_.erase(req);
            return false;
        }
        wait(left);
    }
}

bool Client::reply_error(const Reply& r, std::string* err) {
    if (MsgType(r.type) != MsgType::Error) return false;
    ErrorMsg m;
    if (decode(r.payload, m) && err) *err = m.message.empty() ? error_code_name(m.code) : m.message;
    return true;
}

}  // namespace bromux
