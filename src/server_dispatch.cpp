// Client messages.
#include "bromux/paths.h"
#include "server_impl.h"

#include <algorithm>

#ifndef BROMUX_VERSION_STRING
#define BROMUX_VERSION_STRING "0.0.0"
#endif

namespace bromux::detail {

namespace {
constexpr size_t kMaxBlobBytes = 64u << 20;
constexpr size_t kMaxRawInput = 1u << 20;
}  // namespace

void ServerCore::detach(Conn& c, uint64_t session, DetachReason reason, bool tell_client) {
    auto it = c.attachments.find(session);
    if (it == c.attachments.end()) return;
    std::unique_ptr<Attachment> a = std::move(it->second);
    c.attachments.erase(it);
    auto sit = sessions_.find(session);
    if (sit != sessions_.end()) {
        ServerSession& s = *sit->second;
        s.attachments.erase(std::remove(s.attachments.begin(), s.attachments.end(), a.get()), s.attachments.end());
        apply_resize_policy(s);
        notify(NotifyKind::Changed, s);
    }
    if (tell_client && !c.closing) {
        flush_events(*a);
        DetachedMsg d;
        d.session = session;
        d.reason = reason;
        send(c, d);
    }
}

void ServerCore::handle(Conn& c, uint16_t type, std::string_view payload) {
    const MsgType mt = MsgType(type);
    auto bad = [&] {
        send_error(c, 0, ErrorCode::BadMessage, std::string("malformed ") + msg_type_name(type));
        close_conn(c);
    };
    if (!c.hello && mt != MsgType::Hello) {
        send_error(c, 0, ErrorCode::HelloRequired, "the first message must be Hello");
        close_conn(c);
        return;
    }
    // The session a message names and this client's attachment to it.
    auto session_of = [&](uint64_t id) -> ServerSession* {
        auto it = sessions_.find(id);
        return it == sessions_.end() ? nullptr : it->second.get();
    };
    auto attachment_of = [&](uint64_t id) -> Attachment* {
        auto it = c.attachments.find(id);
        return it == c.attachments.end() ? nullptr : it->second.get();
    };
    // Input needs a writable attachment; marks the client most recent.
    auto input_target = [&](uint64_t id) -> ServerSession* {
        Attachment* a = attachment_of(id);
        if (!a) {
            send_error(c, 0, ErrorCode::NotAttached, "input for a session this client is not attached to");
            return nullptr;
        }
        if (a->read_only()) {
            send_error(c, 0, ErrorCode::ReadOnly, "input on a read-only attachment");
            return nullptr;
        }
        a->activity = next_activity();
        if (a->session->resize_policy == ResizePolicy::Latest) apply_resize_policy(*a->session);
        return a->session->pty ? a->session : nullptr;
    };

    switch (mt) {
    case MsgType::Hello: {
        HelloMsg m;
        if (!decode(payload, m)) return bad();
        if (m.magic != kProtocolMagic || m.major != kProtocolMajor) {
            send_error(c, 0, ErrorCode::VersionMismatch,
                       "server speaks protocol " + std::to_string(kProtocolMajor) + "." +
                           std::to_string(kProtocolMinor) + ", client " + std::to_string(m.major) + "." +
                           std::to_string(m.minor));
            close_conn(c);
            return;
        }
        c.hello = true;
        c.name = m.client_name;
        c.flags = m.flags;
        WelcomeMsg w;
        w.server_version = BROMUX_VERSION_STRING;
        w.pid = current_pid();
        w.start_ms = start_ms();
        send(c, w);
        return;
    }
    case MsgType::ListSessions: {
        ListSessionsMsg m;
        if (!decode(payload, m)) return bad();
        SessionListMsg r;
        r.req = m.req;
        for (auto& [id, s] : sessions_) r.sessions.push_back(s->info());
        send(c, r);
        return;
    }
    case MsgType::CreateSession: {
        CreateSessionMsg m;
        if (!decode(payload, m)) return bad();
        std::string err;
        ServerSession* s = create_session(m.spec, err);
        if (!s) {
            send_error(c, m.req, ErrorCode::SpawnFailed, err);
            return;
        }
        SessionCreatedMsg r;
        r.req = m.req;
        r.info = s->info();
        send(c, r);
        return;
    }
    case MsgType::Attach: {
        AttachMsg m;
        if (!decode(payload, m)) return bad();
        ServerSession* s = session_of(m.session);
        if (!s) {
            send_error(c, m.req, ErrorCode::NoSuchSession, "no session " + std::to_string(m.session));
            return;
        }
        // Attaching again starts over with a full frame.
        if (c.attachments.count(m.session)) detach(c, m.session, DetachReason::Requested, false);
        auto a = std::make_unique<Attachment>();
        a->conn = &c;
        a->session = s;
        a->flags = m.flags;
        a->cols = m.cols;
        a->rows = m.rows;
        a->activity = next_activity();
        s->attachments.push_back(a.get());
        c.attachments.emplace(m.session, std::move(a));
        apply_resize_policy(*s);
        AttachedMsg r;
        r.req = m.req;
        r.info = s->info();
        send(c, r);
        notify(NotifyKind::Changed, *s);
        if (!s->running) {
            // Tell a late client the program already ended.
            EventMsg e;
            e.session = s->id;
            e.kind = EventKind::Exited;
            e.x = s->exit_code;
            send(c, e);
        }
        return;
    }
    case MsgType::Detach: {
        DetachMsg m;
        if (!decode(payload, m)) return bad();
        if (!attachment_of(m.session)) {
            send_error(c, 0, ErrorCode::NotAttached, "not attached to " + std::to_string(m.session));
            return;
        }
        detach(c, m.session, DetachReason::Requested, true);
        return;
    }
    case MsgType::CloseSession: {
        CloseSessionMsg m;
        if (!decode(payload, m)) return bad();
        if (!session_of(m.session)) {
            send_error(c, m.req, ErrorCode::NoSuchSession, "no session " + std::to_string(m.session));
            return;
        }
        close_session(m.session);
        OkMsg ok;
        ok.req = m.req;
        send(c, ok);
        return;
    }
    case MsgType::Resize: {
        ResizeMsg m;
        if (!decode(payload, m)) return bad();
        Attachment* a = attachment_of(m.session);
        if (!a) {
            send_error(c, 0, ErrorCode::NotAttached, "resize for a session this client is not attached to");
            return;
        }
        a->cols = m.cols;
        a->rows = m.rows;
        a->activity = next_activity();
        if (m.cell_width && m.cell_height && !a->read_only() && a->session->pty)
            a->session->term->set_cell_pixel_size(m.cell_width, m.cell_height);
        apply_resize_policy(*a->session);
        return;
    }
    case MsgType::Key: {
        KeyMsg m;
        if (!decode(payload, m)) return bad();
        if (ServerSession* s = input_target(m.session)) s->term->send_key(m.event);
        return;
    }
    case MsgType::Text: {
        TextMsg m;
        if (!decode(payload, m)) return bad();
        if (ServerSession* s = input_target(m.session)) s->term->send_text(m.text);
        return;
    }
    case MsgType::Paste: {
        PasteMsg m;
        if (!decode(payload, m)) return bad();
        if (ServerSession* s = input_target(m.session)) s->term->paste(m.text);
        return;
    }
    case MsgType::Mouse: {
        MouseMsg m;
        if (!decode(payload, m)) return bad();
        if (ServerSession* s = input_target(m.session)) s->term->send_mouse(m.event);
        return;
    }
    case MsgType::Focus: {
        FocusMsg m;
        if (!decode(payload, m)) return bad();
        if (ServerSession* s = input_target(m.session)) s->term->focus(m.focused);
        return;
    }
    case MsgType::RawInput: {
        RawInputMsg m;
        if (!decode(payload, m) || m.bytes.size() > kMaxRawInput) return bad();
        // All or nothing, like a key: a child that stopped reading drops it.
        if (ServerSession* s = input_target(m.session))
            if (!s->term->input_blocked()) s->pty->write(m.bytes);
        return;
    }
    case MsgType::Ack: {
        AckMsg m;
        if (!decode(payload, m)) return bad();
        if (Attachment* a = attachment_of(m.session)) {
            while (!a->inflight.empty() && a->inflight.front().first <= m.frame_seq) {
                a->inflight_bytes -= a->inflight.front().second;
                a->inflight.pop_front();
            }
        }
        return;
    }
    case MsgType::FetchHistory: {
        FetchHistoryMsg m;
        if (!decode(payload, m)) return bad();
        send_history(c, m);
        return;
    }
    case MsgType::SetMeta: {
        SetMetaMsg m;
        if (!decode(payload, m)) return bad();
        ServerSession* s = session_of(m.session);
        if (!s) {
            send_error(c, 0, ErrorCode::NoSuchSession, "no session " + std::to_string(m.session));
            return;
        }
        auto it = std::find_if(s->meta.begin(), s->meta.end(), [&](const auto& p) { return p.first == m.key; });
        if (m.erase) {
            if (it != s->meta.end()) s->meta.erase(it);
        } else if (it != s->meta.end()) {
            it->second = m.value;
        } else {
            s->meta.emplace_back(m.key, m.value);
        }
        notify(NotifyKind::Changed, *s);
        return;
    }
    case MsgType::SetPolicy: {
        SetPolicyMsg m;
        if (!decode(payload, m)) return bad();
        ServerSession* s = session_of(m.session);
        if (!s) {
            send_error(c, 0, ErrorCode::NoSuchSession, "no session " + std::to_string(m.session));
            return;
        }
        if (m.resize_policy != kPolicyUnchanged) s->resize_policy = ResizePolicy(m.resize_policy);
        if (m.clipboard_policy != kPolicyUnchanged) s->clipboard_policy = ClipboardPolicy(m.clipboard_policy);
        apply_resize_policy(*s);
        notify(NotifyKind::Changed, *s);
        return;
    }
    case MsgType::PutBlob: {
        PutBlobMsg m;
        if (!decode(payload, m)) return bad();
        auto it = blobs_.find(m.key);
        if (it != blobs_.end()) {
            blob_bytes_ -= it->first.size() + it->second.size();
            blobs_.erase(it);
        }
        if (!m.data.empty()) {
            if (blob_bytes_ + m.key.size() + m.data.size() > kMaxBlobBytes) {
                send_error(c, 0, ErrorCode::Internal, "blob store full");
                return;
            }
            blob_bytes_ += m.key.size() + m.data.size();
            blobs_.emplace(std::move(m.key), std::move(m.data));
        }
        return;
    }
    case MsgType::GetBlob: {
        GetBlobMsg m;
        if (!decode(payload, m)) return bad();
        BlobMsg r;
        r.req = m.req;
        auto it = blobs_.find(m.key);
        if (it != blobs_.end()) {
            r.found = true;
            r.data = it->second;
        }
        send(c, r);
        return;
    }
    case MsgType::ClipboardData: {
        ClipboardDataMsg m;
        if (!decode(payload, m)) return bad();
        ServerSession* s = session_of(m.session);
        if (!s) return;
        auto& reqs = s->clip_requests;
        auto it = std::find_if(reqs.begin(), reqs.end(),
                               [&](const ServerSession::ClipRequest& r) { return r.token == m.token && r.conn == c.id; });
        if (it == reqs.end()) return;
        const std::string sel = it->selection;
        reqs.erase(it);
        if (m.ok && s->pty && s->clipboard_policy == ClipboardPolicy::ReadWrite)
            s->term->write_to_pty("\x1b]52;" + sel + ";" + base64_encode(m.data) + "\x1b\\");
        return;
    }
    case MsgType::Sync: {
        SyncMsg m;
        if (!decode(payload, m)) return bad();
        Attachment* a = attachment_of(m.session);
        if (!a) {
            send_error(c, m.req, ErrorCode::NotAttached, "sync for a session this client is not attached to");
            return;
        }
        a->force_frame = true;
        a->sync_reqs.push_back(m.req);
        return;
    }
    case MsgType::Ping: {
        PingMsg m;
        if (!decode(payload, m)) return bad();
        PongMsg r;
        r.req = m.req;
        send(c, r);
        return;
    }
    case MsgType::KillServer:
        log("kill-server requested by " + c.name);
        request_stop();
        return;
    default:
        send_error(c, 0, ErrorCode::UnknownMessage, "unknown message type " + std::to_string(type));
        return;
    }
}

}  // namespace bromux::detail
