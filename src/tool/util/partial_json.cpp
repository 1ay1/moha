#include "moha/tool/util/partial_json.hpp"

#include <cstring>
#include <vector>

namespace moha::tools::util {

namespace {

// Is the character at `i` a value-position-follower that we can safely emit
// `null` after if the value never arrived (e.g. `"k":` at EOF)?
bool at_awaiting_value(const std::string& out) {
    // Walk back past whitespace; if we land on ':' then we're waiting for a
    // value. If we land on '{' or ',' we're waiting for a key (handled
    // separately by the caller — we strip the trailing comma instead).
    for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(out.size()) - 1;
         i >= 0; --i) {
        char c = out[static_cast<std::size_t>(i)];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        return c == ':';
    }
    return false;
}

// Remove the last non-whitespace char if it is `,` — a dangling comma inside
// an object/array isn't valid JSON and we can simply forget it.
void strip_trailing_comma(std::string& out) {
    for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(out.size()) - 1;
         i >= 0; --i) {
        char c = out[static_cast<std::size_t>(i)];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        if (c == ',') {
            out.erase(static_cast<std::size_t>(i));
        }
        return;
    }
}

} // namespace

std::string close_partial_json(std::string_view raw) {
    std::string out;
    out.reserve(raw.size() + 16);

    // State machine: we need to know whether we're inside a string (and
    // whether the previous char was the start of an escape), plus a stack of
    // open containers so we can close them in reverse order.
    std::vector<char> stack;   // '{' or '['
    bool in_string = false;
    bool escape = false;       // previous char was `\` inside a string

    for (char c : raw) {
        if (in_string) {
            if (escape) {
                // Keep the escape pair as-is; whatever byte follows `\` is
                // simply kept. JSON accepts `\u` / `\n` / `\"` / etc.
                out.push_back(c);
                escape = false;
                continue;
            }
            if (c == '\\') { out.push_back(c); escape = true; continue; }
            if (c == '"')  { out.push_back(c); in_string = false; continue; }
            out.push_back(c);
            continue;
        }

        switch (c) {
            case '"':
                out.push_back(c);
                in_string = true;
                break;
            case '{': case '[':
                stack.push_back(c);
                out.push_back(c);
                break;
            case '}':
                if (!stack.empty() && stack.back() == '{') stack.pop_back();
                out.push_back(c);
                break;
            case ']':
                if (!stack.empty() && stack.back() == '[') stack.pop_back();
                out.push_back(c);
                break;
            default:
                out.push_back(c);
                break;
        }
    }

    // Finalize tail.

    if (in_string) {
        // Drop a trailing lone `\` — it would force the next byte to be
        // interpreted as an escape pair, but there is no next byte.
        if (escape) out.pop_back();
        out.push_back('"');
    }

    // Now we're outside any string. Close containers in reverse.
    while (!stack.empty()) {
        char open = stack.back();
        stack.pop_back();

        // If the container is waiting for a value after `:`, supply `null`
        // so the pair is well-formed. Otherwise strip a dangling comma.
        if (at_awaiting_value(out)) {
            out.append("null");
        } else {
            strip_trailing_comma(out);
        }

        out.push_back(open == '{' ? '}' : ']');
    }

    // Top-level guards: if the buffer is empty or ends with just `:` / `,`
    // at top level (shouldn't happen for well-formed tool-use, but be safe),
    // produce `null` so callers always get parseable output.
    if (out.empty()) return "null";
    // Top-level dangling `:` can happen if a scalar was promised but never
    // arrived (edge case — defensive).
    if (at_awaiting_value(out)) out.append("null");

    return out;
}

namespace {

// Shared prefix walk for sniff_*: advance past `"key"` followed by `:` and
// the opening `"` of its value. Returns the index just past the opening
// quote, or std::string_view::npos if the expected prefix isn't there yet.
//
// Perf note: this runs on the streaming hot path (called per alias per
// preview tick, ~8x/sec during a turn). The previous implementation built
// a `"key"` needle as a heap-allocated std::string and handed it to
// std::string_view::find — O(N) scan per call, plus a malloc+free every
// call. We now scan the buffer once looking for the opening `"`, then
// memcmp against the key. Zero allocations; the inner loop is a single
// byte compare that usually fails on the first char.
std::size_t sniff_prefix(std::string_view raw, std::string_view key) {
    if (key.empty() || raw.size() < key.size() + 4) return std::string_view::npos;
    const char* const base = raw.data();
    const std::size_t n    = raw.size();
    const std::size_t klen = key.size();
    // Last index where `"key"` could still begin: need klen+2 bytes
    // for the quoted key plus at least `:` `"` after it.
    const std::size_t last_start = n - (klen + 3);
    for (std::size_t i = 0; i <= last_start; ++i) {
        if (base[i] != '"') continue;
        if (base[i + klen + 1] != '"') continue;
        if (std::memcmp(base + i + 1, key.data(), klen) != 0) continue;
        std::size_t p = i + klen + 2;
        while (p < n && (base[p] == ' ' || base[p] == '\t' || base[p] == '\n')) ++p;
        if (p >= n || base[p] != ':') continue;
        ++p;
        while (p < n && (base[p] == ' ' || base[p] == '\t' || base[p] == '\n')) ++p;
        if (p >= n || base[p] != '"') continue;
        return p + 1;
    }
    return std::string_view::npos;
}

// Apply a JSON escape (`\X`) onto the output buffer. Unknown escapes
// degrade to the literal byte — fine for streaming previews, where we
// prefer a benign display over an exception.
void apply_escape(char n, std::string& out) {
    switch (n) {
        case 'n':  out += '\n'; break;
        case 't':  out += '\t'; break;
        case 'r':  out += '\r'; break;
        case '"':  out += '"';  break;
        case '\\': out += '\\'; break;
        case '/':  out += '/';  break;
        case 'b':  out += '\b'; break;
        case 'f':  out += '\f'; break;
        default:   out += n;    break;
    }
}

} // namespace

std::optional<std::string>
sniff_string(std::string_view raw, std::string_view key) {
    std::size_t p = sniff_prefix(raw, key);
    if (p == std::string_view::npos) return std::nullopt;
    std::string out;
    out.reserve(64);
    while (p < raw.size()) {
        char c = raw[p];
        if (c == '\\') {
            if (p + 1 >= raw.size()) return std::nullopt; // wait for next delta
            apply_escape(raw[p + 1], out);
            p += 2;
        } else if (c == '"') {
            return out;
        } else {
            out.push_back(c);
            ++p;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t>
locate_string_value(std::string_view raw, std::string_view key) {
    std::size_t p = sniff_prefix(raw, key);
    if (p == std::string_view::npos) return std::nullopt;
    return p;
}

std::string
decode_string_from(std::string_view raw, std::size_t offset) {
    std::string out;
    if (offset >= raw.size()) return out;
    out.reserve(raw.size() - offset);
    std::size_t p = offset;
    while (p < raw.size()) {
        char c = raw[p];
        if (c == '\\') {
            if (p + 1 >= raw.size()) break; // half-escape at buffer edge
            apply_escape(raw[p + 1], out);
            p += 2;
        } else if (c == '"') {
            return out;
        } else {
            out.push_back(c);
            ++p;
        }
    }
    return out;
}

std::optional<std::string>
sniff_string_progressive(std::string_view raw, std::string_view key) {
    auto off = locate_string_value(raw, key);
    if (!off) return std::nullopt;
    return decode_string_from(raw, *off);
}

} // namespace moha::tools::util
