#pragma once
// Inline images in frames (minor 1; codec.h Op_Images / Op_ImageData).
//
// A session's terminal (bropty) holds kitty images and placements and
// sixel / iTerm2 cell images per screen (bropty/graphics.h). A client gets
// the active screen's set whole, in an Op_Images, whenever it changes; the
// pixels of each image frame travel once per client, in Op_ImageData chunks
// that follow, possibly over several frames:
//
//   Op_Images    := varint cell_width, varint cell_height,
//                   u8 flags (1: cells may hold image placeholders),
//                   varint n, image * n, varint m, placement * m
//   image        := varint key, varint id, varint number, u8 source (0 kitty, 1 sixel, 2 iTerm2),
//                   varint width, varint height,
//                   varint nframes, (varint pixel serial, varint gap_ms) * nframes, varint current frame,
//                   u32 cell_width, u32 cell_height (IEEE 754 float bits), varint box_cols, varint box_rows
//   placement    := varint image_key, varint image_id, varint placement_id,
//                   svarint row (absolute), svarint col, svarint x_offset, svarint y_offset,
//                   varint src_x, varint src_y, varint src_w, varint src_h,
//                   svarint cols, svarint rows, svarint z, u8 virtual,
//                   varint parent_image_key, varint parent_placement_id, varint parent_serial,
//                   svarint parent_dx, svarint parent_dy, varint serial
//   Op_ImageData := varint pixel serial, varint offset, str bytes
//
// A pixel serial names one frame's RGBA (width * height * 4 bytes, the
// image's size). The client keeps the pixels of every serial the latest
// Op_Images lists and forgets the rest; the server tracks the same set per
// client, so pixels are sent again only after they were dropped. Data for a
// serial the client is not holding is ignored. An image is shown once its
// current frame's pixels are complete.

#include "bromux/wire.h"

#include <bropty/graphics.h>
#include <bropty/row_source.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace bromux {

// ---- server side ---------------------------------------------------------------

// The body of an Op_Images for `layer`; `frames` gets every distinct frame's
// pixels it names, in order (what the client will want data for).
void write_images(wire::Writer& w, const bropty::ImageLayer& layer, int cell_width, int cell_height,
                  bool may_have_cells, std::vector<bropty::ImagePixelsPtr>& frames);
// The body of an Op_ImageData: bytes [offset, offset + n) of `px`.
void write_image_data(wire::Writer& w, const bropty::ImagePixels& px, size_t offset, size_t n);

// ---- client side ---------------------------------------------------------------

class ImageMirror {
public:
    static constexpr uint32_t kMaxSide = 16384;
    static constexpr size_t kMaxFrames = 4096;

    // An Op_Images / Op_ImageData body. False on malformed input.
    bool apply_images(wire::Reader& r);
    bool apply_data(wire::Reader& r);
    // After a frame's ops: rebuild the layer when anything shown changed.
    // True when it did.
    bool finish();
    void reset();

    // What a RowSource offers (none until the first Op_Images).
    [[nodiscard]] bropty::SourceImages source_images() const noexcept;
    [[nodiscard]] const bropty::ImageLayer& layer() const noexcept { return layer_; }
    [[nodiscard]] size_t image_count() const noexcept { return images_.size(); }
    [[nodiscard]] size_t placement_count() const noexcept { return placements_.size(); }
    // Decoded RGBA held (complete frames).
    [[nodiscard]] size_t bytes() const noexcept;

private:
    struct Entry {
        bropty::Image image;            // frames' pixels filled in by finish()
        std::vector<uint64_t> serials;  // per frame
    };
    struct Pixels {
        uint32_t width{0}, height{0};
        std::vector<uint8_t> rgba;  // while incomplete
        size_t received{0};
        bropty::ImagePixelsPtr done;
    };

    bool have_{false};
    bool dirty_{false};
    int cell_w_{0}, cell_h_{0};
    bool cells_{false};
    std::vector<Entry> images_;
    std::vector<bropty::Placement> placements_;
    std::unordered_map<uint64_t, Pixels> pixels_;  // by the server's serial
    bropty::ImageLayer layer_;
};

}  // namespace bromux
