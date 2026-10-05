#pragma once
// The bromux wire protocol: message types and their bodies.
//
// Versioning. The client opens with Hello (magic, major, minor); the server
// answers Welcome or Error(VersionMismatch) and closes. Peers with the same
// major interoperate: a newer minor only ever appends fields to the end of a
// body (decoders ignore trailing bytes) or adds message types (a server
// answers an unknown type with Error(UnknownMessage); a client ignores it).
// Anything else bumps the major. docs/protocol.md documents every body.

#include "bromux/wire.h"

#include <bropty/input.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bromux {

inline constexpr uint32_t kProtocolMagic = 0x584D5242;  // "BRMX" little endian
inline constexpr uint16_t kProtocolMajor = 1;
inline constexpr uint16_t kProtocolMinor = 0;

enum class MsgType : uint16_t {
    // client -> server
    Hello = 0x0101,
    ListSessions = 0x0102,
    CreateSession = 0x0103,
    Attach = 0x0104,
    Detach = 0x0105,
    CloseSession = 0x0106,
    Resize = 0x0107,
    Key = 0x0108,
    Text = 0x0109,
    Paste = 0x010A,
    Mouse = 0x010B,
    Focus = 0x010C,
    RawInput = 0x010D,
    Ack = 0x010E,
    FetchHistory = 0x010F,
    SetMeta = 0x0110,
    SetPolicy = 0x0111,
    PutBlob = 0x0112,
    GetBlob = 0x0113,
    ClipboardData = 0x0114,
    Sync = 0x0115,
    Ping = 0x0116,
    KillServer = 0x0117,
    // server -> client
    Welcome = 0x0201,
    Error = 0x0202,
    Ok = 0x0203,
    SessionList = 0x0204,
    SessionCreated = 0x0205,
    Attached = 0x0206,
    Detached = 0x0207,
    Frame = 0x0208,
    Event = 0x0209,
    ClipboardRequest = 0x020A,
    History = 0x020B,
    Blob = 0x020C,
    SyncDone = 0x020D,
    Pong = 0x020E,
    SessionNotify = 0x020F,
};

enum class ErrorCode : uint16_t {
    None = 0,
    BadMessage = 1,       // body did not decode; the connection is closed
    UnknownMessage = 2,   // a type this server does not know
    VersionMismatch = 3,  // Hello with another major; the connection is closed
    NoSuchSession = 4,
    SpawnFailed = 5,
    NotAttached = 6,
    ReadOnly = 7,         // input on a read-only attachment
    NotFound = 8,         // blob key
    Internal = 9,
    HelloRequired = 10,
};

// How a session's size follows the clients attached to it.
enum class ResizePolicy : uint8_t {
    Latest = 0,    // the most recently active client (input, resize or attach) decides
    Smallest = 1,  // min(cols), min(rows) over the attached clients: everyone sees all of it
    Largest = 2,   // max(cols), max(rows)
    Fixed = 3,     // clients never resize it
};

// What OSC 52 may do.
enum class ClipboardPolicy : uint8_t {
    Deny = 0,       // neither set nor query reaches a client
    WriteOnly = 1,  // set is forwarded as an event; queries are refused
    ReadWrite = 2,  // queries are asked of the most recently active client
};

inline constexpr uint8_t kPolicyUnchanged = 0xFF;

enum AttachFlags : uint32_t {
    Attach_ReadOnly = 1u << 0,  // no input; never influences the size
    Attach_NoResize = 1u << 1,  // sends input, but its size never counts
};

enum HelloFlags : uint32_t {
    Hello_WatchSessions = 1u << 0,  // receive SessionNotify for every session change
};

enum class DetachReason : uint8_t {
    Requested = 0,
    SessionClosed = 1,
    ServerShutdown = 2,
};

enum class NotifyKind : uint8_t { Added = 0, Changed = 1, Removed = 2 };

// Events forwarded from a session's terminal. Fields used per kind:
//   Bell                         -
//   Title / IconName / Cwd       a = the new value
//   Notification                 a = title, b = body
//   Progress                     x = state, y = value
//   SemanticMark (OSC 133)       x = kind ('A' 'B' 'C' 'D' ...), a = params
//   ClipboardWrite (OSC 52)      a = selection, b = decoded data
//   Apc                          a = payload
//   ResizedByApp (DECCOLM)       x = cols, y = rows
//   Exited                       x = exit code (-1 unknown)
//   EventsDropped                x = how many events a full queue dropped
enum class EventKind : uint8_t {
    Bell = 1,
    Title = 2,
    IconName = 3,
    Cwd = 4,
    Notification = 5,
    Progress = 6,
    SemanticMark = 7,
    ClipboardWrite = 8,
    Apc = 9,
    ResizedByApp = 10,
    Exited = 11,
    EventsDropped = 12,
};

