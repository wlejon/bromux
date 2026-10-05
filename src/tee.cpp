#include "bromux/tee.h"

#include <algorithm>
#include <cstring>

namespace bromux {

namespace {
constexpr char kMagic[8] = {'B', 'M', 'X', 'T', 'E', 'E', '1', '\n'};

void put16(std::string& s, uint32_t v) {
    s.push_back(char(v));
    s.push_back(char(v >> 8));
}
void put32(std::string& s, uint32_t v) {
    for (int i = 0; i < 4; ++i) s.push_back(char(v >> (8 * i)));
}
uint32_t get(const unsigned char* p, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i) v |= uint32_t(p[i]) << (8 * i);
    return v;
}
}  // namespace

TeeWriter::~TeeWriter() { close(); }

bool TeeWriter::open(const std::string& path, const TeeHeader& header) {
    close();
    f_ = std::fopen(path.c_str(), "wb");
    if (!f_) return false;
    std::string h(kMagic, sizeof kMagic);
    put16(h, uint32_t(header.cols));
    put16(h, uint32_t(header.rows));
    put32(h, header.scrollback_rows);
    std::fwrite(h.data(), 1, h.size(), f_);
    return true;
}

void TeeWriter::record(uint8_t kind, std::string_view body) {
    if (!f_) return;
    std::string h;
    h.push_back(char(kind));
    put32(h, uint32_t(body.size()));
    std::fwrite(h.data(), 1, h.size(), f_);
    if (!body.empty()) std::fwrite(body.data(), 1, body.size(), f_);
}

void TeeWriter::data(std::string_view bytes) { record(TeeRecord::Data, bytes); }

void TeeWriter::resize(int cols, int rows) {
    std::string b;
    put16(b, uint32_t(cols));
    put16(b, uint32_t(rows));
    record(TeeRecord::Resize, b);
}

void TeeWriter::flush() {
    if (f_) std::fflush(f_);
}

void TeeWriter::close() {
    if (f_) {
        std::fclose(f_);
        f_ = nullptr;
    }
}

bool read_tee(const std::string& path, TeeHeader& header, std::vector<TeeRecord>& records, std::string* err) {
    records.clear();
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (err) *err = "cannot open " + path;
        return false;
    }
    std::string all;
    char buf[1 << 16];
    for (;;) {
        size_t n = std::fread(buf, 1, sizeof buf, f);
        if (n == 0) break;
        all.append(buf, n);
    }
    std::fclose(f);
    const auto* p = reinterpret_cast<const unsigned char*>(all.data());
    const size_t n = all.size();
    if (n < 16 || std::memcmp(p, kMagic, 8) != 0) {
        if (err) *err = "not a bromux tee file: " + path;
        return false;
    }
    header.cols = int(get(p + 8, 2));
    header.rows = int(get(p + 10, 2));
    header.scrollback_rows = get(p + 12, 4);
    size_t pos = 16;
    while (pos + 5 <= n) {
        const uint8_t kind = p[pos];
        const uint32_t len = get(p + pos + 1, 4);
        if (pos + 5 + len > n) break;  // truncated tail
        TeeRecord r;
        if (kind == TeeRecord::Data) {
            r.kind = TeeRecord::Data;
            r.data.assign(all, pos + 5, len);
        } else if (kind == TeeRecord::Resize && len == 4) {
            r.kind = TeeRecord::Resize;
            r.cols = int(get(p + pos + 5, 2));
            r.rows = int(get(p + pos + 7, 2));
        } else {
            if (err) *err = "corrupt tee record";
            return false;
        }
        records.push_back(std::move(r));
        pos += 5 + len;
    }
    return true;
}

std::unique_ptr<bropty::Terminal> replay_tee(const TeeHeader& header, const std::vector<TeeRecord>& records,
                                             size_t count) {
    bropty::TerminalOptions o;
    o.cols = header.cols;
    o.rows = header.rows;
    o.scrollback_rows = header.scrollback_rows;
    auto t = std::make_unique<bropty::Terminal>(o);
    count = std::min(count, records.size());
    for (size_t i = 0; i < count; ++i) {
        const TeeRecord& r = records[i];
        if (r.kind == TeeRecord::Data) t->feed(r.data);
        else t->resize(r.cols, r.rows);
    }
    return t;
}

}  // namespace bromux
