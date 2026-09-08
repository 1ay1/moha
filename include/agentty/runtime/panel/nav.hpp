#pragma once
// agentty::ui::nav — the declarative navigation grammar for overlays.
//
// Every list-style overlay answers the same key vocabulary:
//
//   Esc → close/back     Enter → select      ↑/↓ (+ j/k) → move ±1
//   Home/End/PgUp/PgDn → jump                Backspace → filter erase
//   printable → filter type (or vim keys, when there is no filter)
//   <open-chord re-press> → toggle shut
//
// subscribe.cpp used to hand-copy that table into 15+ `on_X(ev)` switch
// functions — `on_thread_list` and `on_checkpoint_picker` were byte-level
// clones, and each new overlay re-implemented (or forgot: missing j/k,
// missing PageUp, missing toggle-close) the grammar one more time.
//
// NavSpec expresses one overlay's ENTIRE keymap as a declarative value:
// the shared grammar is translated ONCE (in `translate`), each overlay
// supplies only its Msg constructors + its genuinely unique keys via
// `extra`. A new picker gets consistent, complete navigation by filling
// in a struct — the grammar cannot drift per-overlay anymore.
//
// Design notes:
//   • Msg factories are std::function<Msg()> etc., bound per overlay in
//     subscribe.cpp. Null factory = "this overlay doesn't do that verb"
//     (e.g. the fork picker has no filter, the todo modal has no cursor).
//   • `vim_nav` — j/k (and optionally h/l) as Move/Step synonyms. ON for
//     read-only lists; OFF where printables type into a filter query
//     (j/k must remain typeable text there).
//   • `toggle_close_chord` — the overlay's own open chord (^K, ^J, ^T…)
//     closes it again. Open/close symmetry as data, not a re-derived
//     special case in every handler.
//   • `extra` runs FIRST so an overlay can override any part of the
//     shared grammar (diff-review's Enter = AcceptHunk, the result
//     card's Enter = Discard) without forking the whole table.

#include <functional>
#include <optional>

#include <maya/maya.hpp>

#include "agentty/runtime/msg.hpp"

namespace agentty::ui::nav {

// Jump targets shared by every pager. Overlays with a `Jump` msg map this
// enum onto their own `Where`; overlays without one map Page* onto ±N Move.
enum class Jump { Home, End, PageUp, PageDown };

struct NavSpec {
    // ── Shared grammar verbs (null = not supported by this overlay). ────
    std::function<Msg()>            close;       // Esc (and q where vim_nav)
    std::function<Msg()>            select;      // Enter
    std::function<Msg(int)>         move;        // ↑=-1 ↓=+1 (j/k when vim_nav)
    std::function<Msg(Jump)>        jump;        // Home/End/PgUp/PgDn
    std::function<Msg(char32_t)>    filter_ch;   // printable → query append
    std::function<Msg()>            filter_bs;   // Backspace → query erase

    // vim j/k as Move synonyms + q as close. Mutually exclusive with a
    // filter in practice (j/k/q must stay typeable when filter_ch is set —
    // translate() enforces the precedence: filter wins).
    bool vim_nav = false;

    // Page granularity for overlays that express paging as a big Move
    // (no jump factory): PgUp/PgDn become move(∓page_step).
    int page_step = 10;

    // The overlay's own open chord, as the ctrl-normalised letter ('k' for
    // ^K, 'j' for ^J, …, '/' for ^/). A re-press closes the overlay. 0 = none.
    char32_t toggle_close_chord = 0;

