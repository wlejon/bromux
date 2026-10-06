// OSC 133 command records in Op_Commands (codec.h).
#include "bromux/codec.h"

namespace bromux {

namespace {

enum CommandFlags : uint8_t {
    Cmd_Input = 1,
    Cmd_Output = 2,
    Cmd_End = 4,
    Cmd_ExitCode = 8,
    Cmd_Finished = 16,
    Cmd_Trimmed = 32,
};

constexpr int64_t kMaxRow = INT64_MAX / 4;
constexpr uint64_t kMaxCol = 0xFFFF;

void write_pos(wire::Writer& w, const bropty::RowPos& p) {
    w.svarint(p.row);
    w.varint(uint64_t(p.col < 0 ? 0 : p.col));
}

bool read_pos(wire::Reader& r, bropty::RowPos& p) {
    p.row = r.svarint();
    p.col = int(r.varint_max(kMaxCol));
    if (p.row < -kMaxRow || p.row > kMaxRow) r.fail();
    return r.ok();
}

}  // namespace

void write_command(wire::Writer& w, const bropty::CommandRecord& c) {
    write_pos(w, c.prompt);
    uint8_t flags = 0;
    if (c.input) flags |= Cmd_Input;
    if (c.output) flags |= Cmd_Output;
    if (c.end) flags |= Cmd_End;
    if (c.exit_code) flags |= Cmd_ExitCode;
    if (c.finished) flags |= Cmd_Finished;
    if (c.trimmed) flags |= Cmd_Trimmed;
    w.u8(flags);
    if (c.input) write_pos(w, *c.input);
    if (c.output) write_pos(w, *c.output);
    if (c.end) write_pos(w, *c.end);
    if (c.exit_code) w.svarint(*c.exit_code);
    w.str(c.command_line);
}

bool read_command(wire::Reader& r, bropty::CommandRecord& c) {
    c = bropty::CommandRecord{};
    if (!read_pos(r, c.prompt)) return false;
    const uint8_t flags = r.u8();
    if (!r.ok() || flags >= 64) return false;
    auto opt = [&r](bool present, std::optional<bropty::RowPos>& out) {
        if (!present) return true;
        bropty::RowPos p;
        if (!read_pos(r, p)) return false;
        out = p;
        return true;
    };
    if (!opt(flags & Cmd_Input, c.input) || !opt(flags & Cmd_Output, c.output) || !opt(flags & Cmd_End, c.end))
        return false;
    if (flags & Cmd_ExitCode) {
        const int64_t v = r.svarint();
        if (v < INT32_MIN || v > INT32_MAX) r.fail();
        c.exit_code = int(v);
    }
    c.finished = (flags & Cmd_Finished) != 0;
    c.trimmed = (flags & Cmd_Trimmed) != 0;
    c.command_line = r.str();
    return r.ok();
}

bool same_command(const bropty::CommandRecord& a, const bropty::CommandRecord& b) noexcept {
    return a.prompt == b.prompt && a.input == b.input && a.output == b.output && a.end == b.end &&
           a.exit_code == b.exit_code && a.finished == b.finished && a.trimmed == b.trimmed &&
           a.command_line == b.command_line;
}

}  // namespace bromux
