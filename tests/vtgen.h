#pragma once
// Deterministic random VT streams: text (ASCII, wide CJK, combining marks,
// emoji clusters), SGR in every color form and underline style, cursor
// movement, erasure, insert / delete, scroll regions and margins, the
// alternate screen, saved cursor, hyperlinks, titles, OSC 133 marks, mode
// toggles. Used in-process (codec tests) and by mux_child (PTY tests).

#include <cstdint>
#include <string>

namespace vtgen {

class Rng {
public:
    explicit Rng(uint64_t seed) : s_(seed * 0x9E3779B97F4A7C15ull + 1) {}
    uint64_t next() {
        s_ ^= s_ << 13;
        s_ ^= s_ >> 7;
        s_ ^= s_ << 17;
        return s_;
    }
    int range(int lo, int hi) { return lo + int(next() % uint64_t(hi - lo + 1)); }  // inclusive
    bool chance(int percent) { return range(1, 100) <= percent; }

private:
    uint64_t s_;
};

inline void utf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(char(cp));
    } else if (cp < 0x800) {
        out.push_back(char(0xC0 | (cp >> 6)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(char(0xE0 | (cp >> 12)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(char(0xF0 | (cp >> 18)));
        out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
}

inline std::string color(Rng& r, int base) {
    switch (r.range(0, 3)) {
    case 0: return std::to_string(base + r.range(0, 7));
    case 1: return std::to_string(base + 60 + r.range(0, 7));
    case 2: return std::to_string(base + 8) + ";5;" + std::to_string(r.range(0, 255));
    default:
        return std::to_string(base + 8) + ";2;" + std::to_string(r.range(0, 255)) + ";" +
               std::to_string(r.range(0, 255)) + ";" + std::to_string(r.range(0, 255));
    }
}

// One random "action" appended to `out`. `cols` / `rows` bound positions.
inline void action(Rng& r, std::string& out, int cols, int rows, bool allow_modes = true) {
    const std::string csi = "\x1b[";
    int k = r.range(0, 99);
    if (k < 30) {  // plain text
        int n = r.range(1, 40);
        for (int i = 0; i < n; ++i) out.push_back(char(r.range(0x21, 0x7E)));
        if (r.chance(30)) out.push_back(' ');
    } else if (k < 36) {  // wide / combining / emoji
        switch (r.range(0, 4)) {
        case 0: utf8(out, char32_t(0x4E00 + r.range(0, 500))); break;   // CJK
        case 1: out += "e"; utf8(out, 0x0301); break;                    // e + acute
        case 2: utf8(out, 0x1F600 + r.range(0, 40)); break;              // emoji
        case 3: utf8(out, 0x2764); utf8(out, 0xFE0F); break;             // heart + VS16
        default: utf8(out, 0x1F468); utf8(out, 0x200D); utf8(out, 0x1F469); break;  // ZWJ
        }
    } else if (k < 46) {  // SGR
        std::string p;
        int n = r.range(1, 4);
        for (int i = 0; i < n; ++i) {
            if (!p.empty()) p += ";";
            switch (r.range(0, 9)) {
            case 0: p += "0"; break;
            case 1: p += std::to_string(r.range(1, 9)); break;
            case 2: p += color(r, 30); break;
            case 3: p += color(r, 40); break;
            case 4: p += "4:" + std::to_string(r.range(0, 5)); break;
            case 5: p += "58;5;" + std::to_string(r.range(0, 255)); break;
            case 6: p += std::to_string(r.range(21, 29)); break;
            case 7: p += "53"; break;
            case 8: p += "39;49"; break;
            default: p += "7"; break;
            }
        }
        out += csi + p + "m";
    } else if (k < 52) {  // newlines and carriage returns (scrolling)
        out += r.chance(50) ? "\r\n" : "\n";
    } else if (k < 58) {  // cursor position
        out += csi + std::to_string(r.range(1, rows)) + ";" + std::to_string(r.range(1, cols)) + "H";
    } else if (k < 62) {  // relative moves
        const char* m = "ABCDEFG";
        out += csi + std::to_string(r.range(1, 5)) + m[r.range(0, 6)];
    } else if (k < 66) {  // erase
        out += csi + std::to_string(r.range(0, 2)) + (r.chance(50) ? "J" : "K");
    } else if (k < 70) {  // insert / delete chars and lines, erase chars, REP
        const char* m = "@PLMXb";
        out += csi + std::to_string(r.range(1, 4)) + m[r.range(0, 5)];
    } else if (k < 73) {  // scroll region
        if (r.chance(30)) {
            out += csi + "r";
        } else {
            int top = r.range(1, rows);
            int bot = r.range(top, rows);
            out += csi + std::to_string(top) + ";" + std::to_string(bot) + "r";
        }
    } else if (k < 76) {  // index / reverse index / scroll up / down
        const char* s[] = {"\x1b" "D", "\x1b" "M", "\x1b" "E", "\x1b[S", "\x1b[2S", "\x1b[T"};
        out += s[r.range(0, 5)];
    } else if (k < 78) {  // save / restore cursor
        out += r.chance(50) ? "\x1b" "7" : "\x1b" "8";
    } else if (k < 80) {  // alternate screen
        if (allow_modes) out += r.chance(50) ? "\x1b[?1049h" : "\x1b[?1049l";
    } else if (k < 83) {  // hyperlinks
        if (r.chance(50)) out += "\x1b]8;id=l" + std::to_string(r.range(0, 5)) + ";https://e.x/" +
                                 std::to_string(r.range(0, 99)) + "\x1b\\";
        else out += "\x1b]8;;\x1b\\";
    } else if (k < 85) {  // title / icon
        out += "\x1b]" + std::to_string(r.range(0, 2)) + ";t" + std::to_string(r.range(0, 999)) + "\x07";
    } else if (k < 87) {  // OSC 133
        const char* m = "ABCD";
        out += std::string("\x1b]133;") + m[r.range(0, 3)] + "\x07";
    } else if (k < 89) {  // tab, backspace, bell
        const char* s[] = {"\t", "\b", "\x07", "\x1b[2I", "\x1b[Z"};
        out += s[r.range(0, 4)];
    } else if (k < 92) {  // modes
        if (!allow_modes) return;
        const int modes[] = {1, 4, 6, 7, 12, 25, 45, 1000, 1002, 1004, 1006, 2004, 2026};
        int m = modes[r.range(0, 12)];
        out += csi + (m == 4 ? "" : "?") + std::to_string(m) + (r.chance(50) ? "h" : "l");
    } else if (k < 94) {  // cursor style
        out += csi + std::to_string(r.range(0, 6)) + " q";
    } else if (k < 96) {  // OSC 7 cwd
        out += "\x1b]7;file://host/tmp/d" + std::to_string(r.range(0, 9)) + "\x1b\\";
    } else if (k < 98) {  // palette
        out += "\x1b]4;" + std::to_string(r.range(0, 255)) + ";rgb:" + std::to_string(r.range(10, 99)) + "/40/50\x07";
    } else {  // double-width line / reset of palette
        out += r.chance(50) ? "\x1b#6" : "\x1b]104\x07";
    }
}

// A whole stream of `n` actions, ending with synchronized output off (so a
// server never holds the final frame back).
inline std::string stream(uint64_t seed, int n, int cols, int rows, bool allow_modes = true) {
    Rng r(seed);
    std::string out;
    for (int i = 0; i < n; ++i) action(r, out, cols, rows, allow_modes);
    out += "\x1b[?2026l";
    return out;
}

}  // namespace vtgen