using Pairs = std::vector<std::pair<std::string, std::string>>;

struct SessionSpec {
    Pairs meta;  // free-form; "name" is the display name by convention
    std::string command;  // empty: the user's shell
    std::vector<std::string> args;
    std::string windows_command_line;  // Windows: exact command line, overrides command/args
    std::string cwd;
    bool inherit_env{true};
    Pairs env;
    std::vector<std::string> env_unset;
    uint16_t cols{80};
    uint16_t rows{24};
    uint32_t scrollback_rows{10000};
    bool remove_on_exit{false};  // close the session as soon as its program exits
    ResizePolicy resize_policy{ResizePolicy::Latest};

    void write(wire::Writer& w) const;
    void read(wire::Reader& r);
};

struct SessionInfo {
    uint64_t id{0};
    Pairs meta;
    std::string command;
    int64_t pid{0};
    bool running{false};
    int32_t exit_code{-1};
    uint16_t cols{0};
    uint16_t rows{0};
    uint32_t clients{0};
    uint64_t created_ms{0};  // unix epoch milliseconds
    std::string title;
    std::string cwd;
    ResizePolicy resize_policy{ResizePolicy::Latest};
    ClipboardPolicy clipboard_policy{ClipboardPolicy::WriteOnly};

    [[nodiscard]] std::string meta_value(std::string_view key) const;
    void write(wire::Writer& w) const;
    void read(wire::Reader& r);
};

void write_key_event(wire::Writer& w, const bropty::KeyEvent& ev);
bool read_key_event(wire::Reader& r, bropty::KeyEvent& ev);
void write_mouse_event(wire::Writer& w, const bropty::MouseEvent& ev);
bool read_mouse_event(wire::Reader& r, bropty::MouseEvent& ev);

// ---- messages ------------------------------------------------------------------
// Each has kType, write() and read(); encode() / decode() below frame them.

#define BROMUX_MSG(Name)                         \
    static constexpr MsgType kType = MsgType::Name; \
    void write(wire::Writer& w) const;           \
    void read(wire::Reader& r);

// client -> server
struct HelloMsg {
    uint32_t magic{kProtocolMagic};
    uint16_t major{kProtocolMajor};
    uint16_t minor{kProtocolMinor};
    std::string client_name;
    uint32_t flags{0};
    BROMUX_MSG(Hello)
};
struct ListSessionsMsg {
    uint32_t req{0};
    BROMUX_MSG(ListSessions)
};
struct CreateSessionMsg {
    uint32_t req{0};
    SessionSpec spec;
    BROMUX_MSG(CreateSession)
};
struct AttachMsg {
    uint32_t req{0};
    uint64_t session{0};
    uint16_t cols{0};  // 0: the client has no size yet (does not count)
    uint16_t rows{0};
    uint32_t flags{0};
    BROMUX_MSG(Attach)
};
struct DetachMsg {
    uint64_t session{0};
    BROMUX_MSG(Detach)
};
struct CloseSessionMsg {
    uint32_t req{0};
    uint64_t session{0};
    BROMUX_MSG(CloseSession)
};
struct ResizeMsg {
    uint64_t session{0};
    uint16_t cols{0};
    uint16_t rows{0};
    uint16_t cell_width{0};  // pixels, 0 = unknown
    uint16_t cell_height{0};
    BROMUX_MSG(Resize)
};
struct KeyMsg {
    uint64_t session{0};
    bropty::KeyEvent event;
    BROMUX_MSG(Key)
};
struct TextMsg {
    uint64_t session{0};
    std::string text;
    BROMUX_MSG(Text)
};
struct PasteMsg {
    uint64_t session{0};
    std::string text;
    BROMUX_MSG(Paste)
};
struct RawInputMsg {
    uint64_t session{0};
    std::string bytes;  // written to the PTY as they are
    BROMUX_MSG(RawInput)
};
struct MouseMsg {
    uint64_t session{0};
    bropty::MouseEvent event;
    BROMUX_MSG(Mouse)
};
struct FocusMsg {
    uint64_t session{0};
    bool focused{false};
    BROMUX_MSG(Focus)
};
struct AckMsg {
    uint64_t session{0};
    uint64_t frame_seq{0};
    BROMUX_MSG(Ack)
};
struct FetchHistoryMsg {
    uint32_t req{0};
    uint64_t session{0};
    uint64_t start{0};  // history row index, 0 = oldest held
    uint32_t count{0};
    BROMUX_MSG(FetchHistory)
};
struct SetMetaMsg {
    uint64_t session{0};
    std::string key;
    std::string value;
    bool erase{false};
    BROMUX_MSG(SetMeta)
};
struct SetPolicyMsg {
    uint64_t session{0};
    uint8_t resize_policy{kPolicyUnchanged};
    uint8_t clipboard_policy{kPolicyUnchanged};
    BROMUX_MSG(SetPolicy)
};
struct PutBlobMsg {
    std::string key;
    std::string data;  // empty erases
    BROMUX_MSG(PutBlob)
};
struct GetBlobMsg {
    uint32_t req{0};
    std::string key;
    BROMUX_MSG(GetBlob)
};
struct ClipboardDataMsg {
    uint64_t session{0};
    uint32_t token{0};
    bool ok{false};  // false: the client refuses to answer
    std::string data;
    BROMUX_MSG(ClipboardData)
};
struct SyncMsg {
    uint32_t req{0};
    uint64_t session{0};
    BROMUX_MSG(Sync)
};
struct PingMsg {
    uint32_t req{0};
    BROMUX_MSG(Ping)
};
struct KillServerMsg {
    BROMUX_MSG(KillServer)
};

