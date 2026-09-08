#pragma once
// image_dims — read an image's PIXEL dimensions straight from its header bytes,
// with no decode and no external codec. Header-only so both the wire layer and
// the composer can consult it. Returns {0,0} for an unrecognised/truncated
// header (callers treat unknown as "can't tell → allow", since a guess-block
// would be worse than the occasional provider 400).
//
// Why this exists: Anthropic 400s a whole turn when ANY image in a many-image
// request exceeds 2000 px on a side ("image dimensions exceed max allowed
// size"). A tiny 48 KB screenshot can easily be 3000+ px wide (flat UI
// compresses small), so file size is no guard — we must read real dimensions.

#include <cstddef>
#include <string_view>

namespace agentty::util {

struct ImageDims {
    unsigned w = 0;
    unsigned h = 0;
    [[nodiscard]] bool known() const noexcept { return w != 0 && h != 0; }
    [[nodiscard]] unsigned longest() const noexcept { return w > h ? w : h; }
};

// Parse dimensions from the leading bytes of a PNG / GIF / JPEG / WebP. The
// same format set sniff_image_media_type recognises. Never throws; bounds-
// checked against `bytes.size()` at every access.
[[nodiscard]] inline ImageDims image_dimensions(std::string_view bytes) noexcept {
    const auto* p = reinterpret_cast<const unsigned char*>(bytes.data());
    const std::size_t n = bytes.size();
    auto be16 = [&](std::size_t i) -> unsigned {
        return (unsigned(p[i]) << 8) | p[i + 1];
    };
    auto be32 = [&](std::size_t i) -> unsigned {
        return (unsigned(p[i]) << 24) | (unsigned(p[i + 1]) << 16)
             | (unsigned(p[i + 2]) << 8) | p[i + 3];
    };
    auto le16 = [&](std::size_t i) -> unsigned {
        return unsigned(p[i]) | (unsigned(p[i + 1]) << 8);
    };

    // PNG: 8-byte signature, then the IHDR chunk — W/H big-endian at 16/20.
    if (n >= 24 && p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G')
        return {be32(16), be32(20)};

    // GIF: logical screen width/height, little-endian at offset 6.
    if (n >= 10 && p[0] == 'G' && p[1] == 'I' && p[2] == 'F')
        return {le16(6), le16(8)};

    // JPEG: walk segment markers to a Start-Of-Frame (SOFn); its payload is
    // [precision(1)][height(2 BE)][width(2 BE)] after the 2-byte length.
    if (n >= 4 && p[0] == 0xFF && p[1] == 0xD8) {
        std::size_t i = 2;
        while (i + 9 < n) {
            if (p[i] != 0xFF) { ++i; continue; }
            const unsigned marker = p[i + 1];
            if (marker == 0xD8 || marker == 0xD9) { i += 2; continue; }
            if (marker >= 0xD0 && marker <= 0xD7) { i += 2; continue; } // RSTn
            const unsigned seg_len = be16(i + 2);
            if (seg_len < 2) break;
            // SOF0..SOF15 carry frame dims; skip DHT(C4)/JPG(C8)/DAC(CC).
            if (marker >= 0xC0 && marker <= 0xCF
                && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
                if (i + 9 < n) return {be16(i + 7), be16(i + 5)}; // {w, h}
            }
            i += 2 + seg_len;
        }
    }

    // WebP: "RIFF"????"WEBP" then a VP8 / VP8L / VP8X chunk.
    if (n >= 30 && p[0] == 'R' && p[1] == 'I' && p[2] == 'F' && p[3] == 'F'
        && p[8] == 'W' && p[9] == 'E' && p[10] == 'B' && p[11] == 'P') {
        // Lossy VP8: 14-bit LE width/height at offset 26/28.
        if (p[12] == 'V' && p[13] == 'P' && p[14] == '8' && p[15] == ' ')
            return {le16(26) & 0x3FFF, le16(28) & 0x3FFF};
        // Extended VP8X: 24-bit LE (w-1)/(h-1) at offset 24/27.
        if (p[12] == 'V' && p[13] == 'P' && p[14] == '8' && p[15] == 'X') {
            const unsigned w = (unsigned(p[24]) | (unsigned(p[25]) << 8)
                              | (unsigned(p[26]) << 16)) + 1;
            const unsigned h = (unsigned(p[27]) | (unsigned(p[28]) << 8)
                              | (unsigned(p[29]) << 16)) + 1;
            return {w, h};
        }
        // Lossless VP8L: 14-bit packed dims — best-effort, treated as unknown.
    }
    return {};
}

// Per-side pixel ceilings, straight from Anthropic's vision docs.
//
// The HARD limit is 8000 px: beyond that the API rejects the image. Below it
// an oversized image is DOWNSCALED server-side (to the model's long-edge tier,
// 1568 or 2576 px) and answered normally — it is not an error.
//
// 2000 px is a different, CONDITIONAL rule: it applies only to a "many-image"
// request, i.e. more than 20 image/document blocks in one request. Under that
// threshold a 2168 px screenshot is perfectly valid.
//
// Conflating the two cost real user data: every image over 2000 px on a side
// was silently discarded, which is most modern screenshots (a 2168x748 window
// grab exceeded it by 168 px), while the prose marker still went out — so the
// model was told an image was present and shown nothing. Send by default;
// only apply the stricter ceiling when the request actually is many-image.
inline constexpr unsigned kMaxWireImageSide      = 8000;
inline constexpr unsigned kManyImageMaxSide      = 2000;
inline constexpr std::size_t kManyImageThreshold = 20;

// True iff the image is small enough to send. Unknown dimensions (unparsed
// header) pass — we don't block on a guess; the provider's own limit is the
// backstop for the rare format we can't read.
[[nodiscard]] inline bool image_within_wire_limits(std::string_view bytes) noexcept {
    const ImageDims d = image_dimensions(bytes);
    if (!d.known()) return true;
    return d.longest() <= kMaxWireImageSide;
}

} // namespace agentty::util
