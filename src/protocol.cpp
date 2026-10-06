// Message bodies. Field order here *is* the protocol: append only (see protocol.h).
#include "bromux/protocol.h"

namespace bromux {

// ---- client -> server --------------------------------------------------------------

void HelloMsg::write(wire::Writer& w) const {
    w.u32(magic);
    w.u16(major);
    w.u16(minor);
    w.str(client_name);
    w.u32(flags);
}
void HelloMsg::read(wire::Reader& r) {
    magic = r.u32();
    major = r.u16();
    minor = r.u16();
    client_name = r.str();
    flags = r.u32();
}

void ListSessionsMsg::write(wire::Writer& w) const { w.u32(req); }
void ListSessionsMsg::read(wire::Reader& r) { req = r.u32(); }

void CreateSessionMsg::write(wire::Writer& w) const {
    w.u32(req);
    spec.write(w);
}
void CreateSessionMsg::read(wire::Reader& r) {
    req = r.u32();
    spec.read(r);
}

void AttachMsg::write(wire::Writer& w) const {
    w.u32(req);
    w.u64(session);
    w.u16(cols);
    w.u16(rows);
    w.u32(flags);
}
void AttachMsg::read(wire::Reader& r) {
    req = r.u32();
    session = r.u64();
    cols = r.u16();
    rows = r.u16();
    flags = r.u32();
}

void DetachMsg::write(wire::Writer& w) const { w.u64(session); }
void DetachMsg::read(wire::Reader& r) { session = r.u64(); }

void CloseSessionMsg::write(wire::Writer& w) const {
    w.u32(req);
    w.u64(session);
}
void CloseSessionMsg::read(wire::Reader& r) {
    req = r.u32();
    session = r.u64();
}

void ResizeMsg::write(wire::Writer& w) const {
    w.u64(session);
    w.u16(cols);
    w.u16(rows);
    w.u16(cell_width);
    w.u16(cell_height);
}
void ResizeMsg::read(wire::Reader& r) {
    session = r.u64();
    cols = r.u16();
    rows = r.u16();
    cell_width = r.u16();
    cell_height = r.u16();
}

void KeyMsg::write(wire::Writer& w) const {
    w.u64(session);
    write_key_event(w, event);
}
void KeyMsg::read(wire::Reader& r) {
    session = r.u64();
    read_key_event(r, event);
}

void TextMsg::write(wire::Writer& w) const {
    w.u64(session);
    w.str(text);
}
void TextMsg::read(wire::Reader& r) {
    session = r.u64();
    text = r.str();
}

void PasteMsg::write(wire::Writer& w) const {
    w.u64(session);
    w.str(text);
}
void PasteMsg::read(wire::Reader& r) {
    session = r.u64();
    text = r.str();
}

void RawInputMsg::write(wire::Writer& w) const {
    w.u64(session);
    w.str(bytes);
}
void RawInputMsg::read(wire::Reader& r) {
    session = r.u64();
    bytes = r.str();
}

void MouseMsg::write(wire::Writer& w) const {
    w.u64(session);
    write_mouse_event(w, event);
}
void MouseMsg::read(wire::Reader& r) {
    session = r.u64();
    read_mouse_event(r, event);
}

void FocusMsg::write(wire::Writer& w) const {
    w.u64(session);
    w.boolean(focused);
}
void FocusMsg::read(wire::Reader& r) {
    session = r.u64();
    focused = r.boolean();
}

void AckMsg::write(wire::Writer& w) const {
    w.u64(session);
    w.u64(frame_seq);
}
void AckMsg::read(wire::Reader& r) {
    session = r.u64();
    frame_seq = r.u64();
}

void FetchHistoryMsg::write(wire::Writer& w) const {
    w.u32(req);
    w.u64(session);
    w.u64(start);
    w.u32(count);
}
void FetchHistoryMsg::read(wire::Reader& r) {
    req = r.u32();
    session = r.u64();
    start = r.u64();
    count = r.u32();
}

void SetMetaMsg::write(wire::Writer& w) const {
    w.u64(session);
    w.str(key);
    w.str(value);
    w.boolean(erase);
}
void SetMetaMsg::read(wire::Reader& r) {
    session = r.u64();
    key = r.str();
    value = r.str();
    erase = r.boolean();
}

void SetPolicyMsg::write(wire::Writer& w) const {
    w.u64(session);
    w.u8(resize_policy);
    w.u8(clipboard_policy);
}
void SetPolicyMsg::read(wire::Reader& r) {
    session = r.u64();
    resize_policy = r.u8();
    clipboard_policy = r.u8();
    if (resize_policy != kPolicyUnchanged && resize_policy > uint8_t(ResizePolicy::Fixed)) r.fail();
    if (clipboard_policy != kPolicyUnchanged && clipboard_policy > uint8_t(ClipboardPolicy::ReadWrite)) r.fail();
}

void PutBlobMsg::write(wire::Writer& w) const {
    w.str(key);
    w.str(data);
}
void PutBlobMsg::read(wire::Reader& r) {
    key = r.str();
    data = r.str();
}

void GetBlobMsg::write(wire::Writer& w) const {
    w.u32(req);
    w.str(key);
}
void GetBlobMsg::read(wire::Reader& r) {
    req = r.u32();
    key = r.str();
}

void ClipboardDataMsg::write(wire::Writer& w) const {
    w.u64(session);
    w.u32(token);
    w.boolean(ok);
    w.str(data);
}
void ClipboardDataMsg::read(wire::Reader& r) {
    session = r.u64();
    token = r.u32();
    ok = r.boolean();
    data = r.str();
}

void SyncMsg::write(wire::Writer& w) const {
    w.u32(req);
    w.u64(session);
}
void SyncMsg::read(wire::Reader& r) {
    req = r.u32();
    session = r.u64();
}

void PingMsg::write(wire::Writer& w) const { w.u32(req); }
void PingMsg::read(wire::Reader& r) { req = r.u32(); }

void KillServerMsg::write(wire::Writer&) const {}
void KillServerMsg::read(wire::Reader&) {}

void FeedMsg::write(wire::Writer& w) const {
    w.u64(session);
    w.str(bytes);
}
void FeedMsg::read(wire::Reader& r) {
    session = r.u64();
    bytes = r.str();
}

// ---- server -> client --------------------------------------------------------------

void WelcomeMsg::write(wire::Writer& w) const {
    w.u32(magic);
    w.u16(major);
    w.u16(minor);
    w.str(server_version);
    w.u64(pid);
    w.u64(start_ms);
}
void WelcomeMsg::read(wire::Reader& r) {
    magic = r.u32();
    major = r.u16();
    minor = r.u16();
    server_version = r.str();
    pid = r.u64();
    start_ms = r.u64();
}

void ErrorMsg::write(wire::Writer& w) const {
    w.u32(req);
    w.u16(uint16_t(code));
    w.str(message);
}
void ErrorMsg::read(wire::Reader& r) {
    req = r.u32();
    code = ErrorCode(r.u16());
    message = r.str();
}

void OkMsg::write(wire::Writer& w) const { w.u32(req); }
void OkMsg::read(wire::Reader& r) { req = r.u32(); }

void SessionListMsg::write(wire::Writer& w) const {
    w.u32(req);
    w.varint(sessions.size());
    for (const SessionInfo& s : sessions) s.write(w);
}
void SessionListMsg::read(wire::Reader& r) {
    req = r.u32();
    sessions.resize(r.count(16));
    for (SessionInfo& s : sessions) s.read(r);
}

void SessionCreatedMsg::write(wire::Writer& w) const {
    w.u32(req);
    info.write(w);
}
void SessionCreatedMsg::read(wire::Reader& r) {
    req = r.u32();
    info.read(r);
}

void AttachedMsg::write(wire::Writer& w) const {
    w.u32(req);
    info.write(w);
}
void AttachedMsg::read(wire::Reader& r) {
    req = r.u32();
    info.read(r);
}

void DetachedMsg::write(wire::Writer& w) const {
    w.u64(session);
    w.u8(uint8_t(reason));
}
void DetachedMsg::read(wire::Reader& r) {
    session = r.u64();
    reason = DetachReason(r.u8());
}

void FrameMsg::write(wire::Writer& w) const {
    w.u64(session);
    w.u64(frame_seq);
    w.u64(feed_seq);
    w.str(ops);
}
void FrameMsg::read(wire::Reader& r) {
    session = r.u64();
    frame_seq = r.u64();
    feed_seq = r.u64();
    ops = r.str();
}

void EventMsg::write(wire::Writer& w) const {
    w.u64(session);
    w.u8(uint8_t(kind));
    w.svarint(x);
    w.svarint(y);
    w.str(a);
    w.str(b);
    w.str(c);
    w.str(d);
}
void EventMsg::read(wire::Reader& r) {
    session = r.u64();
    kind = EventKind(r.u8());
    x = r.svarint();
    y = r.svarint();
    a = r.str();
    b = r.str();
    // Minor 1's fields; an older server's event ends here.
    if (!r.at_end()) c = r.str();
    if (!r.at_end()) d = r.str();
}

void ClipboardRequestMsg::write(wire::Writer& w) const {
    w.u64(session);
    w.u32(token);
    w.str(selection);
}
void ClipboardRequestMsg::read(wire::Reader& r) {
    session = r.u64();
    token = r.u32();
    selection = r.str();
}

void HistoryMsg::write(wire::Writer& w) const {
    w.u32(req);
    w.u64(session);
    w.u64(feed_seq);
    w.u64(first_row);
    w.u64(history_rows);
    w.u64(epoch);
    w.u64(start);
    w.strings(rows);
}
void HistoryMsg::read(wire::Reader& r) {
    req = r.u32();
    session = r.u64();
    feed_seq = r.u64();
    first_row = r.u64();
    history_rows = r.u64();
    epoch = r.u64();
    start = r.u64();
    rows = r.strings();
}

void BlobMsg::write(wire::Writer& w) const {
    w.u32(req);
    w.boolean(found);
    w.str(data);
}
void BlobMsg::read(wire::Reader& r) {
    req = r.u32();
    found = r.boolean();
    data = r.str();
}

void SyncDoneMsg::write(wire::Writer& w) const {
    w.u32(req);
    w.u64(session);
    w.u64(feed_seq);
}
void SyncDoneMsg::read(wire::Reader& r) {
    req = r.u32();
    session = r.u64();
    feed_seq = r.u64();
}

void PongMsg::write(wire::Writer& w) const { w.u32(req); }
void PongMsg::read(wire::Reader& r) { req = r.u32(); }

void SessionNotifyMsg::write(wire::Writer& w) const {
    w.u8(uint8_t(kind));
    info.write(w);
}
void SessionNotifyMsg::read(wire::Reader& r) {
    uint8_t k = r.u8();
    if (k > uint8_t(NotifyKind::Removed)) r.fail();
    kind = NotifyKind(k);
    info.read(r);
}

const char* msg_type_name(uint16_t type) noexcept {
    switch (MsgType(type)) {
    case MsgType::Hello: return "Hello";
    case MsgType::ListSessions: return "ListSessions";
    case MsgType::CreateSession: return "CreateSession";
    case MsgType::Attach: return "Attach";
    case MsgType::Detach: return "Detach";
    case MsgType::CloseSession: return "CloseSession";
    case MsgType::Resize: return "Resize";
    case MsgType::Key: return "Key";
    case MsgType::Text: return "Text";
    case MsgType::Paste: return "Paste";
    case MsgType::Mouse: return "Mouse";
    case MsgType::Focus: return "Focus";
    case MsgType::RawInput: return "RawInput";
    case MsgType::Ack: return "Ack";
    case MsgType::FetchHistory: return "FetchHistory";
    case MsgType::SetMeta: return "SetMeta";
    case MsgType::SetPolicy: return "SetPolicy";
    case MsgType::PutBlob: return "PutBlob";
    case MsgType::GetBlob: return "GetBlob";
    case MsgType::ClipboardData: return "ClipboardData";
    case MsgType::Sync: return "Sync";
    case MsgType::Ping: return "Ping";
    case MsgType::KillServer: return "KillServer";
    case MsgType::Feed: return "Feed";
    case MsgType::Welcome: return "Welcome";
    case MsgType::Error: return "Error";
    case MsgType::Ok: return "Ok";
    case MsgType::SessionList: return "SessionList";
    case MsgType::SessionCreated: return "SessionCreated";
    case MsgType::Attached: return "Attached";
    case MsgType::Detached: return "Detached";
    case MsgType::Frame: return "Frame";
    case MsgType::Event: return "Event";
    case MsgType::ClipboardRequest: return "ClipboardRequest";
    case MsgType::History: return "History";
    case MsgType::Blob: return "Blob";
    case MsgType::SyncDone: return "SyncDone";
    case MsgType::Pong: return "Pong";
    case MsgType::SessionNotify: return "SessionNotify";
    }
    return "?";
}

const char* error_code_name(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::None: return "none";
    case ErrorCode::BadMessage: return "bad message";
    case ErrorCode::UnknownMessage: return "unknown message";
    case ErrorCode::VersionMismatch: return "protocol version mismatch";
    case ErrorCode::NoSuchSession: return "no such session";
    case ErrorCode::SpawnFailed: return "spawn failed";
    case ErrorCode::NotAttached: return "not attached";
    case ErrorCode::ReadOnly: return "read-only attachment";
    case ErrorCode::NotFound: return "not found";
    case ErrorCode::Internal: return "internal error";
    case ErrorCode::HelloRequired: return "hello required";
    }
    return "?";
}

}  // namespace bromux
