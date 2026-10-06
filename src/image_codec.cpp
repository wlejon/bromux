// Inline images in frames (image_codec.h).
#include "bromux/image_codec.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <memory>
#include <unordered_set>

namespace bromux {

namespace {

constexpr int64_t kMaxRow = INT64_MAX / 4;
constexpr int64_t kMaxCoord = 1 << 24;

void write_image(wire::Writer& w, const bropty::Image& img, std::vector<bropty::ImagePixelsPtr>& frames,
                 std::unordered_set<uint64_t>& seen) {
    w.varint(img.key);
    w.varint(img.id);
    w.varint(img.number);
    w.u8(uint8_t(img.source));
    w.varint(img.width);
    w.varint(img.height);
    w.varint(img.frames.size());
    for (const bropty::ImageFrame& f : img.frames) {
        const uint64_t serial = f.pixels ? f.pixels->serial : 0;
        w.varint(serial);
        w.varint(f.gap_ms);
        if (serial && seen.insert(serial).second) frames.push_back(f.pixels);
    }
    w.varint(img.current_frame);
    w.u32(std::bit_cast<uint32_t>(img.cell_width));
    w.u32(std::bit_cast<uint32_t>(img.cell_height));
    w.varint(uint64_t(std::max(0, img.box_cols)));
    w.varint(uint64_t(std::max(0, img.box_rows)));
}

void write_placement(wire::Writer& w, const bropty::Placement& p) {
    w.varint(p.image_key);
    w.varint(p.image_id);
    w.varint(p.placement_id);
    w.svarint(p.row);
    w.svarint(p.col);
    w.svarint(p.x_offset);
    w.svarint(p.y_offset);
    w.varint(p.src_x);
    w.varint(p.src_y);
    w.varint(p.src_w);
    w.varint(p.src_h);
    w.svarint(p.cols);
    w.svarint(p.rows);
    w.svarint(p.z);
    w.u8(p.is_virtual ? 1 : 0);
    w.varint(p.parent_image_key);
    w.varint(p.parent_placement_id);
    w.varint(p.parent_serial);
    w.svarint(p.parent_dx);
    w.svarint(p.parent_dy);
    w.varint(p.serial);
}

int read_int(wire::Reader& r) {
    const int64_t v = r.svarint();
    if (v < -kMaxCoord || v > kMaxCoord) r.fail();
    return int(v);
}

uint32_t read_u32(wire::Reader& r) { return uint32_t(r.varint_max(UINT32_MAX)); }

bool read_placement(wire::Reader& r, bropty::Placement& p) {
    p.image_key = r.varint();
    p.image_id = read_u32(r);
    p.placement_id = read_u32(r);
    p.row = r.svarint();
    if (p.row < -kMaxRow || p.row > kMaxRow) r.fail();
    p.col = read_int(r);
    p.x_offset = read_int(r);
    p.y_offset = read_int(r);
    p.src_x = read_u32(r);
    p.src_y = read_u32(r);
    p.src_w = read_u32(r);
    p.src_h = read_u32(r);
    p.cols = read_int(r);
    p.rows = read_int(r);
    const int64_t z = r.svarint();
    if (z < INT32_MIN || z > INT32_MAX) r.fail();
    p.z = int32_t(z);
    p.is_virtual = r.u8() != 0;
    p.parent_image_key = r.varint();
    p.parent_placement_id = read_u32(r);
    p.parent_serial = r.varint();
    p.parent_dx = read_int(r);
    p.parent_dy = read_int(r);
    p.serial = r.varint();
    return r.ok();
}

}  // namespace

void write_images(wire::Writer& w, const bropty::ImageLayer& layer, int cell_width, int cell_height,
                  bool may_have_cells, std::vector<bropty::ImagePixelsPtr>& frames) {
    w.varint(uint64_t(std::max(0, cell_width)));
    w.varint(uint64_t(std::max(0, cell_height)));
    w.u8(may_have_cells ? 1 : 0);
    std::unordered_set<uint64_t> seen;
    std::vector<const bropty::Image*> images;
    layer.for_each_image([&images](const bropty::Image& img) { images.push_back(&img); });
    // A stable order (the layer's maps are unordered): by creation.
    std::sort(images.begin(), images.end(),
              [](const bropty::Image* a, const bropty::Image* b) { return a->serial < b->serial; });
    w.varint(images.size());
    for (const bropty::Image* img : images) write_image(w, *img, frames, seen);
    w.varint(layer.placements().size());
    for (const bropty::Placement& p : layer.placements()) write_placement(w, p);
}

void write_image_data(wire::Writer& w, const bropty::ImagePixels& px, size_t offset, size_t n) {
    w.varint(px.serial);
    w.varint(offset);
    w.str(std::string_view(reinterpret_cast<const char*>(px.rgba.data()) + offset, n));
}

// ---- client --------------------------------------------------------------------

void ImageMirror::reset() {
    have_ = false;
    dirty_ = false;
    cell_w_ = cell_h_ = 0;
    cells_ = false;
    images_.clear();
    placements_.clear();
    pixels_.clear();
    layer_.clear();
}

bool ImageMirror::apply_images(wire::Reader& r) {
    const int cw = int(r.varint_max(4096));
    const int ch = int(r.varint_max(4096));
    const uint8_t flags = r.u8();
    const uint64_t n = r.varint_max(r.remaining());
    if (!r.ok()) return false;
    std::vector<Entry> images;
    images.reserve(size_t(n));
    std::unordered_map<uint64_t, Pixels> keep;
    for (uint64_t i = 0; i < n; ++i) {
        Entry e;
        bropty::Image& img = e.image;
        img.key = r.varint();
        img.id = read_u32(r);
        img.number = read_u32(r);
        const uint8_t source = r.u8();
        img.width = uint32_t(r.varint_max(kMaxSide));
        img.height = uint32_t(r.varint_max(kMaxSide));
        const uint64_t nframes = r.varint_max(std::min<uint64_t>(kMaxFrames, r.remaining()));
        if (!r.ok() || source > uint8_t(bropty::ImageSource::Iterm2) || nframes == 0) return false;
        img.source = bropty::ImageSource(source);
        e.serials.resize(size_t(nframes));
        img.frames.resize(size_t(nframes));
        for (uint64_t f = 0; f < nframes; ++f) {
            e.serials[size_t(f)] = r.varint();
            img.frames[size_t(f)].gap_ms = read_u32(r);
        }
        img.current_frame = uint32_t(r.varint_max(nframes - 1));
        img.cell_width = std::bit_cast<float>(r.u32());
        img.cell_height = std::bit_cast<float>(r.u32());
        img.box_cols = int(r.varint_max(1 << 16));
        img.box_rows = int(r.varint_max(1 << 16));
        if (!r.ok()) return false;
        if (!(img.cell_width >= 0 && img.cell_width < 1e6f) || !(img.cell_height >= 0 && img.cell_height < 1e6f))
            return false;
        // Every serial this list names is held (moved over), the rest go.
        for (uint64_t s : e.serials) {
            if (!s || keep.count(s)) continue;
            auto it = pixels_.find(s);
            if (it != pixels_.end()) {
                keep.emplace(s, std::move(it->second));
            } else {
                Pixels px;
                px.width = img.width;
                px.height = img.height;
                keep.emplace(s, std::move(px));
            }
        }
        images.push_back(std::move(e));
    }
    const uint64_t m = r.varint_max(r.remaining());
    if (!r.ok()) return false;
    std::vector<bropty::Placement> placements(static_cast<size_t>(m));
    for (bropty::Placement& p : placements)
        if (!read_placement(r, p)) return false;
    have_ = true;
    cell_w_ = cw;
    cell_h_ = ch;
    cells_ = (flags & 1) != 0;
    images_ = std::move(images);
    placements_ = std::move(placements);
    pixels_ = std::move(keep);
    dirty_ = true;
    return true;
}

bool ImageMirror::apply_data(wire::Reader& r) {
    const uint64_t serial = r.varint();
    const uint64_t offset = r.varint();
    const std::string_view bytes = r.str_view();
    if (!r.ok()) return false;
    auto it = pixels_.find(serial);
    if (it == pixels_.end() || it->second.done) return true;  // not (or no longer) wanted
    Pixels& px = it->second;
    const size_t total = size_t(px.width) * px.height * 4;
    if (offset != px.received || offset + bytes.size() > total) return false;
    if (px.rgba.size() != total) px.rgba.resize(total);
    if (!bytes.empty()) std::memcpy(px.rgba.data() + offset, bytes.data(), bytes.size());
    px.received += bytes.size();
    if (px.received == total) {
        px.done = bropty::make_image_pixels(px.width, px.height, std::move(px.rgba));
        px.rgba = {};
        dirty_ = true;
    }
    return true;
}

bool ImageMirror::finish() {
    if (!dirty_) return false;
    dirty_ = false;
    layer_.clear();
    for (const Entry& e : images_) {
        auto cur = pixels_.find(e.serials[e.image.current_frame]);
        if (cur == pixels_.end() || !cur->second.done) continue;  // shown once it has arrived
        auto img = std::make_unique<bropty::Image>(e.image);
        for (size_t f = 0; f < e.serials.size(); ++f) {
            auto it = pixels_.find(e.serials[f]);
            img->frames[f].pixels = it != pixels_.end() ? it->second.done : nullptr;
        }
        layer_.put(std::move(img));
    }
    for (const bropty::Placement& p : placements_) layer_.add_placement(p);
    return true;
}

bropty::SourceImages ImageMirror::source_images() const noexcept {
    bropty::SourceImages s;
    if (!have_) return s;
    s.layer = &layer_;
    s.cell_width = cell_w_;
    s.cell_height = cell_h_;
    s.may_have_cells = cells_;
    return s;
}

size_t ImageMirror::bytes() const noexcept {
    size_t n = 0;
    for (const auto& [serial, px] : pixels_)
        if (px.done) n += px.done->rgba.size();
    return n;
}

}  // namespace bromux
