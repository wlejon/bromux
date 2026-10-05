// Client: requests (fire-and-forget and blocking request / reply helpers).
#include "bromux/client.h"

namespace bromux {

void Client::send_key(uint64_t session, const bropty::KeyEvent& ev) {
    KeyMsg m;
    m.session = session;
    m.event = ev;
    send(m);
}

void Client::send_text(uint64_t session, std::string_view text) {
    TextMsg m;
    m.session = session;
    m.text = std::string(text);
    send(m);
}

void Client::paste(uint64_t session, std::string_view text) {
    PasteMsg m;
    m.session = session;
    m.text = std::string(text);
    send(m);
}

void Client::send_mouse(uint64_t session, const bropty::MouseEvent& ev) {
    MouseMsg m;
    m.session = session;
    m.event = ev;
    send(m);
}

void Client::focus(uint64_t session, bool focused) {
    FocusMsg m;
    m.session = session;
    m.focused = focused;
    send(m);
}

void Client::send_raw(uint64_t session, std::string_view bytes) {
    RawInputMsg m;
    m.session = session;
    m.bytes = std::string(bytes);
    send(m);
}

void Client::resize(uint64_t session, int cols, int rows, int cell_width, int cell_height) {
    auto clamp16 = [](int v) { return uint16_t(v < 0 ? 0 : v > 0xFFFF ? 0xFFFF : v); };
    ResizeMsg m;
    m.session = session;
    m.cols = clamp16(cols);
    m.rows = clamp16(rows);
    m.cell_width = clamp16(cell_width);
    m.cell_height = clamp16(cell_height);
    send(m);
}

void Client::detach(uint64_t session) {
    DetachMsg m;
    m.session = session;
    send(m);
}

void Client::set_meta(uint64_t session, std::string_view key, std::string_view value) {
    SetMetaMsg m;
    m.session = session;
    m.key = std::string(key);
    m.value = std::string(value);
    send(m);
}

void Client::erase_meta(uint64_t session, std::string_view key) {
    SetMetaMsg m;
    m.session = session;
    m.key = std::string(key);
    m.erase = true;
    send(m);
}

void Client::set_policy(uint64_t session, std::optional<ResizePolicy> resize, std::optional<ClipboardPolicy> clipboard) {
    SetPolicyMsg m;
    m.session = session;
    if (resize) m.resize_policy = uint8_t(*resize);
    if (clipboard) m.clipboard_policy = uint8_t(*clipboard);
    send(m);
}

void Client::put_blob(std::string_view key, std::string_view data) {
    PutBlobMsg m;
    m.key = std::string(key);
    m.data = std::string(data);
    send(m);
}

void Client::answer_clipboard(uint64_t session, uint32_t token, bool ok, std::string_view data) {
    ClipboardDataMsg m;
    m.session = session;
    m.token = token;
    m.ok = ok;
    m.data = std::string(data);
    send(m);
}

void Client::kill_server() { send(KillServerMsg{}); }

std::optional<std::vector<SessionInfo>> Client::list_sessions(std::string* err) {
    ListSessionsMsg m;
    m.req = next_req();
    send(m);
    Reply r;
    if (!await(m.req, r, err) || reply_error(r, err)) return std::nullopt;
    SessionListMsg l;
    if (MsgType(r.type) != MsgType::SessionList || !decode(r.payload, l)) {
        if (err) *err = "unexpected reply";
        return std::nullopt;
    }
    return std::move(l.sessions);
}

std::optional<SessionInfo> Client::create_session(const SessionSpec& spec, std::string* err) {
    CreateSessionMsg m;
    m.req = next_req();
    m.spec = spec;
    send(m);
    Reply r;
    if (!await(m.req, r, err) || reply_error(r, err)) return std::nullopt;
    SessionCreatedMsg c;
    if (MsgType(r.type) != MsgType::SessionCreated || !decode(r.payload, c)) {
        if (err) *err = "unexpected reply";
        return std::nullopt;
    }
    return std::move(c.info);
}

std::optional<SessionInfo> Client::attach(uint64_t session, int cols, int rows, uint32_t flags, std::string* err) {
    AttachMsg m;
    m.req = next_req();
    m.session = session;
    m.cols = uint16_t(cols < 0 ? 0 : cols > 0xFFFF ? 0xFFFF : cols);
    m.rows = uint16_t(rows < 0 ? 0 : rows > 0xFFFF ? 0xFFFF : rows);
    m.flags = flags;
    send(m);
    Reply r;
    if (!await(m.req, r, err) || reply_error(r, err)) return std::nullopt;
    AttachedMsg a;
    if (MsgType(r.type) != MsgType::Attached || !decode(r.payload, a)) {
        if (err) *err = "unexpected reply";
        return std::nullopt;
    }
    if (!sync(session, err)) return std::nullopt;
    return std::move(a.info);
}

bool Client::close_session(uint64_t session, std::string* err) {
    CloseSessionMsg m;
    m.req = next_req();
    m.session = session;
    send(m);
    Reply r;
    if (!await(m.req, r, err) || reply_error(r, err)) return false;
    return MsgType(r.type) == MsgType::Ok;
}

std::optional<HistoryChunk> Client::fetch_history(uint64_t session, uint64_t start, uint32_t count, std::string* err) {
    FetchHistoryMsg m;
    m.req = next_req();
    m.session = session;
    m.start = start;
    m.count = count;
    send(m);
    Reply r;
    if (!await(m.req, r, err) || reply_error(r, err)) return std::nullopt;
    HistoryMsg h;
    if (MsgType(r.type) != MsgType::History || !decode(r.payload, h)) {
        if (err) *err = "unexpected reply";
        return std::nullopt;
    }
    HistoryChunk out;
    out.feed_seq = h.feed_seq;
    out.history_rows = h.history_rows;
    out.start = h.start;
    out.rows.resize(h.rows.size());
    for (size_t i = 0; i < h.rows.size(); ++i) {
        wire::Reader rr(h.rows[i]);
        if (!decode_row(rr, out.rows[i], out.styles)) {
            if (err) *err = "bad history row";
            return std::nullopt;
        }
    }
    return out;
}

std::optional<std::string> Client::get_blob(std::string_view key, std::string* err) {
    GetBlobMsg m;
    m.req = next_req();
    m.key = std::string(key);
    send(m);
    Reply r;
    if (!await(m.req, r, err) || reply_error(r, err)) return std::nullopt;
    BlobMsg b;
    if (MsgType(r.type) != MsgType::Blob || !decode(r.payload, b)) {
        if (err) *err = "unexpected reply";
        return std::nullopt;
    }
    if (!b.found) {
        if (err) *err = "no such blob";
        return std::nullopt;
    }
    return std::move(b.data);
}

bool Client::sync(uint64_t session, std::string* err) {
    SyncMsg m;
    m.req = next_req();
    m.session = session;
    send(m);
    Reply r;
    if (!await(m.req, r, err) || reply_error(r, err)) return false;
    return MsgType(r.type) == MsgType::SyncDone;
}

bool Client::ping(std::string* err) {
    PingMsg m;
    m.req = next_req();
    send(m);
    Reply r;
    if (!await(m.req, r, err) || reply_error(r, err)) return false;
    return MsgType(r.type) == MsgType::Pong;
}

}  // namespace bromux
