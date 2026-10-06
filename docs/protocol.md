# bromux wire protocol, version 2.1

Version 2 numbers scrollback rows absolutely (`Op_History`, `FetchHistory`, `History`); version 1 indexed them from the oldest row held, an index that shifted whenever rows were evicted.

Minor 1 (2.1) carries what a local terminal has beyond the screen: `Feed` (bytes into a session's terminal as if its program wrote them), the `Foreground` event (the process that owns the terminal), the notification event's OSC 99 id, urgency and source, and frame ops for the OSC 22 pointer shape, OSC 133 command records and inline images. A server sends a connection only what the minor in its `Hello` knows: a 2.0 client gets 2.0 frames and events, and never sees the new ops, event kinds or messages. A 2.1 client of a 2.0 server reads `Welcome.minor` and does not send `Feed`.

A client and a bromux server talk over one byte stream. Locally that is a Unix socket or a Windows named pipe. Remotely it is the stdio of `ssh host bromux proxy`, which relays bytes to the remote host's local server without reading them. The protocol is the same in every case, and nothing in it depends on the transport.

The protocol is hand-rolled and length-prefixed. Every field is bounds-checked when read, and a peer never trusts a length it receives. The code that implements it lives in `include/bromux/wire.h` (primitives), `include/bromux/protocol.h` (messages) and `include/bromux/codec.h` (screen encoding). This document and those headers describe the same format.

## Framing

```
message := u32 length   -- little endian; the bytes that follow (type + body), >= 2
           u16 type     -- little endian; MsgType
           body         -- type-specific
```

A length below 2, or above 32 MiB (`kMaxMessage`), is a framing error. The stream cannot be resynchronised after one, so it is closed.

## Primitives

| Name | Encoding |
|------|----------|
| `u8` `u16` `u32` `u64` | fixed width, little endian |
| `bool` | `u8`: 0 or 1 |
| `varint` | unsigned LEB128, at most 10 bytes |
| `svarint` | zigzag (`(v << 1) ^ (v >> 63)`), then varint |
| `str` | varint length, then that many bytes (UTF-8 for text; arbitrary for data) |
| `strings` | varint count, then `str` × count |
| `pairs` | varint count, then (`str` key, `str` value) × count |

Readers reject a count that the remaining bytes could not possibly hold, so a hostile count never causes an allocation.

## Versioning

- The client's first message must be `Hello`. Any other message first gets `Error(HelloRequired)`.
- `Hello` carries magic `0x584D5242` ("BRMX" in little-endian byte order), `major` and `minor`. If the major differs, the server answers `Error(VersionMismatch)` and closes the connection. Otherwise it answers `Welcome`.
- **Minor versions only add.** A newer minor may append fields to the end of a body; every decoder ignores trailing bytes. A newer minor may also add message types:
  - A server answers an unknown type with `Error(UnknownMessage)` and stays connected.
  - A client ignores unknown types.
- Any other change bumps the major.

## Requests and replies

Messages that expect a reply carry a client-chosen `u32 req`, and the reply echoes it. A failure is `Error{req, code, message}` in place of the normal reply. Events, frames and notifications arrive between replies, in any order relative to them.

## Messages: client to server (0x01xx)

| Type | Name | Body | Reply |
|------|------|------|-------|
| 0x0101 | Hello | `u32 magic, u16 major, u16 minor, str client_name, u32 flags` (1 = watch sessions) | Welcome |
| 0x0102 | ListSessions | `u32 req` | SessionList |
| 0x0103 | CreateSession | `u32 req, SessionSpec` | SessionCreated |
| 0x0104 | Attach | `u32 req, u64 session, u16 cols, u16 rows, u32 flags` (1 read-only, 2 no-resize); cols/rows 0 = no size yet | Attached, then a full Frame |
| 0x0105 | Detach | `u64 session` | Detached(Requested) |
| 0x0106 | CloseSession | `u32 req, u64 session` | Ok; Detached(SessionClosed) to every attached client |
| 0x0107 | Resize | `u64 session, u16 cols, u16 rows, u16 cell_width, u16 cell_height` (pixels, 0 unknown) | - |
| 0x0108 | Key | `u64 session, KeyEvent` | - |
| 0x0109 | Text | `u64 session, str text` (typed text) | - |
| 0x010A | Paste | `u64 session, str text` (bracketed when the program enabled it) | - |
| 0x010B | Mouse | `u64 session, MouseEvent` | - |
| 0x010C | Focus | `u64 session, bool focused` | - |
| 0x010D | RawInput | `u64 session, str bytes` (written to the PTY unchanged) | - |
| 0x010E | Ack | `u64 session, u64 frame_seq` | - |
| 0x010F | FetchHistory | `u32 req, u64 session, u64 start, u32 count` (`start`: absolute row number) | History |
| 0x0110 | SetMeta | `u64 session, str key, str value, bool erase` | - (SessionNotify to watchers) |
| 0x0111 | SetPolicy | `u64 session, u8 resize_policy, u8 clipboard_policy` (0xFF = unchanged) | - |
| 0x0112 | PutBlob | `str key, str data` (empty data erases) | - |
| 0x0113 | GetBlob | `u32 req, str key` | Blob |
| 0x0114 | ClipboardData | `u64 session, u32 token, bool ok, str data` | - |
| 0x0115 | Sync | `u32 req, u64 session` | a Frame showing the current state, then SyncDone |
| 0x0116 | Ping | `u32 req` | Pong |
| 0x0117 | KillServer | (empty) | the server shuts down |
| 0x0118 | Feed (2.1) | `u64 session, str bytes` (at most 16 MiB): into the session's terminal as if its program had written them; nothing reaches the program. Recorded by the tee like PTY output. | - |

Input messages (Key through RawInput, and Feed) on a read-only attachment get `Error(ReadOnly)`. Input for a session the connection has not attached gets `Error(NotAttached)`.

## Messages: server to client (0x02xx)

| Type | Name | Body |
|------|------|------|
| 0x0201 | Welcome | `u32 magic, u16 major, u16 minor, str server_version, u64 pid, u64 start_ms` |
| 0x0202 | Error | `u32 req, u16 code, str message` |
| 0x0203 | Ok | `u32 req` |
| 0x0204 | SessionList | `u32 req, varint n, SessionInfo × n` |
| 0x0205 | SessionCreated | `u32 req, SessionInfo` |
| 0x0206 | Attached | `u32 req, SessionInfo` |
| 0x0207 | Detached | `u64 session, u8 reason` (0 requested, 1 session closed, 2 server shutdown) |
| 0x0208 | Frame | `u64 session, u64 frame_seq, u64 feed_seq, str ops` |
| 0x0209 | Event | `u64 session, u8 kind, svarint x, svarint y, str a, str b` (2.1 appends `str c, str d`) |
| 0x020A | ClipboardRequest | `u64 session, u32 token, str selection` |
| 0x020B | History | `u32 req, u64 session, u64 feed_seq, u64 first_row, u64 history_rows, u64 epoch, u64 start, strings rows` |
| 0x020C | Blob | `u32 req, bool found, str data` |
| 0x020D | SyncDone | `u32 req, u64 session, u64 feed_seq` |
| 0x020E | Pong | `u32 req` |
| 0x020F | SessionNotify | `u8 kind` (0 added, 1 changed, 2 removed), `SessionInfo` |

Error codes:

| Code | Name |
|------|------|
| 1 | BadMessage (connection closed) |
| 2 | UnknownMessage |
| 3 | VersionMismatch (connection closed) |
| 4 | NoSuchSession |
| 5 | SpawnFailed |
| 6 | NotAttached |
| 7 | ReadOnly |
| 8 | NotFound |
| 9 | Internal |
| 10 | HelloRequired |

## Compound bodies

```
SessionSpec := pairs meta, str command, strings args, str windows_command_line, str cwd,
               bool inherit_env, pairs env, strings env_unset, u16 cols, u16 rows,
               u32 scrollback_rows, bool remove_on_exit, u8 resize_policy
SessionInfo := u64 id, pairs meta, str command, svarint pid, bool running, svarint exit_code,
               u16 cols, u16 rows, varint clients, u64 created_ms, str title, str cwd,
               u8 resize_policy, u8 clipboard_policy
KeyEvent    := varint key, varint codepoint, varint shifted, varint base_layout, varint mods,
               u8 action, str text                        (bropty::KeyEvent)
MouseEvent  := u8 action, u8 button, varint mods, svarint col, svarint row,
               svarint x, svarint y                        (bropty::MouseEvent)
```

- An empty `command` runs the user's shell.
- By convention, `meta["name"]` is the session's display name.
- The server does not interpret layout. A client that wants to store one (for example a split tree) keeps it in a blob (PutBlob/GetBlob) or in session meta.

**Resize policy** (`u8`):

| Value | Policy | Rule |
|-------|--------|------|
| 0 | Latest | The most recently active client sets the size. Activity is input, a resize or an attach. |
| 1 | Smallest | The minimum over attached clients. |
| 2 | Largest | The maximum over attached clients. |
| 3 | Fixed | Clients never resize the session. |

Read-only and no-resize attachments, and attachments that have no size yet, never count towards the size.

**Clipboard policy** (`u8`), which governs OSC 52:

| Value | Policy | Effect |
|-------|--------|--------|
| 0 | Deny | Neither writes nor queries reach a client. |
| 1 | WriteOnly (the default) | Writes become `ClipboardWrite` events. Queries go unanswered. |
| 2 | ReadWrite | A query goes as a `ClipboardRequest` to the most recently active client that can send input. Its `ClipboardData` answers the query: the terminal writes the OSC 52 reply, with the query's selection and terminator. `ok = false` gets no reply. Unanswered requests expire after 10 s, without a reply. |

**Event kinds** (`u8`):

| Kind | Name | Fields |
|------|------|--------|
| 1 | Bell | (none) |
| 2 | Title | a = the new value |
| 3 | IconName | a = the new value |
| 4 | Cwd | a = the new value (OSC 7) |
| 5 | Notification | a = title, b = body (OSC 9, OSC 777 and OSC 99); 2.1: x = urgency (0 low, 1 normal, 2 critical, -1 not given), c = OSC 99 id, d = source (`osc9`, `osc777`, `osc99`) |
| 6 | Progress | x = state, y = value (OSC 9;4) |
| 7 | SemanticMark | x = 'A' / 'B' / 'C' / 'D' / ..., a = params (OSC 133) |
| 8 | ClipboardWrite | a = selection, b = decoded data (OSC 52) |
| 9 | Apc | a = payload |
| 10 | ResizedByApp | x = cols, y = rows (DECCOLM) |
| 11 | Exited | x = exit code (-1 unknown) |
| 12 | EventsDropped | x = how many events were dropped |
| 13 | Foreground (2.1) | x = pid (0: none), a = name, b = executable path, c = command line |

`Foreground` is the process that owns the terminal now, as bropty's `IPtyProcess::foreground_process` answers (POSIX: the leader of the terminal's foreground process group; Windows: the youngest console program in the session's process tree, passing over background jobs). The server asks only while a 2.1 client is attached: at attach (the answer goes to that client at once), shortly after input or output, twice more as things settle (0.5 s and 1.5 s later), and every 3 s otherwise. A change goes to every 2.1 client; when the program exits, one with pid 0 follows.

Each attachment has an event queue of at most 2048 events or 16 MiB. A full queue drops newer events and later reports how many with `EventsDropped`. Consecutive bells are coalesced.

## Screens: frames, not bytes

The server runs the terminal emulator, a bropty `Terminal`. A client never sees PTY bytes. It receives the session's *state* as a model it can render: rows of cells, the cursor, modes, title, palette, and the length of the scrollback.

- **`feed_seq`** is the version of a session's state. It increases by one for each chunk of PTY output the emulator consumes, and for each resize. A frame shows exactly the state at its `feed_seq`.
- **`frame_seq`** counts frames per attachment, starting at 1. The client acknowledges each frame with `Ack` after applying it.
- **Attach** produces a full frame: `Op_Size` first, then every row, then the cursor, modes, text and palette. After that, each frame is a diff against what *that client* was last sent. The diff contains rows whose content changed (the server knows rows by the terminal's row stamps, content serials that move with a row as it scrolls), a scroll op when most rows moved together, and state ops only when the state changed. Each client's diffs are independent, so a client that falls behind skips intermediate states and is not sent a backlog.
- **Flow control:**
  - Frames to a client pause while it has 1 MiB of unacknowledged frame bytes, or 1 MiB of output pending on its connection.
  - Frames to one client are at least 8 ms apart. A change after a quiet spell goes out at once.
  - While the program has synchronized output (mode 2026) on, frames are held for up to 150 ms.
  - Each session gets a bounded slice of PTY output per loop turn (256 KiB or 3 ms), round robin. A flooding session therefore cannot starve the others or the clients' I/O.
- **Sync** forces a frame at the session's current `feed_seq`, then replies `SyncDone`. A client that needs to know its model is current, such as a test, uses it.

### Frame op stream (`FrameMsg::ops`)

The op stream is a sequence of `u8 op, body`, applied in order:

| Op | Name | Body |
|----|------|------|
| 1 | Size | `varint cols, varint rows`: every row becomes blank at the new size |
| 2 | Scroll | `svarint k`: rows move up by k (down by -k); vacated rows become blank |
| 3 | Row | `varint y, row` |
| 4 | Cursor | `varint row, varint col, u8 flags` (1 visible, 2 pending wrap, 4 blink), `u8 shape` |
| 5 | Modes | `varint mode bits, u8 mouse tracking, u8 mouse encoding, varint kitty keyboard flags` |
| 6 | Text | `u8 which` (0 title, 1 icon name, 2 cwd; 2.1: 3 the OSC 22 pointer shape), `str value` |
| 7 | Palette | 259 × (`u8 r, u8 g, u8 b`): colors 0-255, then foreground, background, cursor |
| 8 | History | `varint first_row, varint rows, varint epoch`: the scrollback held, as absolute row numbers (below) |
| 9 | Commands (2.1) | `varint drop_front, varint keep, varint n, command × n`: the OSC 133 command records (below) |
| 10 | Images (2.1) | the active screen's inline images and placements, whole (below) |
| 11 | ImageData (2.1) | `varint pixel_serial, varint offset, str bytes`: part of one image frame's RGBA (below) |

Mode bits are listed in `codec.h` (`ModeBit`). The alternate screen is bit 24. A client that receives an op it does not know, or an op that does not validate, rejects the whole frame. That is why a server sends ops 9-11 and `Text` 3 only to clients whose `Hello` said minor 1 or later.

### Command records (2.1)

The terminal keeps the shell's commands from OSC 133 marks (bropty `Terminal::commands()`, at most 10000). `Op_Commands` brings a client's copy up to date: it drops its first `drop_front` records (those the terminal let go of), keeps the next `keep`, and appends the `n` that follow.

```
command := svarint prompt_row, varint prompt_col,
           u8 flags (1 input, 2 output, 4 end, 8 exit code, 16 finished, 32 trimmed),
           [svarint row, varint col] for each of input, output, end present,
           [svarint exit_code], str command_line
```

Positions are absolute rows, numbered like the screen and scrollback (they change with the numbering epoch, and the records are then sent again). When the shell did not report a command line (`cmdline=`, OSC 633;E), the server fills in the text typed between the end of the prompt (B) and the start of the output (C), as the screen reads.

### Inline images (2.1)

A session's terminal holds kitty graphics images and placements, and sixel and iTerm2 images drawn into cells (bropty `graphics.h`). Image cells travel in rows like any other cell. `Op_Images` sends the active screen's images and placements whole, whenever they change (a transmission, a placement, a deletion, an animation frame, a screen switch, a new cell size):

```
Images    := varint cell_width, varint cell_height, u8 flags (1: cells may hold image placeholders),
             varint n, image × n, varint m, placement × m
image     := varint key, varint id, varint number, u8 source (0 kitty, 1 sixel, 2 iTerm2),
             varint width, varint height,
             varint nframes, (varint pixel_serial, varint gap_ms) × nframes, varint current_frame,
             u32 cell_width, u32 cell_height (IEEE 754 float bits), varint box_cols, varint box_rows
placement := varint image_key, varint image_id, varint placement_id,
             svarint row (absolute), svarint col, svarint x_offset, svarint y_offset,
             varint src_x, varint src_y, varint src_w, varint src_h,
             svarint cols, svarint rows, svarint z, u8 virtual,
             varint parent_image_key, varint parent_placement_id, varint parent_serial,
             svarint parent_dx, svarint parent_dy, varint serial
```

Pixels go separately. A pixel serial names one frame's RGBA: `width × height × 4` bytes, 8-bit, not premultiplied. After an `Op_Images` the client holds the pixels of exactly the serials it lists (those it already had are kept, the rest dropped), and the server tracks the same set per client. It then sends each listed frame the client lacks in `Op_ImageData` chunks, in list order, at most 1 MiB per frame, so a large image takes several frames and never holds the screen back. Data for a serial the client does not hold is ignored. An image shows once its current frame's pixels are complete. Animations run on the server, and an animation step is an `Op_Images` naming another current frame, whose pixels the client already holds.

The server decodes compressed images (kitty `f=100` PNG, iTerm2's formats) with `ServerOptions::decode_image`. The `bromux` executable sets it to broimage when its build found broimage; without one, those transmissions fail with kitty's error reply, and raw RGB/RGBA, zlib-compressed kitty data and sixel still work.

### Row encoding

A row is self-contained, and equal rows encode to equal bytes:

```
row   := varint flags, varint cols, varint nstyles, style × nstyles, run*
style := u32 fg, u32 bg, u32 underline_color    (bropty::Color::packed())
         varint attrs, u8 underline, u8 zone,
         u8 has_link [, str link_id, str uri]
run   := varint (count << 2 | kind), varint style_index, payload
```

Run kinds:

| Kind | Name | Payload |
|------|------|---------|
| 0 | blank | none; `count` empty cells |
| 1 | plain | `count` varint code points: narrow, unclustered, unprotected cells |
| 2 | general | per cell, `varint cell_bits` (bropty `Cell::bits`, low 25 bits). When the cluster bit is set, it is followed by `varint n` and `n` varint code points (the tail of the grapheme). |

- The runs cover exactly `cols` cells.
- Styles form a row-local palette, in order of first use.
- `zone` is the OSC 133 zone (bropty `Style::zone`).
- Hyperlinks (OSC 8) travel inline with the style that uses them.

### Scrollback

Scrollback is never pushed to clients. Rows have absolute numbers (bropty's): the scrollback is rows `first_row .. first_row + rows - 1`, and screen row 0 is row `first_row + rows`, also while the alternate screen (which has no scrollback of its own) is shown. Rows evicted from the front of the scrollback (its capacity, `ED 3`) take their numbers with them: `first_row` only grows. A row keeps its number, and a scrollback row its content, while `epoch` stays the same. A resize reflows the scrollback and starts a new epoch, and a client drops whatever rows it cached. One exception: the newest scrollback row's wrap flag can still change while its line continues on screen row 0.

`Op_History` keeps a client informed of all three. `FetchHistory(start, count)` asks for rows from absolute row `start`. The server clamps `start` to the rows it holds and returns at most 10000 rows per request, as row encodings. The reply carries `start` (the number of `rows[0]`), `first_row`, `history_rows` and `epoch` as they were when the rows were read, and that moment's `feed_seq`. A client keeps rows only from a reply in its current epoch.

The client library serves a session's model plus fetched scrollback as a bropty `RowSource` (`ScreenSource`, `Client::view()`), fetching rows in aligned blocks as a view or search needs them.

## Session recordings (testing)

A server started with `--tee-dir DIR` (`ServerOptions::tee_dir`) writes `DIR/session-<id>.tee` for every session. The recording is everything that changed the session's terminal:

```
"BMXTEE1\n", u16 cols, u16 rows, u32 scrollback_rows          (the initial terminal)
record := u8 kind, u32 length, body
          kind 1 (PTY data): the bytes;  kind 2 (resize): u16 cols, u16 rows
```

Record *n* is the change that produced `feed_seq` *n*. Replaying the first `feed_seq` records into a fresh bropty Terminal (`replay_tee`) reproduces the screen of any frame exactly. This is the oracle the test suite uses.