    // Overlay-specific keys, tried BEFORE the shared grammar so they can
    // override any verb. Receives the ctrl-normalised event view below.
    std::function<std::optional<Msg>(const maya::KeyEvent&)> extra;
};

// Normalised view of a CharKey: raw control bytes (0x01..0x1A — legacy
// terminals send Ctrl-<letter> with NO ctrl flag) are folded to the letter
// with ctrl=true, so every consumer compares against one canonical form.
struct CharView {
    char32_t c    = 0;      // normalised codepoint (raw ctrl → letter)
    char32_t raw  = 0;      // original codepoint
    bool     ctrl = false;  // ev.mods.ctrl OR raw control byte
};

[[nodiscard]] inline std::optional<CharView> char_view(const maya::KeyEvent& ev) noexcept {
    const auto* ck = std::get_if<maya::CharKey>(&ev.key);
    if (!ck) return std::nullopt;
    CharView v;
    v.raw = ck->codepoint;
    v.c   = v.raw;
    const bool raw_ctrl = (v.raw >= 0x01 && v.raw <= 0x1A);
    if (raw_ctrl) v.c = U'a' + (v.raw - 1);
    v.ctrl = ev.mods.ctrl || raw_ctrl;
    return v;
}

// Translate one key event through the spec. Returns nullopt when the key
// means nothing to this overlay (callers decide whether that falls through
// to global handling or is swallowed).
[[nodiscard]] inline std::optional<Msg> translate(const NavSpec& s,
                                                  const maya::KeyEvent& ev) {
    using maya::SpecialKey;

    // 1. Overlay-specific keys first — they may override the shared verbs.
    if (s.extra)
        if (auto r = s.extra(ev)) return r;

    // 2. Special keys — the uniform core of the grammar.
    if (const auto* sk = std::get_if<SpecialKey>(&ev.key)) {
        switch (*sk) {
            case SpecialKey::Escape:
                if (s.close) return s.close();
                break;
            case SpecialKey::Enter:
                if (s.select) return s.select();
                break;
            case SpecialKey::Up:
                if (s.move) return s.move(-1);
                break;
            case SpecialKey::Down:
                if (s.move) return s.move(+1);
                break;
            case SpecialKey::Backspace:
                if (s.filter_bs) return s.filter_bs();
                break;
            case SpecialKey::Home:
                if (s.jump) return s.jump(Jump::Home);
                if (s.move) return s.move(-1000000);
                break;
            case SpecialKey::End:
                if (s.jump) return s.jump(Jump::End);
                if (s.move) return s.move(+1000000);
                break;
            case SpecialKey::PageUp:
                if (s.jump) return s.jump(Jump::PageUp);
                if (s.move) return s.move(-s.page_step);
                break;
            case SpecialKey::PageDown:
                if (s.jump) return s.jump(Jump::PageDown);
                if (s.move) return s.move(+s.page_step);
                break;
            default: break;
        }
        return std::nullopt;
    }

    // 3. Character keys.
    const auto v = char_view(ev);
    if (!v) return std::nullopt;

    // Open-chord re-press → toggle shut (before anything can type it).
    if (v->ctrl && s.toggle_close_chord && s.close) {
        const char32_t lo = s.toggle_close_chord;
        const char32_t up = (lo >= U'a' && lo <= U'z')
                          ? lo - (U'a' - U'A') : lo;
        // ^/ arrives as raw 0x1F with no ctrl flag; char_view doesn't fold
        // it (it's not in 0x01..0x1A), so match the raw byte too.
        if (v->c == lo || v->c == up
            || (lo == U'/' && v->raw == 0x1F))
            return s.close();
    }

    // Filter wins over vim nav: printables type into the query.
    if (s.filter_ch && !v->ctrl && v->raw >= 0x20)
        return s.filter_ch(v->raw);

    if (s.vim_nav && !v->ctrl) {
        switch (v->c) {
            case U'k': case U'K': if (s.move)  return s.move(-1); break;
            case U'j': case U'J': if (s.move)  return s.move(+1); break;
            case U'q': case U'Q': if (s.close) return s.close();  break;
            default: break;
        }
    }
    return std::nullopt;
}

} // namespace agentty::ui::nav

namespace agentty::ui::panel {

// The message that closes an overlay of this Kind.
//
// The BACKSTOP for the escape guarantee (see subscribe.cpp). Every exclusive
// overlay swallows unclaimed keys — that is what makes it modal — so a handler
// that fails to answer Esc strands the user with no way out and the app looks
// frozen. The Retrieval pane shipped exactly that bug.
//
// Rather than trusting fifteen handlers to each get Esc right forever, the
// dispatcher falls back to this. A modal can be buggy, half-built, or brand
// new and STILL never trap the user.
//
// Exhaustive on Kind (-Wswitch), so adding an overlay without naming its close
// message is a compile warning rather than a modal you cannot leave — the
// guarantee must not depend on anyone remembering it.
[[nodiscard]] inline Msg close_msg(Kind k) noexcept {
    switch (k) {
        case Kind::Login:           return Msg{CloseLogin{}};
        case Kind::Permission:      return Msg{PermissionReject{}};
        case Kind::Palette:  return Msg{ClosePalette{}};
        case Kind::Mention:         return Msg{CloseMention{}};
        case Kind::Symbol:          return Msg{CloseSymbol{}};
        case Kind::CodeBlocks:      return Msg{CloseCodeBlocks{}};
        case Kind::CodeBlockResult: return Msg{CloseCodeBlocks{}};
        case Kind::ToolOutput:      return Msg{CloseToolOutput{}};
        case Kind::Checkpoints:     return Msg{CloseCheckpoints{}};
        case Kind::Rag:     return Msg{CloseRag{}};
        case Kind::SettingsList:    return Msg{CloseSettingsList{}};
        case Kind::PluginEdit:      return Msg{ClosePluginEdit{}};
        case Kind::Fork:            return Msg{CloseFork{}};
        case Kind::Models:     return Msg{CloseModels{}};
        case Kind::Providers:  return Msg{CloseProviders{}};
        case Kind::ThreadList:      return Msg{CloseThreadList{}};
        case Kind::SmartMode:       return Msg{CloseSmartMode{}};
        case Kind::DiffReview:      return Msg{CloseDiffReview{}};
        // Ambient: never swallows, so it never needs rescuing.
        case Kind::Todo:            return Msg{NoOp{}};
        case Kind::None:            return Msg{NoOp{}};
    }
    return Msg{NoOp{}};
}

} // namespace agentty::ui::panel
