#pragma once
// agentty::ui::picker_detail — shared building blocks for the overlay pickers.
//
// pickers.cpp was one 2000-line translation unit holding every picker view
// AND their shared helpers in an anonymous namespace. Splitting the views
// into sibling .cpp files (model_picker.cpp, nav_pickers.cpp, …) means those
// helpers need a home every sibling can include — this header. Everything
// here is `inline` (header-only), pure, and free of per-picker state, so a
// picker file gets a consistent list shape (viewport sizing, badge width),
// a consistent semantic palette (tier hue), the shared reasoning footer, and
// a uniform section-divider header just by including this.

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <maya/dsl.hpp>
#include <maya/widget/panel.hpp>
#include <maya/platform/io.hpp>

#include "agentty/runtime/model.hpp"
#include "agentty/domain/catalog.hpp"   // resolved_caps, efforts, caps_provider_scope
#include "agentty/runtime/view/helpers.hpp"
#include "agentty/runtime/view/palette.hpp"
#include "agentty/provider/registry.hpp"    // wire_streams_reasoning_text
#include "agentty/provider/selection.hpp"

namespace agentty::ui::picker_detail {

using namespace maya;
using namespace maya::dsl;

// ── Path formatting (thread list / mention palette) ───────────────────────

// "src/runtime/foo.cpp" → ("foo.cpp", "src/runtime/").
// Returns ("foo.cpp", "") for a bare filename.
[[nodiscard]] inline std::pair<std::string_view, std::string_view>
split_name_dir(std::string_view path) {
    auto slash = path.find_last_of('/');
    if (slash == std::string_view::npos) return {path, {}};
    return {path.substr(slash + 1), path.substr(0, slash + 1)};
}

// Compress a directory path to its IMMEDIATE parent only — that's the
// disambiguator the user actually scans for. Truncation of the segment
// itself is left to maya (`| clip`), so this just performs the semantic
// step ("/home/.../Best Of Kumar Sanu/" → "Kumar Sanu/").
[[nodiscard]] inline std::string parent_segment(std::string_view dir) {
    if (dir.empty()) return {};
    auto inner = dir;
    if (inner.back() == '/') inner.remove_suffix(1);
    auto slash = inner.find_last_of('/');
    auto last = (slash == std::string_view::npos)
        ? inner : inner.substr(slash + 1);
    std::string out{last};
    out.push_back('/');
    return out;
}

// ── Panel width floors (shared by every overlay that isn't content-sized) ──
//
// Three named sizes rather than a number per pane. The widths had drifted to
// 45 / 50 / 52 / 54 / 60 / 64 across twelve overlays with no story behind the
// differences — which is accretion, not decision.
//
// Be precise about what this buys, because it is less than it looks. These are
// FLOORS, and maya::Panel treats min_width as a minimum on a box that still
// stretches to its container: on any terminal wider than the floor every pane
// renders at the same width regardless, and on a NARROWER one the caller
// clamps it down anyway (see form_view.cpp, which caps min_width at
// max_width - 6 so a floor wider than the screen cannot push the label column
// off both edges). So the old numbers were mostly inert, and unifying them
// changes no pixels today.
//
// What it does buy: a new pane picks a NAME that says what shape its content
// is, instead of inventing a number nobody can later justify — and the floor
// is then right on the narrow terminals where it does bind.

// One column of short text — a plan item, a mode name. Enough to read a
// sentence fragment without wrapping on a phone-sized terminal.
inline constexpr int kPanelNarrow = 48;

// The default: a label column plus a value/meta column. Threads, providers,
// checkpoints, the command palette — anything shaped "name … detail". Matches
// maya::Panel::Config's own default, so a pane that says nothing gets this.
inline constexpr int kPanelStandard = 60;

// Three columns, or one column carrying a path. Symbol and code-block pickers
// show file:line alongside their subject, and the settings list carries a
// value plus a provenance tag.
inline constexpr int kPanelWide = 68;

// ── Viewport sizing (shared by every picker's scrollable list) ────────────

// Viewport height (rows) for every picker's scrollable list. Single constant
// so all pickers share the same shape; items beyond it are reachable via the
// scrollbar, and the selection always stays visible via the widget's
// auto-scroll-to-selection logic.
inline constexpr int kViewportH = 14;

// Picker chrome around the scrollable list: top border + title row + a blank
// + the two-row footer (blank + hint line) + bottom border, plus the
// AppLayout outer padding. ~7 rows. The picker floats bottom-pinned in a
// zstack over the base; maya extends the frame to whichever layer is taller.
// If the picker's TOTAL height (list + chrome) exceeds the terminal viewport,
// opening it scrolls the base's top rows into native scrollback that can't be
// reclaimed — so each open/close strands another copy of the welcome
// wordmark. Clamping the list so the WHOLE picker fits avoids the overflow.
inline constexpr int kPickerChromeRows = 7;

[[nodiscard]] inline int picker_terminal_rows() {
    const auto sz = maya::platform::query_terminal_size(
        maya::platform::stdout_handle());
    // Prefer the real ioctl height. When it's unavailable (no tty: a pipe, a
    // test harness) ws_col is 0 and the query returns maya's hardcoded
    // {80,24} fallback — not the real viewport. In that no-tty case only,
    // fall back to the LINES env var so the clamp uses the true height. A
    // valid ioctl always wins over LINES (which may be stale).
    int term_rows = sz.height.value;
    const bool have_tty = maya::platform::is_tty(
        maya::platform::stdout_handle());
    if (!have_tty) {
        if (const char* lines_env = std::getenv("LINES")) {
            if (const int n = std::atoi(lines_env); n > 0) term_rows = n;
        }
    }
    if (term_rows <= 0) term_rows = 40;
    return term_rows;
}

[[nodiscard]] inline int picker_viewport_h() {
    const int term_rows = picker_terminal_rows();
    // Leave the chrome plus a small breathing margin so the picker's top
    // border sits strictly below the viewport top with the base behind it.
    const int avail = term_rows - kPickerChromeRows - 1;
    // Floor of 4 list rows keeps the picker usable even on a tiny term (it
    // scrolls); ceiling is the shared kViewportH.
    return std::clamp(avail, 4, kViewportH);
}

// Terminal WIDTH, resolved the same way picker_terminal_rows() resolves
// height: real ioctl first, COLUMNS only when there is no tty.
[[nodiscard]] inline int picker_terminal_cols() {
    const auto sz = maya::platform::query_terminal_size(
        maya::platform::stdout_handle());
    int cols = sz.width.value;
    const bool have_tty = maya::platform::is_tty(
        maya::platform::stdout_handle());
    if (!have_tty) {
        if (const char* c = std::getenv("COLUMNS")) {
            if (const int n = std::atoi(c); n > 0) cols = n;
        }
    }
    if (cols <= 0) cols = 80;
    return cols;
}

// How many columns the provider badge may occupy. The badge is the grouping
// signal in a flat cross-provider list, so it must stay legible — but it
// competes with the model NAME the user is actually reading.
//
// The cap used to be a fixed 8/12/16 columns by terminal width, which broke
// the whole layout the moment a custom host grew its label past 16 columns
// (e.g. `ollama.com [seaventures]` at 24): the widget renders the badge at
// its natural width, so that one row was wider than the rest and the model
// column went ragged. Derive the cap from the terminal width instead — let
// the column grow to fit the longest actual label, but never let it eat
// more than half the picker's horizontal space so the model name column
// still has usable room on narrow terminals.
[[nodiscard]] inline int picker_badge_max_cols() {
    const int cols = picker_terminal_cols();
    // Reserve at least ~half the width for the model NAME column. On a very
    // narrow terminal (≤ 40 cols) the badge cap bottoms out at 8 (the old
    // "abbreviate hard" floor); on wider terminals it tracks half the
    // terminal, so a long custom-host label is never the cause of ragged
    // alignment — the alignment breaks only when a pathologically wide host
    // (≥ half the terminal) would leave no room for a model name at all.
    if (cols <= 40) return 8;            // tiny: abbreviate hard
    const int cap = cols / 2;
    return cap < 8 ? 8 : cap;            // never below the tiny-terminal floor
}

// ── Capability-tier hue (model picker badges) ─────────────────────────────

// Provider-badge hue, keyed on the model's capability TIER. The browse list
// is ordered strongest-first; colour makes that invisible ordering legible.
// Hues come from the shared palette's semantic ramp, intensity-ordered so it
// reads as a gradient (bright accent → blue → cyan → grey). Applied to the
// BADGE, never the name, and never the sole signal (ordering + ✦ mark say the
// same thing without colour — WCAG 1.4.1).
[[nodiscard]] inline maya::Color tier_hue(ModelCapabilities::Tier t) {
    using T = ModelCapabilities::Tier;
    switch (t) {
        case T::Flagship: return role_brand_alt;  // bright magenta — top lane
        case T::Mid:      return role_info;        // blue — workhorse lane
        case T::Cheap:    return code_path;        // bright cyan — fast/small
        case T::Weak:     return muted;            // grey — tool-use unreliable
    }
    return muted;
}

// Resolve the currently-active provider id so a picker can mark the active
// row. Anthropic (the default) when kind==Anthropic, else the endpoint label
// / ACP agent id. Shared by the provider picker and the Smart Mode overlay.
[[nodiscard]] inline std::string active_provider_id() {
    const auto& sel = provider::active();
    if (sel.kind == provider::Kind::OpenAI) return sel.openai_endpoint.label;
    if (sel.kind == provider::Kind::ExternalAcp) return sel.acp_agent_id;
    return std::string{provider::default_provider_id()};
}

// A section is one labelled band of a grouped picker list. `hue` tints the
// label; `count` (when > 0) renders dim + right-pinned so a band can carry a
// quiet stat without competing with the label.
struct SectionHeader {
    std::string label;                 // rendered UPPERCASED
    maya::Color hue   = fg;
    int         count = 0;             // 0 = no count shown
};

// Build one `is_header` row from a SectionHeader.
//
// The label is passed through as-is: Panel uppercases a header and draws the
// rule to the right edge. Doing it here too was the same transform applied
// twice by two owners — harmless only because upper-casing is idempotent.
[[nodiscard]] inline Panel::Item section_header(SectionHeader h) {
    Panel::Item hdr;
    hdr.control       = maya::panel::Header{};
    hdr.leading       = std::move(h.label);
    hdr.leading_style = fg_of(h.hue);
    if (h.count > 0) {
        hdr.trailing       = std::to_string(h.count);
        hdr.trailing_style = fg_italic(muted);
    }
    return hdr;
}

// ── Reasoning-effort footer (model picker) ────────────────────────────────

// The reasoning-effort control footer, shared by every model surface so they
// render IDENTICALLY and read/write the SAME state (m.d.effort, the per-model
// reasoning_override, m.d.show_reasoning). `model_id` is the highlighted
// row's model; `scope` its provider (empty = active provider) so a fused
// row's ladder reflects THAT host's contract. Returns the rows to append to
// a picker's footer (may be empty). Kept in ONE place so surfaces can never
// diverge (the "off in one, on in the other" bug).
[[nodiscard]] inline std::vector<Element>
reasoning_effort_footer(const Model& m, std::string_view model_id,
                        std::string_view scope = {}) {
    std::vector<Element> out;
    if (model_id.empty()) return out;

    const std::string hi_id{model_id};
    const auto caps = resolved_caps(hi_id, scope);

    // ── Show-reasoning toggle piece (^R), a global on/off display switch.
    // Appended inline to the reasoning line so effort + show/hide live in ONE
    // place. ✦ + accented when on, dim when off.
    auto append_show_reasoning = [&](std::vector<Element>& parts) {
        const bool on = m.d.show_reasoning;
        parts.push_back(text("  ", fg_dim(muted)));
        parts.push_back(text("^R ", fg_of(fg)));
        // Honest label when the DIALECT can't carry reasoning text back for
        // THIS model. First-party OpenAI uses Chat Completions, which does
        // not transmit GPT-5 reasoning at all. Copilot is MIXED and therefore
        // model-dependent (gpt-5*/mai-code-* stream reasoning; claude-*/
        // gpt-4.x are chat-only) — so the answer is per ROW, not per provider.
        // `scope` empty means the ACTIVE provider, which is what
        // caps_provider_scope() publishes on every switch.
        const std::string prov =
            scope.empty() ? caps_provider_scope() : std::string{scope};
        if (!provider::wire_streams_reasoning_text(prov, hi_id)) {
            parts.push_back(text("n/a on this API", fg_dim(muted)));
            return;
        }
        parts.push_back(text(on ? "\xe2\x9c\xa6 shown" : "hidden",
                             on ? fg_bold(accent) : fg_dim(muted)));
    };

    if (effort_capable(caps)) {
        // One line: the effort ladder (current tier bracketed ‹like this› and
        // accented), then the global ^R show/hide toggle. ←/→ cycles the tier.
        std::vector<Element> parts;
        parts.push_back(text("reasoning ", fg_dim(muted)));
        for (Effort lvl : available_efforts(caps)) {
            const std::string lbl{effort_label(lvl)};
            if (lvl == m.d.effort) {
                parts.push_back(text("\xe2\x80\xb9", fg_of(accent)));      // ‹
                parts.push_back(text(lbl, fg_bold(accent)));
                parts.push_back(text("\xe2\x80\xba ", fg_of(accent)));   // ›
            } else {
                parts.push_back(text(lbl + " ", fg_dim(muted)));
            }
        }
        append_show_reasoning(parts);
        out.push_back(h(std::move(parts)).build());
    } else {
        // No effort control on this model — still show the global ^R toggle.
        std::vector<Element> parts;
        parts.push_back(text("reasoning ", fg_dim(muted)));
        parts.push_back(text("off", fg_dim(muted)));
        append_show_reasoning(parts);
        out.push_back(h(std::move(parts)).build());
    }
    return out;
}

}  // namespace agentty::ui::picker_detail
