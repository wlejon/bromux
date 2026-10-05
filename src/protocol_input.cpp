// Bodies shared by several messages: session spec / info and input events.
#include "bromux/protocol.h"

namespace bromux {

void SessionSpec::write(wire::Writer& w) const {
    w.pairs(meta);
    w.str(command);
    w.strings(args);
    w.str(windows_command_line);
    w.str(cwd);
    w.boolean(inherit_env);
    w.pairs(env);
    w.strings(env_unset);
    w.u16(cols);
    w.u16(rows);
    w.u32(scrollback_rows);
    w.boolean(remove_on_exit);
    w.u8(uint8_t(resize_policy));
}

void SessionSpec::read(wire::Reader& r) {
    meta = r.pairs();
    command = r.str();
    args = r.strings();
    windows_command_line = r.str();
    cwd = r.str();
    inherit_env = r.boolean();
    env = r.pairs();
    env_unset = r.strings();
    cols = r.u16();
    rows = r.u16();
    scrollback_rows = r.u32();
    remove_on_exit = r.boolean();
    uint8_t p = r.u8();
    if (p > uint8_t(ResizePolicy::Fixed)) r.fail();
    resize_policy = ResizePolicy(p);
}

std::string SessionInfo::meta_value(std::string_view key) const {
    for (const auto& [k, v] : meta)
        if (k == key) return v;
    return {};
}

void SessionInfo::write(wire::Writer& w) const {
    w.u64(id);
    w.pairs(meta);
    w.str(command);
    w.svarint(pid);
    w.boolean(running);
    w.svarint(exit_code);
    w.u16(cols);
    w.u16(rows);
    w.varint(clients);
    w.u64(created_ms);
    w.str(title);
    w.str(cwd);
    w.u8(uint8_t(resize_policy));
    w.u8(uint8_t(clipboard_policy));
}

void SessionInfo::read(wire::Reader& r) {
    id = r.u64();
    meta = r.pairs();
    command = r.str();
    pid = r.svarint();
    running = r.boolean();
    exit_code = int32_t(r.svarint());
    cols = r.u16();
    rows = r.u16();
    clients = uint32_t(r.varint_max(UINT32_MAX));
    created_ms = r.u64();
    title = r.str();
    cwd = r.str();
    uint8_t rp = r.u8();
    uint8_t cp = r.u8();
    if (rp > uint8_t(ResizePolicy::Fixed) || cp > uint8_t(ClipboardPolicy::ReadWrite)) r.fail();
    resize_policy = ResizePolicy(rp);
    clipboard_policy = ClipboardPolicy(cp);
}

void write_key_event(wire::Writer& w, const bropty::KeyEvent& ev) {
    w.varint(uint32_t(ev.key));
    w.varint(uint32_t(ev.codepoint));
    w.varint(uint32_t(ev.shifted));
    w.varint(uint32_t(ev.base_layout));
    w.varint(ev.mods);
    w.u8(uint8_t(ev.action));
    w.str(ev.text);
}

bool read_key_event(wire::Reader& r, bropty::KeyEvent& ev) {
    ev.key = bropty::Key(r.varint_max(0x10FFFF));
    ev.codepoint = char32_t(r.varint_max(0x10FFFF));
    ev.shifted = char32_t(r.varint_max(0x10FFFF));
    ev.base_layout = char32_t(r.varint_max(0x10FFFF));
    ev.mods = bropty::KeyMods(r.varint_max(0xFFFF));
    uint8_t a = r.u8();
    if (a < 1 || a > 3) r.fail();
    ev.action = bropty::KeyAction(a);
    ev.text = r.str();
    return r.ok();
}

void write_mouse_event(wire::Writer& w, const bropty::MouseEvent& ev) {
    w.u8(uint8_t(ev.action));
    w.u8(uint8_t(ev.button));
    w.varint(ev.mods);
    w.svarint(ev.col);
    w.svarint(ev.row);
    w.svarint(ev.x);
    w.svarint(ev.y);
}

bool read_mouse_event(wire::Reader& r, bropty::MouseEvent& ev) {
    uint8_t a = r.u8();
    uint8_t b = r.u8();
    if (a > uint8_t(bropty::MouseAction::Motion) || b > uint8_t(bropty::MouseButton::Button11)) r.fail();
    ev.action = bropty::MouseAction(a);
    ev.button = bropty::MouseButton(b);
    ev.mods = bropty::KeyMods(r.varint_max(0xFFFF));
    auto coord = [&r] {
        int64_t v = r.svarint();
        if (v < -1000000 || v > 1000000) r.fail();
        return int(v);
    };
    ev.col = coord();
    ev.row = coord();
    ev.x = coord();
    ev.y = coord();
    return r.ok();
}

}  // namespace bromux
