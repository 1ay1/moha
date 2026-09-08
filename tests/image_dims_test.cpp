// image_dims_test — pins the pixel-dimension gate that keeps an oversized
// image off the wire (Anthropic 400s a many-image turn if any image exceeds
// 2000 px/side). Covers the byte-header dimension reader AND the wire gate
// (wire_image_sendable), which is the SSOT every dialect's image selector uses.

#include "agtest.hpp"

#include "agentty/domain/conversation.hpp"
#include "agentty/provider/msg_shared.hpp"
#include "agentty/util/image_dims.hpp"

#include <string>

using agentty::ImageContent;
using agentty::util::image_dimensions;
namespace wire = agentty::provider::wire;

namespace {
// A minimal PNG with a given width/height in the IHDR (all this test's readers
// look at). 8-byte sig + IHDR length/type + W/H big-endian, then filler.
std::string png_with_dims(unsigned w, unsigned h) {
    std::string b;
    const unsigned char sig[8] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    b.append(reinterpret_cast<const char*>(sig), 8);
    b.append("\x00\x00\x00\x0D", 4);      // IHDR length = 13
    b.append("IHDR", 4);
    auto be32 = [&](unsigned v) {
        b.push_back(char((v >> 24) & 0xFF));
        b.push_back(char((v >> 16) & 0xFF));
        b.push_back(char((v >> 8) & 0xFF));
        b.push_back(char(v & 0xFF));
    };
    be32(w); be32(h);
    b.append(64, '\x00');                 // filler body
    return b;
}
} // namespace

TEST_CASE("image dimension reader") {
    auto d1 = image_dimensions(png_with_dims(1920, 1080));
    check(d1.known() && d1.w == 1920 && d1.h == 1080, "PNG dims parsed");
    check(d1.longest() == 1920, "longest side = width here");

    auto d2 = image_dimensions(png_with_dims(3440, 200));
    check(d2.w == 3440 && d2.longest() == 3440, "wide thin PNG parsed");

    // Truncated / non-image → unknown (never crashes, never false-positives).
    check(!image_dimensions(std::string("not an image")).known(),
          "garbage bytes → unknown dims");
    check(!image_dimensions(std::string{}).known(), "empty → unknown");
}

TEST_CASE("wire drops oversized images, keeps in-range ones") {
    // In-range: a normal screenshot is sendable.
    {
        ImageContent img;
        img.media_type = "image/png";
        img.bytes = png_with_dims(1600, 900);
        check(wire::wire_image_sendable(img),
              "1600x900 is well under the cap → sendable");
    }
    // A hi-DPI capture MUST reach the model. Anthropic downscales anything
    // over the model's long-edge tier server-side and answers normally — it
    // is not an error. The old 2000 px ceiling was the MANY-image rule (>20
    // blocks in one request) misapplied to every image, so ordinary
    // screenshots were discarded while the "[image: ...]" marker still went
    // out: the model was told an image was present and shown nothing.
    {
        ImageContent img;
        img.media_type = "image/png";
        img.bytes = png_with_dims(3024, 1200);
        check(wire::wire_image_sendable(img),
              "3024 px retina capture is sent, not dropped");
    }
    {
        // The exact paste this was found on: a 2168x748 window grab, 168 px
        // over the old ceiling, silently discarded.
        ImageContent img{"image/png", png_with_dims(2168, 748)};
        check(wire::wire_image_sendable(img),
              "2168x748 window grab is sent");
    }
    // The real boundary is the API's hard limit: at it, allowed; past it,
    // refused (the request would be rejected outright).
    {
        ImageContent at{"image/png",
                        png_with_dims(agentty::util::kMaxWireImageSide,
                                      agentty::util::kMaxWireImageSide)};
        ImageContent over{"image/png",
                          png_with_dims(agentty::util::kMaxWireImageSide + 1, 10)};
        check(wire::wire_image_sendable(at),    "8000 px is allowed (== cap)");
        check(!wire::wire_image_sendable(over), "8001 px is rejected");
    }
    // Empty bytes still rejected (unchanged behaviour).
    {
        ImageContent img; img.media_type = "image/png";
        check(!wire::wire_image_sendable(img), "empty bytes → not sendable");
    }
    // Unknown format (can't read dims) is allowed through — we don't block on
    // a guess; the provider's own limit is the backstop.
    {
        ImageContent img;
        img.media_type = "image/png";
        img.bytes = std::string("\xff\xd8\xffno-valid-sof-here", 18);
        check(wire::wire_image_sendable(img),
              "unreadable dims → allowed (provider is the backstop)");
    }
}
