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

// ── The MANY-IMAGE ceiling ──────────────────────────────────────────────────
//
// The 2000 px cap is real, but conditional: it applies only once a request
// carries many image blocks. Both constants existed with no call site at all
// for a while — the loose cap shipped unconditionally and a 20-image request
// would have been 400'd by the provider with no local trace. These lock the
// rule to the two things that can silently drift: the threshold arithmetic,
// and the fact that the count spans BOTH sources of image blocks.
TEST_CASE("wire image cap tightens on a many-image request") {
    using agentty::util::kManyImageMaxSide;
    using agentty::util::kManyImageThreshold;
    using agentty::util::kMaxWireImageSide;
    using agentty::util::wire_max_side;

    // The threshold boundary itself — off-by-one here silently disables the
    // rule for the exact request size it exists to protect.
    check(wire_max_side(0) == kMaxWireImageSide, "no images → loose cap");
    check(wire_max_side(kManyImageThreshold - 1) == kMaxWireImageSide,
          "just under the threshold → still the loose cap");
    check(wire_max_side(kManyImageThreshold) == kManyImageMaxSide,
          "at the threshold → the strict 2000 px cap");
    check(wire_max_side(kManyImageThreshold + 5) == kManyImageMaxSide,
          "well past the threshold → strict cap");

    // The SAME image is sendable alone and dropped in a many-image request.
    // This is the whole point of making the cap a parameter: sendability is
    // not a property of the picture, it's a property of the request.
    {
        ImageContent img{"image/png", png_with_dims(2168, 748)};
        check(wire::wire_image_sendable(img, kMaxWireImageSide),
              "2168 px screenshot ships in an ordinary request");
        check(!wire::wire_image_sendable(img, kManyImageMaxSide),
              "the same screenshot is dropped once the request is many-image");
    }

    // The count spans user images AND tool_result images. A session that
    // READS twenty screenshots hits the provider's limit exactly like one
    // that pastes twenty, so counting only user turns would miss it.
    {
        std::vector<agentty::Message> msgs;
        agentty::Message user;
        user.role = agentty::Role::User;
        user.images.push_back({"image/png", png_with_dims(64, 64)});
        user.images.push_back({"image/png", ""});   // empty: never counted
        msgs.push_back(std::move(user));
        check(wire::wire_image_count(msgs) == 1,
              "empty bytes don't count toward the many-image threshold");

        agentty::Message asst;
        asst.role = agentty::Role::Assistant;
        agentty::ToolUse tc;
        tc.status = agentty::ToolUse::Done{
            .images = {{"image/png", png_with_dims(64, 64)},
                       {"image/png", png_with_dims(64, 64)}}};
        asst.tool_calls.push_back(std::move(tc));
        msgs.push_back(std::move(asst));
        check(wire::wire_image_count(msgs) == 3,
              "tool_result images count toward the threshold too");
    }
}
