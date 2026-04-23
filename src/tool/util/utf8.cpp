#include "moha/tool/util/utf8.hpp"

#include <cstdint>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace moha::tools::util {

bool is_valid_utf8(std::string_view s) noexcept {
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { ++i; continue; }
        int extra; unsigned char mask; uint32_t min_cp;
        if      ((c & 0xE0) == 0xC0) { extra = 1; mask = 0x1F; min_cp = 0x80; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; mask = 0x0F; min_cp = 0x800; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; mask = 0x07; min_cp = 0x10000; }
        else return false;
        if (i + (size_t)extra >= s.size()) return false;
        uint32_t cp = c & mask;
        for (int k = 1; k <= extra; ++k) {
            unsigned char d = (unsigned char)s[i + (size_t)k];
            if ((d & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (d & 0x3F);
        }
        if (cp < min_cp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += (size_t)extra + 1;
    }
    return true;
}

std::string sanitize_utf8(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    auto repl = [&]{ out.append("\xEF\xBF\xBD"); };
    size_t i = 0;
    while (i < in.size()) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x80) { out.push_back((char)c); ++i; continue; }
        int extra; unsigned char mask; uint32_t min_cp;
        if      ((c & 0xE0) == 0xC0) { extra = 1; mask = 0x1F; min_cp = 0x80; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; mask = 0x0F; min_cp = 0x800; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; mask = 0x07; min_cp = 0x10000; }
        else { repl(); ++i; continue; }
        if (i + (size_t)extra >= in.size()) { repl(); ++i; continue; }
        uint32_t cp = c & mask;
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            unsigned char d = (unsigned char)in[i + (size_t)k];
            if ((d & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (d & 0x3F);
        }
        if (!ok || cp < min_cp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            repl(); ++i; continue;
        }
        out.append(in.data() + i, (size_t)(extra + 1));
        i += (size_t)extra + 1;
    }
    return out;
}

#ifdef _WIN32
// Transcode from a Windows code page (OEM console output / CP1252 / etc.)
// to UTF-8 via the wide-char pivot. Returns empty on any API failure so
// callers can fall back to the raw bytes + scrub path.
static std::string windows_cp_to_utf8(std::string_view s, UINT cp) {
    if (s.empty()) return {};
    int wlen = ::MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), nullptr, 0);
    if (wlen <= 0) return {};
    std::wstring w((size_t)wlen, L'\0');
    ::MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), w.data(), wlen);
    int u8 = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, nullptr, 0, nullptr, nullptr);
    if (u8 <= 0) return {};
    std::string out((size_t)u8, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, out.data(), u8, nullptr, nullptr);
    return out;
}
#endif

std::size_t safe_utf8_cut(std::string_view s, std::size_t max_bytes) noexcept {
    if (s.size() <= max_bytes) return s.size();
    // We want the largest `cut <= max_bytes` such that s[0, cut) ends on a
    // UTF-8 code-point boundary. That means s[cut] (the first excluded byte)
    // must NOT be a continuation byte — if it were, the lead for its sequence
    // would live in [0, cut) and we'd be splitting it.
    //
    // Start at max_bytes and walk back while we're *on* a continuation byte
    // (0x80..0xBF — top bits 10xxxxxx). In valid UTF-8 at most 3 steps back
    // (4-byte sequences are the widest). After the loop, s[cut] is either
    // past-the-end, a lead byte, or ASCII — all safe cut points.
    std::size_t cut = max_bytes;
    while (cut > 0 && cut < s.size() && ((unsigned char)s[cut] & 0xC0) == 0x80)
        --cut;
    return cut;
}

std::string to_valid_utf8(std::string s) {
    if (is_valid_utf8(s)) return s;
#ifdef _WIN32
    if (UINT cp = ::GetConsoleOutputCP(); cp != 0 && cp != CP_UTF8) {
        auto converted = windows_cp_to_utf8(s, cp);
        if (!converted.empty() && is_valid_utf8(converted)) return converted;
    }
    {
        auto converted = windows_cp_to_utf8(s, CP_ACP);
        if (!converted.empty() && is_valid_utf8(converted)) return converted;
    }
#endif
    return sanitize_utf8(s);
}

} // namespace moha::tools::util
