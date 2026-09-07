#include "agentty/runtime/app/subscribe.hpp"

#include <chrono>
#include <cstdlib>
#include <optional>
#include <variant>

#include <maya/terminal/ansi.hpp>

#include "agentty/runtime/login.hpp"
#include "agentty/runtime/panel/common.hpp"
#include "agentty/runtime/panel/top.hpp"
#include "agentty/runtime/panel/nav.hpp"
#include "agentty/runtime/app/update/internal.hpp"

namespace pn = agentty::ui::panel;

namespace agentty::app {

using maya::Sub;
using maya::KeyEvent;
using maya::CharKey;
using maya::SpecialKey;
namespace pick = agentty::ui::pick;
namespace nav  = agentty::ui::nav;

namespace {

// True when agentty is driven over an SSH session. Detected once from
// the env the SSH daemon exports into the remote shell. Over SSH the
// wire (not local CPU) is the render bottleneck: each streaming frame
// emits several KB of ANSI diff, and at 30 fps that saturates a
// high-latency / low-bandwidth link, producing the mid-turn lag and
// the end-of-turn catch-up repaint. We slow the streaming tick when
// remote (see the cadence block below).
// SSH detection moved to app::detail (stream.cpp) so the end-of-turn
// reveal policy can share it; see internal.hpp. This TU keeps using it
// via the detail:: declaration.

} // namespace

// The single definition of the streaming Tick cadence. See the
// declaration in subscribe.hpp for the full rationale. Computed once
// per session — the inputs are immutable — and shared verbatim by both
// subscribe() (the timer interval) and Program::visual_hash() (the
// phase-locked animation bucket).
std::chrono::milliseconds streaming_tick_period() noexcept {
    static const std::chrono::milliseconds period = [] {
        auto base = maya::ansi::env_supports_synchronized_output()
            ? std::chrono::milliseconds(33)
            : std::chrono::milliseconds(100);
        if (detail::running_over_ssh())
            return std::max(base, std::chrono::milliseconds(80));
        return base;
    }();
    return period;
}

// True when the last message still carries in-flight wire bytes
// (streaming_text / pending_stream not yet drained into the settled
// body). The Tick subscription gates the reveal-fx animation clock and
// the pending_stream→streaming_text drip; gating it ONLY on
// m.s.active() leaves a one-tick gap at the very START of a stream:
// the first 1-2 StreamTextDelta msgs paint via their own render (fps=0),
// but the animation loop + drip don't engage until the Tick timer the
// reducer just armed actually fires — a visible split-second hang after
// the first word or two. agent_session never sees this because it
// subscribes to Tick UNCONDITIONALLY (the clock is always running).
// Rather than run an always-on tick when idle (wasteful at fps=0), we
// extend the tick window to cover any frame where live bytes exist, so
// the clock is already ticking the instant the first delta lands and
// the reveal animation is continuous from byte one — same end result
// as the reference, with zero idle cost.
bool tail_has_live_bytes(const Model& m) noexcept {
    if (m.d.current.messages.empty()) return false;
    const auto& back = m.d.current.messages.back();
    return !back.streaming_text.empty() || !back.pending_stream.empty();
}

bool reveal_needs_frames(const Model& m) noexcept {
    if (!m.s.active() || m.d.current.messages.empty()) return false;
    const auto& back = m.d.current.messages.back();
    if (back.role != Role::Assistant) return false;
    return !back.streaming_text.empty()
        || !back.pending_stream.empty()
        // Pure-reasoning phase: no answer bytes yet, but the reasoning
        // channel (msg.thinking) streams through its own reveal. Omitting
        // this term is the "Thinking gets stuck" bug: the fast bucket
        // never engages, armed RAF frames get gated away, and the
        // reasoning typewriter freezes until a keypress.
        || (m.d.show_reasoning
            && !back.reasoning_display_text().empty());
}

bool reveal_draining(const Model& m) noexcept {
    return m.ui.pending_settle_freeze
        || m.ui.settle_cooldown_ticks > 0
        // True exactly while the reveal (is_live / reveal_in_progress /
        // is_finalizing / is_parsing) has NOT drained — the wall-clock
        // typewriter often has seconds of animation left after the wire
        // goes quiet, and stopping the clock here freezes it mid-glide at
        // 1% CPU until the next keystroke.
        || !detail::live_tail_reveal_settled(m);
}

bool animation_demand(const Model& m) noexcept {
    return m.s.active()
        // `models_loading` keeps the fused picker's "loading …" spinner
        // spinning while a slow backend (Ollama, a custom host) is still
        // answering — a frozen glyph is the visual signature of a hang,
        // the opposite of what it must convey.
        || m.s.models_loading
        || m.loading_spinner_visible()
        || tail_has_live_bytes(m)
        || m.ui.pending_rehydrate_trim
        // A LOOP waiting out a backoff must keep the frame clock alive: the
        // re-send fires from the settle path, and the countdown chip has to
        // tick down. Without this the app goes fully idle after a failed
        // iteration and the loop silently never resumes until a keystroke.
        || m.ui.composer.loop_wait_until_ms > 0
        || reveal_draining(m);
}

namespace {

// ── Per-modal key handlers — return std::nullopt to fall through ──────────

std::optional<Msg> on_permission(const KeyEvent& ev) {
    if (auto* ck = std::get_if<CharKey>(&ev.key)) {
        char32_t c = ck->codepoint;
        // ^C must stay quittable while a prompt is up — this handler's
        // result is returned unconditionally (never falls through to
        // on_global), so swallow-by-nullopt would make the app unquittable
        // until the prompt is answered. Legacy terminals send raw 0x03.
        if (c == 0x03 || (ev.mods.ctrl && (c == U'c' || c == U'C')))
            return Quit{};
        switch (c) {
            case 'y': case 'Y': return PermissionApprove{};
            case 'n': case 'N': return PermissionReject{};
            case 'a': case 'A': return PermissionApproveAlways{};
        }
    }
    if (std::holds_alternative<SpecialKey>(ev.key)
        && std::get<SpecialKey>(ev.key) == SpecialKey::Escape)
        return PermissionReject{};
    return std::nullopt;
}

std::optional<Msg> on_command_palette(const KeyEvent& ev) {
    // ^K palette: shared grammar + typed filter. No jump/vim (printables
    // type into the query). Ctrl chords other than the toggle are eaten —
    // they must never become query text.
    nav::NavSpec s;
    s.close     = [] { return Msg{ClosePalette{}}; };
    s.select    = [] { return Msg{PaletteSelect{}}; };
    s.move      = [](int d) { return Msg{PaletteMove{d}}; };
    s.filter_bs = [] { return Msg{PaletteBackspace{}}; };
    s.filter_ch = [](char32_t c) { return Msg{PaletteInput{c}}; };
    s.toggle_close_chord = U'k';
    return nav::translate(s, ev);
}

std::optional<Msg> on_mention_palette(const KeyEvent& ev) {
    // @-mention list rides the composer: every CharKey (even ctrl'd) goes
    // to the palette's own input handling, so `extra` claims chars first.
    nav::NavSpec s;
    s.close     = [] { return Msg{CloseMention{}}; };
    s.select    = [] { return Msg{MentionSelect{}}; };
    s.move      = [](int d) { return Msg{MentionMove{d}}; };
    s.filter_bs = [] { return Msg{MentionBackspace{}}; };
    s.extra     = [](const KeyEvent& e) -> std::optional<Msg> {
        if (auto* ck = std::get_if<CharKey>(&e.key))
            return Msg{MentionInput{ck->codepoint}};
        return std::nullopt;
    };
    return nav::translate(s, ev);
}

std::optional<Msg> on_symbol_palette(const KeyEvent& ev) {
    nav::NavSpec s;
    s.close     = [] { return Msg{CloseSymbol{}}; };
    s.select    = [] { return Msg{SymbolSelect{}}; };
    s.move      = [](int d) { return Msg{SymbolMove{d}}; };
    s.filter_bs = [] { return Msg{SymbolBackspace{}}; };
    s.extra     = [](const KeyEvent& e) -> std::optional<Msg> {
        if (auto* ck = std::get_if<CharKey>(&e.key))
            return Msg{SymbolInput{ck->codepoint}};
        return std::nullopt;
    };
    return nav::translate(s, ev);
}

// Ctrl+G code-block picker. Enter runs the cursor row; a bare digit
// runs that row directly (1-based, matching the ①②③ row labels — the
// zero-navigation fast path: Ctrl+G, 2, done). `e` stages into the
// composer for editing, `y` copies clean.
std::optional<Msg> on_code_block_picker(const KeyEvent& ev) {
    nav::NavSpec s;
    s.close  = [] { return Msg{CloseCodeBlocks{}}; };
    s.select = [] { return Msg{CodeBlocksSelect{}}; };
    s.move   = [](int d) { return Msg{CodeBlocksMove{d}}; };
    s.toggle_close_chord = U'g';
    s.extra  = [](const KeyEvent& e) -> std::optional<Msg> {
        const auto v = nav::char_view(e);
        if (!v) return std::nullopt;
        if (!v->ctrl && v->c >= U'1' && v->c <= U'9')
            return Msg{CodeBlocksSelect{static_cast<int>(v->c - U'1')}};
        if (!v->ctrl) switch (v->c) {
            case U'e': case U'E': return Msg{CodeBlocksEdit{}};
            case U'y': case U'Y': return Msg{CodeBlocksCopy{}};
            case U'q': case U'Q': return Msg{CloseCodeBlocks{}};
            default: break;
        }
        return std::nullopt;
    };
    return nav::translate(s, ev);
}

// Post-run result card: a = attach to composer, y = copy, Esc/q/Enter
// dismiss. Enter deliberately DISCARDS rather than attaches — the
// default action must be the safe one (no surprise composer content);
// attaching is the explicit `a`.
std::optional<Msg> on_code_block_result(const KeyEvent& ev) {
    nav::NavSpec s;
    s.close     = [] { return Msg{CodeBlockResultDiscard{}}; };
    s.select    = [] { return Msg{CodeBlockResultDiscard{}}; };   // safe default
    s.move      = [](int d) { return Msg{CodeBlocksMove{d}}; };
    s.page_step = 10;
    s.extra     = [](const KeyEvent& e) -> std::optional<Msg> {
        const auto v = nav::char_view(e);
        if (!v || v->ctrl) return std::nullopt;
        switch (v->c) {
            case U'a': case U'A': return Msg{CodeBlockResultAttach{}};
            case U'y': case U'Y': return Msg{CodeBlockResultCopy{}};
            case U'q': case U'Q': case U'd': case U'D':
                return Msg{CodeBlockResultDiscard{}};
            default: return std::nullopt;
        }
    };
    return nav::translate(s, ev);
}

// Rewind checkpoint picker: shared grammar + vim nav; read-only list.
std::optional<Msg> on_checkpoint_picker(const KeyEvent& ev) {
    nav::NavSpec s;
    s.close     = [] { return Msg{CloseCheckpoints{}}; };
    s.select    = [] { return Msg{CheckpointsSelect{}}; };
    s.move      = [](int d) { return Msg{CheckpointsMove{d}}; };
    s.vim_nav   = true;
    s.page_step = 10;
    return nav::translate(s, ev);
}

// What the key router needs to know about a form-backed pane.
//
// NOT the form itself. `subscribe()` runs every frame, and a Form owns a
// vector of rows each holding several std::strings — snapshotting it per frame
// meant an allocation and a deep copy of the whole settings pane on every
// keystroke and every animation tick, which is exactly the input lag it was
// meant to avoid. The router only ever asks WHICH MODE owns the keyboard and,
// for Choosing, nothing else. Three bools and an int is the whole dependency.
struct FormFocus {
    bool open     = false;
    bool editing  = false;
    bool choosing = false;
};

[[nodiscard]] inline FormFocus focus_of(const form::Form& f) noexcept {
    return FormFocus{true, f.editing(), f.choosing()};
}

// Forward a key to the shared form layer. Returns nullopt when the form does
// not claim the key, so ambient handling (^C quits from anywhere) still runs.
template <class Wrap>
[[nodiscard]] std::optional<Msg> on_form(const FormFocus& f, const KeyEvent& ev,
                                        Wrap&& wrap) {
    if (!f.open) return std::nullopt;
    if (auto a = form::keys::translate(f.editing, f.choosing, ev)) return wrap(*a);
    return std::nullopt;
}

// Key routing for the Retrieval overlay. The ENTIRE key map lives in the
// shared form layer — which mode owns the keyboard, what Enter does per row
// kind, how Esc unwinds — so this only forwards.
//
// `a` is the one exception: it changes which rows EXIST rather than acting on
// a row, so it is pane state. A bare letter rather than ^A, which the form
// layer uses for caret-home and which tmux takes as its default prefix.
// Suppressed while editing so it stays literal in a text field.
std::optional<Msg> on_rag_settings(const FormFocus& f, const KeyEvent& ev) {
    if (f.open && !f.editing)
        if (const auto v = nav::char_view(ev); v && !v->ctrl && v->c == U'a')
            return Msg{RagAdvanced{}};
    return on_form(f, ev, [](form::keys::Action a) { return Msg{RagEmbedKey{a}}; });
}

// Settings pickers (Plugins/Commands/Agents/Hooks). Two modes:
//   list mode — shared grammar + vim nav, `a` add, `d` remove, Space act.
//   add mode  — a one-line text prompt: every printable is LITERAL text,
//               so the nav shortcuts are suppressed entirely.
std::optional<Msg> on_settings_list(const KeyEvent& ev, bool input_active) {
    if (input_active) {
        nav::NavSpec s;
        s.close     = [] { return Msg{SettingsListCancelInput{}}; };
        s.select    = [] { return Msg{SettingsListSubmitInput{}}; };
        s.filter_bs = [] { return Msg{SettingsListBackspace{}}; };
        s.filter_ch = [](char32_t c) { return Msg{SettingsListChar{c}}; };
        return nav::translate(s, ev);
    }
    nav::NavSpec s;
    s.close   = [] { return Msg{CloseSettingsList{}}; };
    s.select  = [] { return Msg{SettingsListActivate{}}; };
    s.move    = [](int d) { return Msg{SettingsListMove{d}}; };
    s.vim_nav = true;
    s.extra   = [](const KeyEvent& e) -> std::optional<Msg> {
        const auto v = nav::char_view(e);
        if (!v || v->ctrl) return std::nullopt;
        switch (v->c) {
            case U'a': case U'A': return Msg{SettingsListAddStart{}};
            case U'd': case U'D': return Msg{SettingsListRemove{}};
            case U' ':            return Msg{SettingsListActivate{}};
            default: return std::nullopt;
        }
    };
    return nav::translate(s, ev);
}

// Fork picker: a pure list — each row is a distinct RAG mode for the fork.
std::optional<Msg> on_fork_picker(const KeyEvent& ev) {
    nav::NavSpec s;
    s.close   = [] { return Msg{CloseFork{}}; };
    s.select  = [] { return Msg{ForkThread{}}; };
    s.move    = [](int d) { return Msg{ForkMove{d}}; };
    s.vim_nav = true;
    return nav::translate(s, ev);
}

// The model picker: full grammar + typed filter + effort cycling. ^P
// cross-hops to providers, ^F favourites, ^R toggles reasoning display,
// ^L force-refreshes every catalog. There is exactly one model surface.
std::optional<Msg> on_fused_picker(const KeyEvent& ev) {
    nav::NavSpec s;
    s.close     = [] { return Msg{CloseModels{}}; };
    s.select    = [] { return Msg{ModelsSelect{}}; };
    s.move      = [](int d) { return Msg{ModelsMove{d}}; };
    s.jump      = [](nav::Jump j) {
        using W = ModelsJump::Where;
        return Msg{ModelsJump{j == nav::Jump::Home ? W::Home
                                 : j == nav::Jump::End  ? W::End
                                 : j == nav::Jump::PageUp ? W::PageUp
                                                          : W::PageDown}};
    };
    s.filter_bs = [] { return Msg{ModelsFilterBackspace{}}; };
    s.filter_ch = [](char32_t c) { return Msg{ModelsFilterInput{c}}; };
    s.extra     = [](const KeyEvent& e) -> std::optional<Msg> {
        if (const auto* sk = std::get_if<SpecialKey>(&e.key)) {
            // ←/→ cycle the highlighted model's reasoning-effort tier.
            if (*sk == SpecialKey::Left)  return Msg{ModelsCycleEffort{-1}};
            if (*sk == SpecialKey::Right) return Msg{ModelsCycleEffort{+1}};
            return std::nullopt;
        }
        const auto v = nav::char_view(e);
        if (!v) return std::nullopt;
        // ^/ (also arrives as raw 0x1F, which carries no ctrl flag) scopes
        // the list to the highlighted row's provider — press again to clear.
        // Close is Esc (the fused picker is the only model surface now, so ^/
        // no longer needs to toggle a second one shut).
        if (v->raw == 0x1F || (v->ctrl && v->c == U'/'))
            return Msg{ModelsScopeProvider{}};
        if (v->ctrl) {
            switch (v->c) {
                case U'p': return Msg{OpenProviders{}};   // cross-hop
                case U'f': return Msg{ModelsToggleFavorite{}};
                case U'r': return Msg{ModelsToggleShowReasoning{}};
                case U'l': return Msg{ModelsRefresh{}};
                default:   break;
            }
            // Fall through to nullopt: translate() never types ctrl'd
            // chars into the filter, and the dispatch chain returns this
            // handler's result unconditionally — the chord is swallowed.
        }
        return std::nullopt;
    };
    return nav::translate(s, ev);
}

// Provider picker: shared grammar + typed filter. ^/ hops to the fused
// model picker; ^D (and forward-Delete) removes a saved host / signs out
// of a keyed preset — two-press. Ctrl'd so it can't collide with typing
// a provider name starting with 'd' (deepseek…).
std::optional<Msg> on_provider_picker(const KeyEvent& ev) {
    nav::NavSpec s;
    s.close     = [] { return Msg{CloseProviders{}}; };
    s.select    = [] { return Msg{ProvidersSelect{}}; };
    s.move      = [](int d) { return Msg{ProvidersMove{d}}; };
    s.jump      = [](nav::Jump j) {
        using W = ProvidersJump::Where;
        return Msg{ProvidersJump{j == nav::Jump::Home ? W::Home
                                    : j == nav::Jump::End  ? W::End
                                    : j == nav::Jump::PageUp ? W::PageUp
                                                             : W::PageDown}};
    };
    s.filter_bs = [] { return Msg{ProvidersFilterBackspace{}}; };
    s.filter_ch = [](char32_t c) { return Msg{ProvidersFilterInput{c}}; };
    s.toggle_close_chord = U'p';
    s.extra     = [](const KeyEvent& e) -> std::optional<Msg> {
        if (const auto* sk = std::get_if<SpecialKey>(&e.key)) {
            // Forward-Delete removes; Mac laptops lack the key (their
            // "delete" sends Backspace) — ^D below is the reachable twin.
            if (*sk == SpecialKey::Delete) return Msg{ProvidersDelete{}};
            return std::nullopt;
        }
        const auto v = nav::char_view(e);
        if (!v) return std::nullopt;
        if (v->raw == 0x1F || (v->ctrl && v->c == U'/'))
            return Msg{OpenModels{}};                       // cross-hop
        if (v->ctrl && (v->c == U'd' || v->c == U'D'))
            return Msg{ProvidersDelete{}};
        return std::nullopt;
    };
    return nav::translate(s, ev);
}

// Thread list: shared grammar + vim nav; n = new thread, d = delete
// (two-press confirm).
std::optional<Msg> on_thread_list(const KeyEvent& ev) {
    nav::NavSpec s;
    s.close   = [] { return Msg{CloseThreadList{}}; };
    s.select  = [] { return Msg{ThreadListSelect{}}; };
    s.move    = [](int d) { return Msg{ThreadListMove{d}}; };
    s.jump    = [](nav::Jump j) {
        using W = ThreadListJump::Where;
        return Msg{ThreadListJump{j == nav::Jump::Home ? W::Home
                                : j == nav::Jump::End  ? W::End
                                : j == nav::Jump::PageUp ? W::PageUp
                                                         : W::PageDown}};
    };
    s.vim_nav = true;
    // ^J re-press closes (raw 0x0A is Enter on legacy terminals and maya
    // maps it to SpecialKey::Enter first, so only the flagged form arrives).
    s.toggle_close_chord = U'j';
    s.extra   = [](const KeyEvent& e) -> std::optional<Msg> {
        const auto v = nav::char_view(e);
        if (!v || v->ctrl) return std::nullopt;
        switch (v->c) {
            case U'n': case U'N': return Msg{NewThread{}};
            case U'd': case U'D': return Msg{ThreadListDelete{}};
            default: return std::nullopt;
        }
    };
    return nav::translate(s, ev);
}

// Smart Mode config: shared grammar + vim nav; Space toggles the row,
// x resets the selected slot to auto.
// Smart Mode. Same three lines as Retrieval, by construction — that is what
// keeps the two panes behaving identically.
std::optional<Msg> on_smart_mode(const FormFocus& f, const KeyEvent& ev) {
    // ^S re-pressed closes the pane it opened, the one chord that is genuinely
    // pane-specific (the form layer has no concept of an open chord).
    if (const auto v = nav::char_view(ev); v && v->ctrl && v->c == U's')
        return Msg{CloseSmartMode{}};
    // `a` reveals the advanced routing-policy rows. A BARE letter, not ^A:
    // ^A is the form layer's caret-home while editing, and it is the default
    // tmux prefix, so a chord there is swallowed before the app ever sees it.
    // In nav mode the form only claims k/j/h/l/x and space, so `a` is free —
    // the same vocabulary as the `x` that resets a slot.
    if (!f.editing)
        if (const auto v = nav::char_view(ev); v && !v->ctrl && v->c == U'a')
            return Msg{SmartModeAdvanced{}};
    return on_form(f, ev, [](form::keys::Action a) { return Msg{SmartModeKey{a}}; });
}

std::optional<Msg> on_diff_review(const KeyEvent& ev) {
    if (std::holds_alternative<SpecialKey>(ev.key)) {
        auto sk = std::get<SpecialKey>(ev.key);
        switch (sk) {
            case SpecialKey::Escape: return CloseDiffReview{};
            case SpecialKey::Up:     return DiffReviewMove{-1};
            case SpecialKey::Down:   return DiffReviewMove{+1};
            case SpecialKey::Left:   return DiffReviewPrevFile{};
            case SpecialKey::Right:  return DiffReviewNextFile{};
            // Enter accepts the current hunk (the primary action), so a
            // terminal with only Return + letters can still drive the review.
            case SpecialKey::Enter:  return AcceptHunk{};
            case SpecialKey::Tab:      return DiffReviewNextFile{};
            case SpecialKey::BackTab:  return DiffReviewPrevFile{};
            // Page INSIDE the focused hunk's capped body (a monster hunk
            // can exceed the 24-row window; without this rows 25+ were
            // unreachable — you decided blind).
            case SpecialKey::PageDown: return DiffReviewScroll{+12};
            case SpecialKey::PageUp:   return DiffReviewScroll{-12};
            default: break;
        }
    }
    if (auto* ck = std::get_if<CharKey>(&ev.key)) {
        char32_t c = ck->codepoint;
        // Legacy terminals deliver Ctrl-<letter> as the raw control byte
        // 0x01..0x1A with NO ctrl-modifier flag; normalise to the letter and
        // treat it as ctrl-held so the bulk shortcuts fire regardless of
        // keyboard-protocol support.
        const bool raw_ctrl = (c >= 0x01 && c <= 0x1A);
        if (raw_ctrl) c = U'a' + (c - 1);
        const bool ctrl = ev.mods.ctrl || raw_ctrl;

        // BULK actions are destructive/irreversible-ish, so they require
        // Ctrl — Ctrl+A = accept all, Ctrl+X = reject all. This also frees
        // plain a/x from being a one-key "reject everything" footgun.
        if (ctrl) {
            switch (c) {
                case U'a': case U'A': return AcceptAllChanges{};
                case U'x': case U'X': return RejectAllChanges{};
                // ^R re-press closes the pane (same commit semantics as Esc
                // — decisions persist, undecided hunks stay live).
                case U'r': case U'R': return CloseDiffReview{};
                // vim half-page scroll inside the focused hunk's body.
                case U'd': case U'D': return DiffReviewScroll{+12};
                case U'u': case U'U': return DiffReviewScroll{-12};
                default: break;
            }
            return std::nullopt;   // don't let a ctrl-chord fall through to a per-hunk key
        }

        switch (c) {
            // Per-hunk decisions — frequent + safe, so plain keys.
            case U'y': case U'Y': return AcceptHunk{};
            case U'n': case U'N': return RejectHunk{};
            // vim-style nav — no arrow keys needed (phone / legacy terminals).
            case U'j': case U'J': return DiffReviewMove{+1};
            case U'k': case U'K': return DiffReviewMove{-1};
            case U'h': case U'H': return DiffReviewPrevFile{};
            case U'l': case U'L': return DiffReviewNextFile{};
            case U'q': case U'Q': return CloseDiffReview{};
            default: break;
        }
    }
    return std::nullopt;
}

// Todo modal is AMBIENT: it only claims Esc + its ^T toggle; every other
// key falls through to on_global via the dispatcher's todo arm.
std::optional<Msg> on_todo_modal(const KeyEvent& ev) {
    nav::NavSpec s;
    s.close = [] { return Msg{CloseTodoModal{}}; };
    s.toggle_close_chord = U't';
    return nav::translate(s, ev);
}

// Ctrl+O tool-output viewer. Two stages share one handler: the LIST
// (↑↓/j/k move the cursor, Enter opens the body, y copies, Esc/q close)
// and the BODY (↑↓/j/k/PgUp/PgDn/Home/End scroll, y copies, Esc back to
// the list). The reducer branches on Open::viewing, so the same Move/
// Copy msgs serve both stages. h/l (and ←/→) step between entries.
std::optional<Msg> on_tool_viewer(const KeyEvent& ev) {
    nav::NavSpec s;
    s.close     = [] { return Msg{CloseToolOutput{}}; };
    s.select    = [] { return Msg{ToolOutputSelect{}}; };
    s.move      = [](int d) { return Msg{ToolOutputMove{d}}; };
    s.vim_nav   = true;
    s.page_step = 10;
    s.toggle_close_chord = U'o';
    s.extra     = [](const KeyEvent& e) -> std::optional<Msg> {
        if (const auto* sk = std::get_if<SpecialKey>(&e.key)) {
            if (*sk == SpecialKey::Left)  return Msg{ToolOutputStep{-1}};
            if (*sk == SpecialKey::Right) return Msg{ToolOutputStep{+1}};
            return std::nullopt;
        }
        const auto v = nav::char_view(e);
        if (!v || v->ctrl) return std::nullopt;
        switch (v->c) {
            case U'h': case U'H': return Msg{ToolOutputStep{-1}};
            case U'l': case U'L': return Msg{ToolOutputStep{+1}};
            case U'y': case U'Y': return Msg{ToolOutputCopy{}};
            default: return std::nullopt;
        }
    };
    return nav::translate(s, ev);
}

// Login modal — dispatches based on which sub-state we're in.
// Picking accepts only '1'/'2' (and Esc to close);
// OAuthCode + ApiKeyInput consume free-text input + cursor keys + Enter;
// OAuthExchanging consumes only Esc (cancel back to Picking is fine);
// Failed accepts any key to return to Picking.
std::optional<Msg> on_login(const ui::login::State& state, const KeyEvent& ev) {
    using namespace agentty::ui::login;

    // Esc pops ONE level of the flow (key prompt → host input → provider
    // picker → closed) — stepwise back-out, not a full collapse. The
    // reducer reads each sub-state's `back` origin; states with no parent
    // still close outright.
    if (std::holds_alternative<SpecialKey>(ev.key)
        && std::get<SpecialKey>(ev.key) == SpecialKey::Escape)
        return LoginBack{};

    if (std::holds_alternative<Picking>(state)
        || std::holds_alternative<Failed>(state)) {
        if (auto* ck = std::get_if<CharKey>(&ev.key))
            return LoginPickMethod{ck->codepoint};
        return std::nullopt;
    }

    if (std::holds_alternative<OAuthExchanging>(state)
        || std::holds_alternative<ChatGptWaiting>(state)
        || std::holds_alternative<HostProbing>(state)) {
        // Awaiting an async result (HTTP exchange / loopback callback /
        // host probe). No keys accepted besides Esc (handled above).
        return NoOp{};
    }

    // ── Account switcher: a LIST, not a text field ──────────────────
    // ↑/↓ (and j/k) move the highlight, Enter switches to / adds the
    // highlighted account, Delete/Backspace/d twice confirms removal.
    if (std::holds_alternative<AccountList>(state)) {
        if (std::holds_alternative<SpecialKey>(ev.key)) {
            switch (std::get<SpecialKey>(ev.key)) {
                case SpecialKey::Up:        return AccountMove{-1};
                case SpecialKey::Down:      return AccountMove{+1};
                case SpecialKey::Enter:     return AccountSelect{};
                case SpecialKey::Backspace: return AccountRemove{};
                case SpecialKey::Delete:    return AccountRemove{};
                default: return std::nullopt;
            }
        }
        if (auto* ck = std::get_if<CharKey>(&ev.key)) {
            switch (ck->codepoint) {
                case U'k': return AccountMove{-1};
                case U'j': return AccountMove{+1};
                case U'd': return AccountRemove{};   // vim-ish "delete"
                default: return std::nullopt;
            }
        }
        return std::nullopt;
    }

    // ── OAuthCode-only chrome shortcuts ─────────────────────────────
    // Bare letter `c` / `o` would collide with text input on the code
    // field, so we gate these on a modifier OR on the field being
    // empty (the typical state when the user has just landed in this
    // screen and wants to copy the URL before pasting the code).
    if (auto* oc = std::get_if<OAuthCode>(&state)) {
        if (auto* ck = std::get_if<CharKey>(&ev.key)) {
            char32_t c = ck->codepoint;
            // Normalize Ctrl-letter codes (terminals deliver Ctrl+X as
            // either raw 0x01..0x1A or the lowercase letter + ctrl mod
            // depending on whether KKP / modifyOtherKeys is enabled).
            if (ev.mods.ctrl && c >= 0x01 && c <= 0x1A)
                c = U'a' + (c - 1);
            const bool empty_code = oc->code_input.empty();
            // Ctrl+C is reserved for Quit at the global layer, so we
            // use Ctrl+Y as the no-collision "yank URL" binding. Plain
            // `c` / `o` also work when the code input is empty, so the
            // typical happy path (land here, hit `c`, paste in
            // browser) needs zero modifiers.
            const bool is_y = (c == U'y' || c == U'Y');
            const bool is_c = (c == U'c' || c == U'C');
            const bool is_o = (c == U'o' || c == U'O');
            if (ev.mods.ctrl && is_y) return LoginCopyAuthUrl{};
            if (ev.mods.ctrl && is_o) return LoginOpenBrowserAgain{};
            if (empty_code && is_c)   return LoginCopyAuthUrl{};
            if (empty_code && is_o)   return LoginOpenBrowserAgain{};
        }
    }

    // Copilot device-flow modal: there's no free-text field (the user types
    // the code into their BROWSER, not here), so `c` copies the code and `o`
    // reopens the browser — no modifier needed, and no `empty_code` gate.
    if (std::holds_alternative<DeviceWaiting>(state)) {
        // Device-flow modal (Copilot / Kimi / …): the user types the CODE into
        // the BROWSER, so bare letters are safe here (no text field). `c`
        // copies the code, `u` copies the URL, `o` reopens the browser.
        if (auto* ck = std::get_if<CharKey>(&ev.key)) {
            char32_t c = ck->codepoint;
            if (ev.mods.ctrl && c >= 0x01 && c <= 0x1A) c = U'a' + (c - 1);
            if (c == U'c' || c == U'C') return LoginCopyCode{};
            if (c == U'u' || c == U'U') return LoginCopyAuthUrl{};
            if (c == U'o' || c == U'O') return LoginOpenBrowserAgain{};
        }
    }

    // OAuthCode or ApiKeyInput — both accept free-text input.
    if (std::holds_alternative<SpecialKey>(ev.key)) {
        switch (std::get<SpecialKey>(ev.key)) {
            case SpecialKey::Enter:     return LoginSubmit{};
            case SpecialKey::Backspace: return LoginBackspace{};
            case SpecialKey::Left:      return LoginCursorLeft{};
            case SpecialKey::Right:     return LoginCursorRight{};
            default: return std::nullopt;
        }
    }
    if (auto* ck = std::get_if<CharKey>(&ev.key))
        if (ck->codepoint >= 0x20) return LoginCharInput{ck->codepoint};
    return std::nullopt;
}

std::optional<Msg> on_global(const KeyEvent& ev) {
    // Ctrl-J on legacy terminals (iSH, plain xterm, tmux without KKP /
    // modifyOtherKeys) arrives as the bare LF byte 0x0A. maya's input
    // parser folds BOTH \r (0x0D) and \n (0x0A) into SpecialKey::Enter,
    // so by the time we see it the key looks identical to Return — and
    // every other Ctrl shortcut works because they arrive as distinct
    // control bytes, but Ctrl-J gets swallowed into Enter. The only
    // surviving discriminator is raw_sequence, which preserves the
    // original byte: Return sends \r, Ctrl-J sends \n. Recover the
    // OpenThreadList binding from that.  (Terminals that map Return to
    // LF can't distinguish the two; there Ctrl-J is simply unavailable,
    // which is unavoidable for this keystroke.)
    if (std::holds_alternative<SpecialKey>(ev.key)
        && std::get<SpecialKey>(ev.key) == SpecialKey::Enter
        && !ev.mods.shift && !ev.mods.alt && !ev.mods.ctrl
        && ev.raw_sequence == "\n")
        return OpenThreadList{};
    // Alt+←/→ — quick-cycle through threads without the picker. In the
    // deck order the ^J list shows (newest first): ← = newer, → = older.
    // Checked in on_global (before on_composer) so it never collides
    // with plain/Ctrl arrow cursor movement in the composer.
    // (Terminals without a real Alt key — iPhone terminals — emulate Alt
    // as an Esc prefix. That only works because Esc is INERT on the main
    // screen: a stray Escape keystroke must never quit the app, or the
    // Esc-then-arrow sequence would kill agentty mid-chord. Quit is
    // Ctrl+C, deliberately the only app-exit key.)
    if (ev.mods.alt && !ev.mods.ctrl
        && std::holds_alternative<SpecialKey>(ev.key)) {
        switch (std::get<SpecialKey>(ev.key)) {
            case SpecialKey::Left:  return ThreadCycle{-1};
            case SpecialKey::Right: return ThreadCycle{+1};
            default: break;
        }
    }
    if (auto* ck = std::get_if<CharKey>(&ev.key)) {
        char32_t c = ck->codepoint;
        // Legacy terminals (iSH, plain xterm, tmux without KKP /
        // modifyOtherKeys) deliver Ctrl-<letter> as the raw control
        // byte 0x01..0x1A with NO ctrl modifier flag set — e.g. Ctrl-K
        // arrives as 0x0B. Normalise that to the lower-case letter with
        // ctrl implied so the shortcuts fire regardless of keyboard-
        // protocol support. (Ctrl-J / 0x0A is handled above since maya
        // turns it into Enter before we ever see it as a CharKey.)
        const bool raw_ctrl = (c >= 0x01 && c <= 0x1A);
        if (raw_ctrl) c = U'a' + (c - 1);
        // Ctrl-/ arrives as the raw byte 0x1F on legacy terminals (it sits
        // just past the 0x01..0x1A letter range above, so raw_ctrl misses
        // it and no ctrl modifier is reported). Map it to '/' with ctrl
        // implied so the model picker's primary key fires without needing
        // the kitty / modifyOtherKeys protocol. Mirrors on_provider_picker,
        // which already special-cases 0x1F.
        const bool ctrl_slash = (c == 0x1F);
        if (ctrl_slash) c = U'/';
        // 0x09 (Ctrl-I / Tab) and 0x0D (Ctrl-M / Enter) are excluded:
        // those are unconditionally Tab / Enter on legacy terminals and
        // hijacking them would break tab-completion and submit.
        const bool ctrl = ev.mods.ctrl || ctrl_slash
                       || (raw_ctrl && c != U'i' && c != U'm');
        if (ctrl) {
            switch (c) {
                case U'c': case U'C': return Quit{};
                case U'/':           return OpenModels{};
                case U'j': case U'J': return OpenThreadList{};
                case U'k': case U'K': return OpenPalette{};
                case U'p': case U'P': return OpenProviders{};
                case U'l': case U'L': return RedrawScreen{};
                case U'r': case U'R': return OpenDiffReview{};
                case U'n': case U'N': return NewThread{};
                case U't': case U'T': return OpenTodoModal{};
                case U'e': case U'E': return ComposerToggleExpand{};
                // ^B — LOOP: send this message, then keep re-sending it after
                // every completed turn until toggled off. Global (not
                // composer-local) so it can also be disarmed mid-stream,
                // when the composer isn't the thing taking keys.
                case U'b': case U'B': return ComposerToggleLoop{};
                case U'g': case U'G': return OpenCodeBlocks{};
                case U'o': case U'O': return OpenToolOutput{};
                case U's': case U'S': return OpenSmartMode{};
                default: break;
            }
        }
    }
    if (ev.mods.ctrl && std::holds_alternative<SpecialKey>(ev.key)) {
        auto sk = std::get<SpecialKey>(ev.key);
        // ^Tab — quick-swap to the previous (provider,model) in the MRU.
        if (sk == SpecialKey::Tab) return SwitchToPreviousModel{};
    }
    if (ev.mods.shift && std::holds_alternative<SpecialKey>(ev.key)) {
        auto sk = std::get<SpecialKey>(ev.key);
        if (sk == SpecialKey::Tab || sk == SpecialKey::BackTab)
            return CycleProfile{};
    }
    return std::nullopt;
}

// Composer state needed for keymap decisions, captured into the
// subscription so the lambda doesn't have to retain a Model copy
// (Model is move-only since the view cache moved on-board, and a
// reference would dangle past the subscribe() call).
struct ComposerKeyState {
    bool text_empty;
    bool has_queued;
    bool in_history;     // walking over prior user messages — ↓ has meaning
    bool has_history;    // any prior user turns exist at all — ↑ has meaning
    bool in_queue_peek;  // editing a queued item in place (Alt+↑/↓ cycle)
};

std::optional<Msg> on_composer(ComposerKeyState s, const KeyEvent& ev) {
    if (std::holds_alternative<SpecialKey>(ev.key)) {
        auto sk = std::get<SpecialKey>(ev.key);
        switch (sk) {
            case SpecialKey::Enter:
                // Newline on Shift+Enter (chat-app muscle memory) OR
                // Alt+Enter (universal fallback for terminals that don't
                // speak KKP / modifyOtherKeys). Maya enables both
                // protocols on entry, but if the user's terminal ignores
                // both, Shift+Enter arrives as plain Enter and the only
                // way to insert a newline is Alt+Enter — the legacy
                // binding that EVERY terminal delivers as `\x1b\r`.
                // Plain Enter still submits.
                return (ev.mods.shift || ev.mods.alt)
                       ? Msg{ComposerNewline{}}
                       : Msg{ComposerEnter{}};
            case SpecialKey::Backspace:
                // Alt+Backspace on an empty composer with nothing peeked
                // — "undo queue": drop the most recently queued message.
                // Useful when you fire-and-forget into the queue and
                // immediately regret it while the agent's still busy.
                if (ev.mods.alt && s.text_empty && s.has_queued && !s.in_queue_peek)
                    return Msg{ComposerQueuePopLast{}};
                return ComposerBackspace{};
            case SpecialKey::Left:
                // Ctrl+Left = jump-by-word. Mirrors readline / every
                // text editor. Plain Left is per-character (chip-aware).
                if (ev.mods.ctrl) return ComposerCursorWordLeft{};
                return ComposerCursorLeft{};
            case SpecialKey::Right:
                if (ev.mods.ctrl) return ComposerCursorWordRight{};
                return ComposerCursorRight{};
            case SpecialKey::Home:      return ComposerCursorHome{};
            case SpecialKey::End:       return ComposerCursorEnd{};
            case SpecialKey::Up:
                // Alt+↑ — per-item queue editor. Takes precedence over
                // every other ↑ binding so it works mid-edit (e.g. you
                // typed half a thought, realized you want to fix the
                // queued one first — Alt+↑ still gets you there
                // without having to clear the composer first).
                if (ev.mods.alt && s.has_queued)
                    return Msg{ComposerQueuePeekPrev{}};
                // ↑ priorities, in order:
                //   1. queue non-empty AND composer empty → recall queue
                //      (Claude Code's "Press up to edit queued
                //      messages" affordance, binary offsets 84602515 /
                //      76303220).
                //   2. already mid-history-walk → step further into the
                //      past.
                //   3. composer empty AND there's at least one prior
                //      user turn → start history walk.
                // Anything else (multi-line text editing, etc.) falls
                // through so ↑ stays available for cursor moves later.
                if (s.text_empty && s.has_queued)
                    return Msg{ComposerRecallQueued{}};
                if (s.in_history) return ComposerHistoryPrev{};
                if (s.text_empty && s.has_history) return ComposerHistoryPrev{};
                return std::nullopt;
            case SpecialKey::Down:
                // Alt+↓ — walk back OUT of the per-item queue peek
                // toward the live draft. Only meaningful while peeking;
                // outside that, falls through.
                if (ev.mods.alt && s.in_queue_peek)
                    return Msg{ComposerQueuePeekNext{}};
                // ↓ only has meaning while walking history — it walks
                // back toward the live draft. Outside the walk it
                // falls through.
                if (s.in_history) return ComposerHistoryNext{};
                return std::nullopt;
            case SpecialKey::Escape:
                // Deliberately INERT on the main screen — quit is Ctrl+C
                // only. Two reasons: (1) Esc is the most-mashed key in a
                // terminal (vim muscle memory, "dismiss whatever this
                // is") and instant app-exit on it loses sessions; (2)
                // iPhone terminals (iSH, Termius, a-Shell) have no Alt
                // key and emulate Alt+←/→ as Esc-then-arrow — the Esc
                // half of that chord must not be a live Quit binding.
                // Esc still cancels a streaming turn (handled in the
                // dispatch above) and closes every modal (each modal
                // handler owns its own Esc).
                return std::nullopt;
            default: return std::nullopt;
        }
    }
    if (auto* ck = std::get_if<CharKey>(&ev.key)) {
        // Ctrl-prefixed letter keys: editor-style controls. Tested
        // before the printable-text branch so a Ctrl+Z arriving as
        // CharKey{0x1A} or CharKey{'z'}+ctrl is captured either way.
        if (ev.mods.ctrl && !ev.mods.alt) {
            char32_t c = ck->codepoint;
            // Some terminals deliver ASCII Ctrl-X as 0x01..0x1A; others
            // (KKP / modifyOtherKeys) keep it as the lower-case letter
            // with mods.ctrl=true. Normalise.
            if (c >= 0x01 && c <= 0x1A) c = U'a' + (c - 1);
            switch (c) {
                case U'u': return ComposerKillToBeginningOfLine{};
                case U'w':
                    // Ctrl+W — delete word backward (readline
                    // unix-word-rubout). The universal "oops, drop
                    // that word" key across every shell and editor.
                    return ComposerDeleteWordBack{};
                case U'z':
                    // Ctrl+Shift+Z is the alternate Redo binding (no
                    // Ctrl+Y on macOS muscle-memory). Plain Ctrl+Z is
                    // Undo.
                    return ev.mods.shift ? Msg{ComposerRedo{}}
                                         : Msg{ComposerUndo{}};
                case U'y': return ComposerRedo{};
                case U'v':
                    // Ctrl+V → image paste from clipboard. On Linux/macOS
                    // this reaches us as raw 0x16 because the terminal
                    // emulator forwards Ctrl-letter codes. On Windows
                    // Terminal Ctrl+V is bound to the terminal's own
                    // "paste" action by default, so this keystroke is
                    // swallowed before it ever hits agentty — that's
                    // why we also accept Alt+V below and detect empty
                    // bracketed-paste in update/composer.cpp.
                    return ComposerImagePasteFromClipboard{};
                default: break;
            }
        }
        // Alt+V → image paste from clipboard, alternate trigger that
        // every terminal (Windows Terminal included) passes through
        // to the application. Same Msg as Ctrl+V; the reducer arm
        // doesn't care which key fired it.
        if (ev.mods.alt && !ev.mods.ctrl) {
            char32_t c = ck->codepoint;
            // Alt+V arrives as ESC v → CharKey{'v'} + mods.alt. Some
            // terminals upcase the codepoint when Shift is also held;
            // both V and v should fire.
            if (c == U'v' || c == U'V')
                return ComposerImagePasteFromClipboard{};
            // Alt+D — delete word forward (readline kill-word).
            // Symmetric to Ctrl+W; arrives as ESC d → CharKey{'d'}+alt.
            if (c == U'd' || c == U'D')
                return ComposerDeleteWordForward{};
            // Alt+K — kill to END of line. Readline's kill-to-end is
            // Ctrl+K, but that's reserved app-wide for the command
            // palette (on_global claims every Ctrl+K before the
            // composer sees it), so kill-to-end moves to the meta
            // variant. Pairs with Ctrl+U (kill-to-start), the same way
            // Alt+D (kill word forward) pairs with Ctrl+W (kill word
            // back).
            if (c == U'k' || c == U'K')
                return ComposerKillToEndOfLine{};
        }
        if (ck->codepoint >= 0x20) return ComposerCharInput{ck->codepoint};
    }
    return std::nullopt;
}

} // namespace

Sub<Msg> subscribe(const Model& m) {
    // THE routing decision: which overlay owns the keyboard. Computed once
    // per subscription rebuild by panel::top() — the SAME function the
    // view uses to decide what renders, so keys and pixels can never go to
    // different surfaces (the old twin if-chains could, and did, disagree
    // on order).
    const auto active_panel = ui::panel::top(m);
    const bool in_login = active_panel == ui::panel::Kind::Login;
    const bool settings_list_adding = [&] {
        const auto* so = m.ui.panel.get<pn::SettingsList>();
        return so && so->input_active;
    }();
    const bool streaming  = m.s.active()
                         && !m.s.is_awaiting_permission();
    // Ctrl+←/→ thread-cycle gate: the agent turn must be fully idle.
    // Distinct from `streaming` (which carves out awaiting-permission
    // so Esc keeps meaning "cancel") — switching threads mid-turn is
    // never allowed, permission prompt or not.
    const bool turn_active = m.s.active();
    bool has_history = false;
    for (const auto& msg : m.d.current.messages)
        if (msg.role == Role::User && !msg.text.empty()) { has_history = true; break; }
    // Newest LIVE (not-yet-frozen) retrieved-context card, if any — the
    // target for the Ctrl+U expand toggle. Empty when the most recent
    // proactive card has already settled into the frozen prefix (its
    // Element is baked; the reducer would no-op) or none exists. Frozen
    // cards' full passages are viewed via the Ctrl+O overlay instead.
    std::optional<MessageId> live_retrieved_id;
    for (std::size_t i = m.d.current.messages.size(); i-- > m.ui.frozen_through; ) {
        if (m.d.current.messages[i].is_proactive_context()) {
            live_retrieved_id = m.d.current.messages[i].id;
            break;
        }
    }
    const ComposerKeyState composer_state{
        m.ui.composer.text.empty(),
        !m.ui.composer.queued.empty(),
        m.ui.composer.history_index().has_value(),
        has_history,
        m.ui.composer.queue_peek_index().has_value(),
    };

    // Which mode each form-backed pane is in. Three bools per pane — NOT the
    // pane's rows, which would be a per-frame deep copy on the input path.
    FormFocus rag_form, smart_form_snap;
    if (const auto* o = m.ui.panel.get<ui::panel::Rag>())
        rag_form = focus_of(o->embed.form);
    if (const auto* o = m.ui.panel.get<ui::panel::SmartMode>())
        smart_form_snap = focus_of(o->form);

    auto key_sub = Sub<Msg>::on_key(
        [=, login_state = m.ui.login](const KeyEvent& ev) -> std::optional<Msg> {
            // ^C quits from ANYWHERE, before overlay routing. Every modal
            // picker's handler returns UNCONDITIONALLY (the dispatch below
            // never falls through for them), so without this a ^C pressed
            // while a picker/thread-list/palette is open would be swallowed
            // and the app would feel unquittable until you Esc'd out first.
            // on_permission already did this locally; hoisting it here makes
            // it uniform. Legacy terminals send raw 0x03.
            if (auto* ck = std::get_if<CharKey>(&ev.key)) {
                const char32_t c = ck->codepoint;
                if (c == 0x03 || (ev.mods.ctrl && (c == U'c' || c == U'C')))
                    return Quit{};
            }
            // Login modal owns the whole keyboard — auth is the gating
            // step, no other UI is reachable until the user finishes
            // (or Escs out, which is allowed but leaves agentty unauth'd).
            // Route to the active overlay's handler. Exhaustive on Kind
            // (-Wswitch): adding an overlay without a routing arm is a
            // compile warning, not a silent dead key. Kind::Todo is the one
            // AMBIENT overlay — unclaimed keys fall THROUGH to the global
            // handling below rather than being swallowed.
            using OK = ui::panel::Kind;

            // ── The escape guarantee ──────────────────────────────────
            // Every EXCLUSIVE overlay swallows unclaimed keys (that is what
            // makes it modal), which means a handler that fails to answer Esc
            // strands the user with no way out and the app appears frozen.
            // The Retrieval pane shipped exactly that bug: it owned the
            // keyboard for a frame before its form existed, so every key
            // — Esc included — went nowhere.
            //
            // Per-handler diligence cannot enforce this: it is one missing
            // branch away, in any of fifteen handlers, forever. So it is
            // enforced HERE, once, structurally. `dispatch` runs the overlay's
            // own handler; if that handler declines an Escape, the overlay's
            // generic close fires instead. A modal can therefore be buggy,
            // half-built, or brand new and STILL never trap the user.
            //
            // Ambient overlays (Todo) are exempt by construction — they fall
            // through to global handling and never swallow anything.
            const auto dispatch = [&]() -> std::optional<Msg> {
                switch (active_panel) {
                    case OK::Login:          return on_login(login_state, ev);
                    case OK::Permission:     return on_permission(ev);
                    case OK::Palette: return on_command_palette(ev);
                    case OK::Mention:        return on_mention_palette(ev);
                    case OK::Symbol:         return on_symbol_palette(ev);
                    case OK::CodeBlocks:     return on_code_block_picker(ev);
                    case OK::CodeBlockResult: return on_code_block_result(ev);
                    case OK::ToolOutput:     return on_tool_viewer(ev);
                    case OK::Checkpoints:    return on_checkpoint_picker(ev);
                    case OK::Rag:    return on_rag_settings(rag_form, ev);
                    case OK::SettingsList:
                        return on_settings_list(ev, settings_list_adding);
                    case OK::Fork:           return on_fork_picker(ev);
                    case OK::Models:    return on_fused_picker(ev);
                    case OK::Providers: return on_provider_picker(ev);
                    case OK::ThreadList:     return on_thread_list(ev);
                    case OK::SmartMode:      return on_smart_mode(smart_form_snap, ev);
                    case OK::DiffReview:     return on_diff_review(ev);
                    case OK::Todo:           return on_todo_modal(ev);
                    case OK::None:           break;
                }
                return std::nullopt;
            };

            if (auto r = dispatch()) return r;

            // Todo is ambient: unclaimed keys fall THROUGH to global handling
            // below rather than being swallowed, so the guarantee (and the
            // swallow) do not apply to it.
            if (active_panel != OK::None && active_panel != OK::Todo) {
                if (std::holds_alternative<SpecialKey>(ev.key)
                    && std::get<SpecialKey>(ev.key) == SpecialKey::Escape)
                    return ui::panel::close_msg(active_panel);
                // Exclusive overlay, unclaimed non-Esc key: swallow it, as
                // modality requires.
                return Msg{NoOp{}};
            }
            // Esc during a live stream cancels the request rather than
            // quitting the app. Modals above swallow Esc themselves, so this
            // only fires from the bare composer view.
            if (streaming
                && std::holds_alternative<SpecialKey>(ev.key)
                && std::get<SpecialKey>(ev.key) == SpecialKey::Escape)
                return CancelStream{};
            // Ctrl+←/→ — quick-cycle threads, same deck order as Alt+←/→
            // (← = newer, → = older). Only when the composer is EMPTY:
            // with text in the box Ctrl+arrows stay jump-by-word
            // (readline muscle memory, handled in on_composer). And only
            // while no agent turn is running — mid-turn the key falls
            // through to the composer so it can't yank the thread out
            // from under a live stream.
            if (!turn_active && composer_state.text_empty
                && ev.mods.ctrl && !ev.mods.alt
                && std::holds_alternative<SpecialKey>(ev.key)) {
                switch (std::get<SpecialKey>(ev.key)) {
                    case SpecialKey::Left:  return ThreadCycle{-1};
                    case SpecialKey::Right: return ThreadCycle{+1};
                    default: break;
                }
            }
            // Ctrl+U on an EMPTY composer — expand / collapse the newest
            // retrieved-context card (full passage text ↔ one-line snippet).
            // Guarded on empty composer so it never steals Ctrl+U's
            // readline "kill-to-start-of-line" meaning while typing; with
            // no live card it simply falls through. Handled here (not
            // on_global) because it needs the pre-computed target id.
            if (composer_state.text_empty && live_retrieved_id
                && ev.mods.ctrl && !ev.mods.alt) {
                if (auto* ck = std::get_if<CharKey>(&ev.key)) {
                    char32_t c = ck->codepoint;
                    if (c >= 0x01 && c <= 0x1A) c = U'a' + (c - 1);
                    if (c == U'u')
                        return ToggleRetrievedExpanded{*live_retrieved_id};
                }
            }
            if (auto msg = on_global(ev)) return msg;
            return on_composer(composer_state, ev);
        });

    auto paste_sub = Sub<Msg>::on_paste(
        [in_login, settings_list_adding,
         rag_editing   = rag_form.editing,
         smart_editing = smart_form_snap.editing](std::string s) -> Msg {
        // Route a bracketed paste to whatever modal currently owns text
        // input, so it lands in that field's buffer — NOT the composer.
        //   • login modal open      → its code/key fields (OAuth codes, keys)
        //   • settings-list add-mode → the inline prompt (e.g. a plugin's
        //     "name command args…" line under Ctrl+K → Plugins). Without
        //     this the paste fell through to ComposerPaste and appeared in
        //     the composer while the add-prompt had visual focus.
        //   • a form field being EDITED (Retrieval / Smart Mode) → that
        //     field — API keys and hosts are the fields pasting exists for;
        //     these panes' reducers re-verify the mode, so a stale snapshot
        //     degrades to a dropped paste, never a mis-target.
        //   • otherwise               → the composer.
        if (in_login) return LoginPaste{std::move(s)};
        if (settings_list_adding) return SettingsListPaste{std::move(s)};
        if (rag_editing)   return RagEmbedPaste{std::move(s)};
        if (smart_editing) return SmartModePaste{std::move(s)};
        return ComposerPaste{std::move(s)};
    });

    // Only subscribe to Tick while the spinner is visible. With fps=0 the
    // maya loop is purely event-driven; an unconditional 16ms tick would
    // force a render 60× per second even when nothing is changing.
    //
    // Tick cadence is gated on the host terminal's support for DEC mode
    // 2026 (synchronized output). On terminals that buffer the frame
    // atomically, 33 ms (~30 fps) keeps the spinner smooth without
    // flicker. On terminals that paint bytes as they arrive (Apple
    // Terminal, plain xterm, tmux without sync passthrough), every
    // repaint is visibly progressive — so we drop to 100 ms (10 fps) to
    // cut the flicker frequency by 3× at the cost of a slightly choppier
    // spinner. The capability is heuristic-detected once at startup; see
    // maya::ansi::env_supports_synchronized_output().
    //
    // SSH override: when remote, the wire — not local paint — is the
    // bottleneck. Each streaming frame emits several KB of ANSI diff;
    // at 30 fps that's ~290 KB/s, which saturates a high-latency or
    // low-bandwidth link and shows up as mid-turn lag plus an
    // end-of-turn catch-up repaint (the kernel send buffer drains the
    // backlog after the stream stops). We clamp the streaming tick to
    // at least 80 ms (~12 fps) when remote — the SSH round-trip latency
    // already dominates perceived smoothness, so dropped frames aren't
    // noticeable, while the sustained byte rate falls proportionally.
    // We take the SLOWER of the local choice and the SSH floor so a
    // non-sync terminal (already 100 ms) is never sped up. The reveal
    // SPEED is unchanged — the pacer is bytes/second, not bytes/tick, so
    // prose fills at the same wall-clock rate, just in fewer, larger
    // frames.
    // The settle-freeze (meta.cpp Tick, the agent_session MessageStop
    // analog) fires on a Tick while m.s.is_idle() and pending_settle_
    // freeze is set. Gating the tick ONLY on active()/live-bytes drops
    // the clock the instant the reveal drains its last bytes into the
    // settled body — but the widget hasn't flipped live_ off yet, so
    // pending_settle_freeze is still set with no tick left to fire it.
    // The deferred freeze then strands until the next user keystroke and
    // diffs against a stale prev_cells = cache-miss re-emit = the
    // duplicated turn the user sees in scrollback. Keep ticking until the
    // freeze has actually fired (flag cleared) so the live-tail→frozen
    // handoff always lands on a fresh frame, exactly like agent_session's
    // always-on 30fps clock reconciles the collapse the same frame.
    //
    // Reveal-still-gliding term (was MISSING — the low-CPU post-stream
    // stall). maya's reveal_fx is a WALL-CLOCK typewriter: after the wire
    // drains its last bytes (m.s.active() false, tail_has_live_bytes
    // false) the cursor is often still gliding across the final
    // paragraph at ~90 cps, with SECONDS of animation left on a longer
    // turn. If the gate rests only on the four terms above, the Tick
    // stops the instant the bytes land and the reveal FREEZES mid-glide
    // — the widget sits at 1% CPU waiting for a clock that won't tick
    // until the next keystroke, which then snaps the remaining text into
    // view all at once. `!live_tail_reveal_settled(m)` is true exactly
    // while the reveal (is_live / reveal_in_progress / is_finalizing /
    // is_parsing) has NOT drained, so we keep waking the widget until the
    // typewriter reaches the live edge — the same reason build_live_tail
    // and the deferred settle-freeze consult this predicate.
    // Terminal window focus (?1004, maya enables it in inline mode).
    // Gates the hardware caret: unfocused ⇒ the composer stops emitting
    // its caret anchor and the real cursor parks + hides.
    auto focus_sub = Sub<Msg>::on_focus(
        [](bool focused) -> Msg { return TerminalFocus{focused}; });

    // Tick drives every time-based animation. THREE gates must agree on
    // WHAT is animating, and animation_demand (subscribe.hpp) is now the
    // one definition they share — this subscription arms the timer, the
    // spinner advance (update/meta.cpp Tick arm) steps reducer state, and
    // the visual hash (app/program.hpp) lets the wake reach view(). The
    // per-term rationale lives on animation_demand's definition above.
    if (animation_demand(m)) {
        auto tick = Sub<Msg>::every(streaming_tick_period(), Tick{});
        return Sub<Msg>::batch(std::move(key_sub), std::move(paste_sub),
                               std::move(focus_sub), std::move(tick));
    }
    return Sub<Msg>::batch(std::move(key_sub), std::move(paste_sub),
                           std::move(focus_sub));
}

} // namespace agentty::app
