// The server's decoder for compressed inline images (ServerOptions::
// decode_image): broimage when the build found it, else none.
#include "cli.h"

#if defined(BROMUX_HAVE_BROIMAGE)
#include <broimage/decode.h>

#include <algorithm>
#include <limits>
#endif

namespace bromux::cli {

#if defined(BROMUX_HAVE_BROIMAGE)

namespace {

bool decode(std::string_view data, const bropty::ImageLimits& limits, bropty::DecodedImage& out) {
    // bropty's zero means "nothing fits", broimage's "no limit".
    if (limits.max_width == 0 || limits.max_height == 0 || limits.max_bytes == 0) return false;
    broimage::DecodeLimits bl;
    bl.max_width = int(std::min<uint32_t>(limits.max_width, uint32_t(std::numeric_limits<int>::max())));
    bl.max_height = int(std::min<uint32_t>(limits.max_height, uint32_t(std::numeric_limits<int>::max())));
    bl.max_bytes = limits.max_bytes;
    broimage::Animation a;
    if (!broimage::decode_memory_bounded(reinterpret_cast<const uint8_t*>(data.data()), data.size(), bl, a))
        return false;
    out.width = uint32_t(a.width);
    out.height = uint32_t(a.height);
    out.frames.clear();
    out.frames.reserve(a.frames.size());
    for (broimage::AnimationFrame& f : a.frames) {
        bropty::DecodedFrame df;
        df.rgba = std::move(f.rgba);
        df.delay_ms = uint32_t(std::max(0, f.delay_ms));
        out.frames.push_back(std::move(df));
    }
    return !out.frames.empty();
}

}  // namespace

ImageDecoder image_decoder() { return decode; }

#else

ImageDecoder image_decoder() { return {}; }

#endif

}  // namespace bromux::cli
