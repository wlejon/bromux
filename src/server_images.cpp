// The minor-1 parts of a frame: OSC 133 command records (Op_Commands) and
// inline images (Op_Images, Op_ImageData; image_codec.h). Only for clients
// whose Hello said minor >= 1 (send_frame asks).
#include "bromux/image_codec.h"
#include "server_impl.h"

#include <bropty/selection.h>

#include <algorithm>

namespace bromux::detail {

namespace {

// Pixel bytes one frame carries at most; the rest follows in later frames
// (paced and acknowledged like any frame), so a large image never holds a
// client's screen back by more than this.
constexpr size_t kImageDataPerFrame = 1u << 20;

// A record as the client gets it: with the command line a shell that does
// not report one typed between the end of its prompt (B) and its output (C),
// as the text there reads.
bropty::CommandRecord with_command_line(const bropty::CommandRecord& c, bropty::Terminal& t) {
    bropty::CommandRecord out = c;
    if (out.command_line.empty() && c.input && c.output && !c.trimmed && *c.input < *c.output) {
        bropty::Selection probe(t);
        probe.select_range(bropty::RowRange{*c.input, *c.output});
        const std::string text = probe.text();
        const size_t a = text.find_first_not_of(" \t\r\n");
        const size_t b = text.find_last_not_of(" \t\r\n");
        out.command_line = a == std::string::npos ? std::string() : text.substr(a, b - a + 1);
    }
    return out;
}

}  // namespace

// The records changed since the client's copy: those dropped from the front
// (capacity), the run it still has right, and the rest anew. Records are
// compared as the terminal holds them; the command lines read from the
// screen are filled in only for what is sent.
void ServerCore::write_commands(Attachment& a, wire::Writer& w) {
    bropty::Terminal& t = a.session->t();
    if (a.sent_commands_version == t.commands_version()) return;
    a.sent_commands_version = t.commands_version();
    const std::vector<bropty::CommandRecord>& cur = t.commands();
    const std::vector<bropty::CommandRecord>& old = a.sent_commands;
    size_t drop = 0;
    if (!old.empty() && !cur.empty() && !same_command(old[0], cur[0])) {
        // The oldest went (the list is full): find where the new list starts.
        auto it = std::find_if(old.begin(), old.end(),
                               [&](const bropty::CommandRecord& c) { return same_command(c, cur[0]); });
        drop = it == old.end() ? old.size() : size_t(it - old.begin());
    }
    size_t keep = 0;
    while (drop + keep < old.size() && keep < cur.size() && same_command(old[drop + keep], cur[keep])) ++keep;
    if (drop == 0 && keep == old.size() && keep == cur.size()) return;  // nothing the client sees changed
    w.u8(Op_Commands);
    w.varint(drop);
    w.varint(keep);
    w.varint(cur.size() - keep);
    for (size_t i = keep; i < cur.size(); ++i) write_command(w, with_command_line(cur[i], t));
    a.sent_commands = cur;
}

bool ServerCore::extras_due(const Attachment& a) {
    if (!a.minor1()) return false;
    if (a.images_pending()) return true;
    bropty::Terminal& t = a.session->t();
    return a.sent_images_version != t.images_version() || a.sent_images_alt != t.alt_screen_active() ||
           a.sent_image_cell_w != t.image_cell_width() || a.sent_image_cell_h != t.image_cell_height();
}

void ServerCore::write_images_ops(Attachment& a, wire::Writer& w) {
    bropty::Terminal& t = a.session->t();
    const bool alt = t.alt_screen_active();
    const bool changed = a.sent_images_version != t.images_version() || a.sent_images_alt != alt ||
                         a.sent_image_cell_w != t.image_cell_width() || a.sent_image_cell_h != t.image_cell_height();
    if (changed) {
        const bool first = a.sent_images_version == UINT64_MAX;
        a.sent_images_version = t.images_version();
        a.sent_images_alt = alt;
        a.sent_image_cell_w = t.image_cell_width();
        a.sent_image_cell_h = t.image_cell_height();
        const bropty::ImageLayer& layer = t.images();
        // A terminal that never had an image sends nothing at attach.
        const bool empty = layer.image_count() == 0 && layer.placements().empty();
        if (!(first && empty && !t.may_have_image_cells())) {
            std::vector<bropty::ImagePixelsPtr> frames;
            w.u8(Op_Images);
            write_images(w, layer, t.image_cell_width(), t.image_cell_height(), t.may_have_image_cells(), frames);
            // What the client holds from now on: the frames listed, with the
            // bytes it already has of each; everything else it dropped.
            std::unordered_map<uint64_t, size_t> sent;
            for (const bropty::ImagePixelsPtr& px : frames) {
                auto it = a.image_sent.find(px->serial);
                sent.emplace(px->serial, it == a.image_sent.end() ? 0 : it->second);
            }
            a.image_sent = std::move(sent);
            a.image_frames = std::move(frames);
            a.image_next = 0;
        }
    }
    // Pixels owed, in list order, up to this frame's share.
    size_t budget = kImageDataPerFrame;
    while (a.image_next < a.image_frames.size() && budget > 0) {
        const bropty::ImagePixels& px = *a.image_frames[a.image_next];
        size_t& done = a.image_sent[px.serial];
        const size_t total = px.rgba.size();
        if (done >= total) {
            ++a.image_next;
            continue;
        }
        const size_t n = std::min(budget, total - done);
        w.u8(Op_ImageData);
        write_image_data(w, px, done, n);
        done += n;
        budget -= n;
        if (done >= total) ++a.image_next;
    }
}

}  // namespace bromux::detail