// server -> client
struct WelcomeMsg {
    uint32_t magic{kProtocolMagic};
    uint16_t major{kProtocolMajor};
    uint16_t minor{kProtocolMinor};
    std::string server_version;
    uint64_t pid{0};
    uint64_t start_ms{0};
    BROMUX_MSG(Welcome)
};
struct ErrorMsg {
    uint32_t req{0};
    ErrorCode code{ErrorCode::None};
    std::string message;
    BROMUX_MSG(Error)
};
struct OkMsg {
    uint32_t req{0};
    BROMUX_MSG(Ok)
};
struct SessionListMsg {
    uint32_t req{0};
    std::vector<SessionInfo> sessions;
    BROMUX_MSG(SessionList)
};
struct SessionCreatedMsg {
    uint32_t req{0};
    SessionInfo info;
    BROMUX_MSG(SessionCreated)
};
struct AttachedMsg {
    uint32_t req{0};
    SessionInfo info;
    BROMUX_MSG(Attached)
};
struct DetachedMsg {
    uint64_t session{0};
    DetachReason reason{DetachReason::Requested};
    BROMUX_MSG(Detached)
};
// A screen update. `ops` is the op stream (row_codec.h / screen_model.h).
struct FrameMsg {
    uint64_t session{0};
    uint64_t frame_seq{0};  // per attachment, from 1; acknowledge with Ack
    uint64_t feed_seq{0};   // the session's state version this frame shows
    std::string ops;
    BROMUX_MSG(Frame)
};
struct EventMsg {
    uint64_t session{0};
    EventKind kind{EventKind::Bell};
    int64_t x{0};
    int64_t y{0};
    std::string a;
    std::string b;
    BROMUX_MSG(Event)
};
struct ClipboardRequestMsg {
    uint64_t session{0};
    uint32_t token{0};
    std::string selection;
    BROMUX_MSG(ClipboardRequest)
};
struct HistoryMsg {
    uint32_t req{0};
    uint64_t session{0};
    uint64_t feed_seq{0};      // state version the rows were read at
    uint64_t history_rows{0};  // rows held at that moment
    uint64_t start{0};         // index of rows[0]
    std::vector<std::string> rows;  // row encodings (row_codec.h)
    BROMUX_MSG(History)
};
struct BlobMsg {
    uint32_t req{0};
    bool found{false};
    std::string data;
    BROMUX_MSG(Blob)
};
struct SyncDoneMsg {
    uint32_t req{0};
    uint64_t session{0};
    uint64_t feed_seq{0};
    BROMUX_MSG(SyncDone)
};
struct PongMsg {
    uint32_t req{0};
    BROMUX_MSG(Pong)
};
struct SessionNotifyMsg {
    NotifyKind kind{NotifyKind::Added};
    SessionInfo info;
    BROMUX_MSG(SessionNotify)
};

#undef BROMUX_MSG

// A complete framed message (length + type + body).
template <class M>
[[nodiscard]] std::string encode(const M& m) {
    wire::Writer w;
    m.write(w);
    return wire::make_message(uint16_t(M::kType), w.data());
}
template <class M>
void encode_into(std::string& out, const M& m) {
    wire::Writer w;
    m.write(w);
    wire::frame_message(out, uint16_t(M::kType), w.data());
}
// Decode a body. Trailing bytes (fields of a newer minor version) are ignored.
template <class M>
[[nodiscard]] bool decode(std::string_view payload, M& m) {
    wire::Reader r(payload);
    m.read(r);
    return r.ok();
}

[[nodiscard]] const char* msg_type_name(uint16_t type) noexcept;
[[nodiscard]] const char* error_code_name(ErrorCode code) noexcept;

}  // namespace bromux
