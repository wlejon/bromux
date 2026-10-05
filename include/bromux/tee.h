#pragma once
// Session recordings ("tee" files): every input that changed a session's
// terminal, in order -- PTY output chunks and resizes -- so a fresh bropty
// Terminal replays the session to exactly the state any frame showed. Record
// i (0-based) is the change that took the session's feed_seq from i to i + 1,
// so replaying the first FrameMsg::feed_seq records reproduces that frame.
// The server writes them when ServerOptions::tee_dir is set; tests use them
// as the oracle, and they are handy for reproducing rendering bugs.
//
// File format: "BMXTEE1\n", u16 cols, u16 rows, u32 scrollback rows, then
// records: u8 kind (1 data, 2 resize), u32 length, body (data: the bytes;
// resize: u16 cols, u16 rows). Little endian.

#include <bropty/terminal.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bromux {

struct TeeHeader {
    int cols{80};
    int rows{24};
    uint32_t scrollback_rows{10000};
};

struct TeeRecord {
    enum Kind : uint8_t { Data = 1, Resize = 2 };
    Kind kind{Data};
    std::string data;
    int cols{0};
    int rows{0};
};

class TeeWriter {
public:
    TeeWriter() = default;
    ~TeeWriter();
    TeeWriter(const TeeWriter&) = delete;
    TeeWriter& operator=(const TeeWriter&) = delete;

    bool open(const std::string& path, const TeeHeader& header);
    void data(std::string_view bytes);
    void resize(int cols, int rows);
    void flush();
    void close();
    [[nodiscard]] bool is_open() const noexcept { return f_ != nullptr; }

private:
    void record(uint8_t kind, std::string_view body);
    std::FILE* f_{nullptr};
};

// Read a whole recording. A truncated final record (a writer still running)
// is ignored.
bool read_tee(const std::string& path, TeeHeader& header, std::vector<TeeRecord>& records,
              std::string* err = nullptr);

// A terminal set up as the header says, with the first `count` records applied.
[[nodiscard]] std::unique_ptr<bropty::Terminal> replay_tee(const TeeHeader& header,
                                                           const std::vector<TeeRecord>& records,
                                                           size_t count);

}  // namespace bromux
