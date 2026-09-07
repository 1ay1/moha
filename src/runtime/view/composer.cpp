#include "agentty/runtime/view/composer.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "agentty/runtime/composer_attachment.hpp"
#include "agentty/runtime/panel/top.hpp"
#include "agentty/runtime/view/helpers.hpp"
#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/view/status_bar/model_badge.hpp"
#include "agentty/provider/selection.hpp"
#include <maya/core/anim_clock.hpp>
#include <maya/terminal/tmux.hpp>
#include <maya/core/motion.hpp>   // anim::keep_animating_after (typing-window lapse wake)

namespace agentty::ui {

namespace {

// Map agentty runtime state → widget State enum. Pure data translation;
// the widget owns all visual decisions (border color, prompt boldness,
// placeholder text, height pin).
maya::Composer::State composer_state(const Model& m) {
    if (m.s.is_awaiting_permission()) return maya::Composer::State::AwaitingPermission;
    if (m.s.is_executing_tool())      return maya::Composer::State::ExecutingTool;
    if (m.s.is_streaming())           return maya::Composer::State::Streaming;
    return maya::Composer::State::Idle;
}

// Walk the composer text, replacing each placeholder token with a
// human-readable chip caption ("[Pasted text · 412 lines · 14 KB]" /
// "[@src/auth/login.cpp]"). Translate the agentty-space cursor into
// view-space simultaneously: any agentty cursor position is — by the
// chip-aware navigation in update/composer.cpp — always at a chip
// boundary, never inside a token, so the mapping is well-defined.
struct DisplayText {
    std::string text;
    int         cursor = 0;
};
DisplayText render_chips(std::string_view src, int agentty_cursor,
                         const std::vector<Attachment>& attachments) {
    DisplayText out;
    out.text.reserve(src.size());
    int i = 0;
    int n = static_cast<int>(src.size());
    int cur = std::clamp(agentty_cursor, 0, n);
    while (i < n) {
        if (i == cur) out.cursor = static_cast<int>(out.text.size());
        if (static_cast<unsigned char>(src[i]) == 0x01) {
            auto len = attachment::placeholder_len_at(
                src, static_cast<std::size_t>(i));
            if (len > 0) {
                auto idx = attachment::placeholder_index(
                    src, static_cast<std::size_t>(i));
                std::string chip = "[?]";
                if (idx < attachments.size()) {
                    chip = "[" + attachment::chip_label(attachments[idx]) + "]";
                }
                out.text.append(chip);
                i += static_cast<int>(len);
                continue;
            }
        }
        out.text.push_back(src[i++]);
    }
    if (cur >= n) out.cursor = static_cast<int>(out.text.size());
    return out;
}

// Visually clip lines that are absurdly wide. The maya Composer
// word-wraps at terminal width, so an 800-byte single-line paste
// (URL, base64 blob, hash) produces ~10 wrapped rows and dominates
// the composer's vertical space. We replace the BODY of any line
// over `kVisibleLineWidth` bytes with `head … tail (N chars)` —
// purely visual, the underlying buffer (which is what gets sent to
// the model) is untouched. The cursor's host line is NEVER clipped
// so the user can edit inside what they pasted.
//
// Discipline: this runs *after* render_chips so the byte counts here
// already reflect chip captions, not raw placeholders. We work in
// bytes, not display columns — close enough for ASCII-heavy pastes
// (URLs / hashes) and we don't owe perfect grapheme accounting for a
// purely cosmetic clip.
constexpr int kVisibleLineWidth = 160;
constexpr int kHeadKeep         = 80;
constexpr int kTailKeep         = 40;

DisplayText clip_long_lines(DisplayText in) {
    DisplayText out;
    out.text.reserve(in.text.size());
    int n   = static_cast<int>(in.text.size());
    int cur = in.cursor;
    int i   = 0;
    while (i < n) {
        int j = i;
        while (j < n && in.text[j] != '\n') ++j;
        int line_len = j - i;
        bool cursor_on_line = cur >= i && cur <= j;
        if (line_len > kVisibleLineWidth && !cursor_on_line) {
            out.text.append(in.text, i, kHeadKeep);
            out.text.append(" \xe2\x80\xa6 ");                  // " … "
            out.text.append(in.text, j - kTailKeep, kTailKeep);
            // Always-on suffix so the user knows there's more under
            // the visual clip ("(412 chars)" / "(1.2 KB)").
            char suf[32];
            if (line_len < 1024)
                std::snprintf(suf, sizeof(suf), " (%d chars)", line_len);
            else
                std::snprintf(suf, sizeof(suf), " (%.1f KB)",
                              static_cast<double>(line_len) / 1024.0);
            out.text.append(suf);
        } else {
            // Cursor is on this line OR the line is short enough to
            // render verbatim. If the cursor is on a clipped-eligible
            // line we surface it as-is so editing keeps working — the
            // composer's word-wrap will still chunk it, but at least
            // the bytes the user is poking at are visible.
            if (cursor_on_line && cur > i) {
                // Translate the cursor offset into the OUT buffer.
                // out has gained no extra bytes for this line, so the
                // delta is just (cur - i) added to current out size.
                int new_cursor = static_cast<int>(out.text.size()) + (cur - i);
                out.cursor = new_cursor;
            }
            out.text.append(in.text, i, line_len);
        }
        if (j < n) {
            out.text.push_back('\n');
            // Cursor exactly on the trailing newline of a clipped line
            // — placement onto the post-clip representation is well-
            // defined as end-of-clipped-line; cursor_on_line caught it
            // above for unclipped path. For the clipped path we deal
            // with it here: if the cursor sits at j (the '\n') and the
            // line was clipped, drop it on the newline we just emitted.
            if (cur == j && line_len > kVisibleLineWidth)
                out.cursor = static_cast<int>(out.text.size()) - 1;
        }
        i = j + 1;
        if (j == n) break;  // no trailing newline; we're done
    }
    // Cursor at end-of-buffer.
    if (cur >= n) out.cursor = static_cast<int>(out.text.size());
    return out;
}

} // namespace

// One definition of "does the terminal own the caret this frame". See the
// declaration in composer.hpp for why this is shared rather than inlined:
// the config builder and visual_hash must agree, and they live in different
// files. Pure over the model, and cheap — visual_hash calls it every frame.
bool composer_uses_hardware_caret(const Model& m) noexcept {
    // AGENTTY_PAINTED_CARET=1 opts out wholesale (broken-DECTCEM terminals,
    // or users who prefer the painted block).
    static const bool painted_caret_env =
        std::getenv("AGENTTY_PAINTED_CARET") != nullptr;
    const auto state = composer_state(m);
    const bool agent_active =
        state == maya::Composer::State::Streaming ||
        state == maya::Composer::State::ExecutingTool;
    // Whenever a widget-paced animation is on screen the frame repaints at
    // ~30 fps, and the hardware caret's per-frame ?25l→move→?25h dance makes
    // cursor-animating terminals (WezTerm, Windows Terminal) replay their fade
    // on EVERY frame: a caret flicker (confirmed — AGENTTY_PAINTED_CARET=1
    // removes it). The painted block caret is a plain cell, immune to that.
    // Two on-screen animations drive this:
    //   • the welcome-screen letter bob (shown when the thread is empty), and
    //   • a live stream / tool run (agent_active).
    // Use the painted caret for both. Streaming already preferred painted
    // EXCEPT within the recent-typing window; drop that exception so a stream
    // right after a keystroke doesn't flicker. Idle without animation keeps the
    // native hardware caret + terminal-side blink.
    const bool welcome_bob = m.d.current.messages.empty();
    return !painted_caret_env
        && !welcome_bob
        && !agent_active
        && ui::panel::top(m) == ui::panel::Kind::None
        && m.ui.terminal_focused
        // Inside tmux, only claim the hardware caret when tmux will
        // actually FORWARD the cursor-style escape (its `cstyle`
        // feature). Without it our DECSCUSR is swallowed and the caret
        // silently reverts to whatever shape the outer terminal had —
        // the painted fallback is honest about that instead. Outside
        // tmux has_feature() is false for everything, so the check is
        // scoped to the tmux case only.
        && (!maya::tmux::active()
            || maya::tmux::has_feature(maya::tmux::Feature::CursorStyle));
}

maya::Composer::Config composer_config(const Model& m) {
    maya::Composer::Config cfg;
    auto disp = clip_long_lines(render_chips(m.ui.composer.text,
                                             m.ui.composer.cursor,
                                             m.ui.composer.attachments));
    cfg.text            = std::move(disp.text);
    cfg.cursor          = disp.cursor;
    cfg.state           = composer_state(m);
    cfg.active_color    = phase_color(m.s.phase);
    cfg.text_color      = fg;
    cfg.accent_color    = accent;
    cfg.warn_color      = warn;
    cfg.highlight_color = highlight;
    cfg.queued          = m.ui.composer.queued.size();
    // LOOP (^B): armed → ⟳ chip + tinted border. `looping()` (not the raw
    // flag) so an armed-but-empty payload can't paint a loop that will
    // never fire.
    cfg.loop            = m.ui.composer.looping();
    cfg.loop_iterations = m.ui.composer.loop_iterations;
    cfg.loop_wait_secs  = m.ui.composer.loop_wait_secs(
        maya::anim::default_clock().now_ms());
    cfg.loop_color      = role_brand_alt;   // distinct from the cyan queue chip
    // While looping the box is a readout of the prompt being re-sent, not an
    // editable draft (the reducer drops every mutating message), so render it
    // as read-only: dimmed text, parked caret.
    cfg.read_only       = cfg.loop;
    cfg.profile         = {.label = std::string{profile_label(m.d.profile)},
                           .color = profile_color(m.d.profile)};
    cfg.expanded        = m.ui.composer.expanded;

    // Footer-row left slot: the model · provider identity, relocated here
    // from the status bar. Two reasons it reads better in this seat:
    //
    //   • It is a property of the message you are about to SEND, so it
    //     belongs against the input, not in the turn-status strip that
    //     churns with phase / throughput / context while streaming.
    //   • The slot it takes over was spending itself on "↵ send · ⇧↵ /
    //     ⌥↵ newline" — two keys the user learns once and then never
    //     needs restated on every frame for the rest of the session.
    //
    // Setting a non-empty `status` displaces those hints in the widget;
    // show_key_hints=false makes that intent explicit so a future empty
    // badge (unknown model) leaves the slot blank rather than silently
    // resurrecting the hints.
    cfg.status          = model_badge_config(m);
    cfg.show_key_hints  = false;

    // ── Ambient counters over the REAL payload ──────────────────────
    //
    // cfg.text is the chip-rendered display string: a long paste or
    // @file collapses to a short caption, so counting words / tokens /
    // lines off it undercounts massively whenever an attachment
    // exists. Expand the attachment bodies back in (the same pass the
    // transport runs at submit time) and count off THAT so the live
    // meter reflects what actually goes to the model. Skip the expand
    // entirely when there are no attachments — the visible text is the
    // payload, and -1 lets the widget derive counts itself.
    if (!m.ui.composer.attachments.empty() && !m.ui.composer.text.empty()) {
        std::string full = attachment::expand(m.ui.composer.text,
                                               m.ui.composer.attachments);
        int words = 0;
        bool in_word = false;
        int lines = 1;
        for (char c : full) {
            const bool ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
            if (!ws && !in_word) { ++words; in_word = true; }
            else if (ws)         { in_word = false; }
            if (c == '\n') ++lines;
        }
        cfg.word_estimate  = words;
        cfg.token_estimate = static_cast<int>((full.size() + 3) / 4);
        cfg.line_estimate  = lines;
    }

    // Start as a single-row composer: the box hugs one line of input
    // until the user's own text wraps to a second row. A floor of 2
    // (former default) kept a permanent blank row for streaming
    // anti-flicker, but the single-row look is tighter and the
    // cache_id below already holds the box stable across streaming
    // frames, so the extra pinned row isn't needed to suppress the bob.
    cfg.min_body_rows   = 1;

    // Idle blink-stop: hand the widget the last-interaction timestamp so
    // it stops blinking the painted cursor 15 s after the user goes idle
    // (mirrors kitty's cursor_stop_blinking_after). Keeps an idle agentty
    // from driving the terminal compositor forever on GPU terminals with
    // an aggressive repaint_delay.
    cfg.last_edit_ms    = m.ui.composer.last_edit_ms;

    // ── Hardware caret ─────────────────────────────────────────
    // Use the terminal's REAL cursor as the composer caret: native
    // blink with zero idle wake-ups, IME popovers anchored at the true
    // cell, screen readers tracking the real position. Shown only when
    // ALL of these hold — each gate kills a real ghost-cursor report:
    //   • no overlay (palette/picker owns the keys — and now owns the
    //     caret too, via query_caret in its search header);
    //   • terminal window is FOCUSED (?1004) — no blinking bar in an
    //     inactive pane;
    //   • agent idle, OR the user typed within the last few seconds —
    //     while the agent streams, rows scroll into native scrollback
    //     under a shown cursor; if the user scrolls up to read (tmux
    //     copy-mode / scrollback freezes the visible screen) the
    //     re-aimed cursor renders over THREAD content — the "ghost
    //     cursor on the rail line" report. A reading user needs no
    //     caret; a typing one gets it back on the first keystroke
    //     (ComposerCharInput refreshes last_edit_ms), and the
    //     streaming tick re-evaluates this window every frame.
    // AGENTTY_PAINTED_CARET=1 opts out wholesale (broken-DECTCEM
    // terminals or users who prefer the block).
    // Read-only (loop mode) parks the caret: there is nothing to type into,
    // and a hardware caret would sit blinking in a buffer that ignores keys.
    cfg.hardware_caret = composer_uses_hardware_caret(m) && !cfg.read_only;
    // Rationale for the (!agent_active || typing_recently) gate — the
    // SAME rule with and without tmux:
    //   • idle (agent not streaming): the screen is STATIC, so tmux's
    //     copy-mode cursor tracks a stable position — no ghost. This
    //     is the resting state you spend most time in, and where the
    //     hardware caret's wins (your configured block/beam, native
    //     blink, IME anchoring) matter most. Caret ON.
    //   • streaming AND you've typed within 4 s: you're queueing a
    //     message and looking at the composer — caret ON (parked at
    //     the right margin, so a tmux scroll-up ghost lands on blank
    //     padding, not the thread rail; see maya's caret epilogue).
    //   • streaming AND idle-handed: rows scroll into scrollback; if
    //     you scroll up to read, a shown caret would track content.
    //     Caret OFF — painted fallback covers it.
    // Under tmux the copy-mode cursor is tmux's OWN, drawn at the pane-
    // cursor position regardless of DECTCEM and unremovable from the
    // pane side (probed empirically). The right-margin park + this gate
    // mean it only ever appears on blank padding, never mid-thread —
    // so tmux no longer needs a stricter rule than a bare terminal.
    // The typing window LAPSES without any model change — nothing would
    // re-run view() to flip the caret back (painted mode / park) until
    // the next keystroke or stream tick. Request one wake at the lapse
    // boundary so the flip happens on time. Idle-with-no-window frames
    // request nothing (zero-wake idle preserved).
    // Recomputed here rather than threaded out of
    // composer_uses_hardware_caret: this block needs the WINDOW, not the
    // caret decision, and duplicating two cheap comparisons is better than
    // widening that predicate's return type for one caller.
    {
        constexpr std::int64_t kTypingWindowMs = 4000;
        const bool agent_active =
            cfg.state == maya::Composer::State::Streaming ||
            cfg.state == maya::Composer::State::ExecutingTool;
        const bool typing_recently =
            m.ui.composer.last_edit_ms > 0 &&
            (maya::anim::default_clock().now_ms() - m.ui.composer.last_edit_ms)
                < kTypingWindowMs;
        if (cfg.hardware_caret && typing_recently && agent_active) {
            const auto until_lapse = kTypingWindowMs
                - (maya::anim::default_clock().now_ms() - m.ui.composer.last_edit_ms);
            if (until_lapse > 0)
                maya::anim::keep_animating_after(until_lapse + 30);
        }
    }

    // ── Cross-frame cache key (streaming anti-flicker) ───────────────
    //
    // During streaming the host re-runs view() on every delta; without
    // a stable identity the composer's whole box (border + divider +
    // width-adaptive hint component) re-lays-out each frame and the
    // hint row's 1-cell drift reads as flicker. Fold in EXACTLY the
    // inputs that change the rendered cells so the key holds constant
    // across the many streaming frames where the composer is visually
    // identical, and moves the instant any of them does (the user
    // types, the phase flips, the queue depth ticks, the profile
    // swaps). maya::Composer::build() only consults this while active
    // (steady cursor, no blink), so excluding the blink phase is safe.
    cfg.cache_id = maya::CacheIdBuilder{}
        .add(std::string_view{"agentty-composer"})
        .add(std::string_view{cfg.text})
        .add(static_cast<std::uint64_t>(cfg.cursor))
        .add(static_cast<std::uint64_t>(cfg.state))
        .add(cfg.active_color)
        .add(static_cast<std::uint64_t>(cfg.queued))
        // Loop mode changes the border hue AND adds a chip whose label
        // carries the iteration count — both must move the hash or an
        // auto-send would repaint nothing.
        .add(static_cast<std::uint64_t>(cfg.loop ? 1 : 0))
        .add(static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(cfg.loop_iterations)))
        // The countdown ticks every second and must repaint with it.
        .add(static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(cfg.loop_wait_secs)))
        .add(std::string_view{cfg.profile.label})
        .add(cfg.profile.color)
        .add(static_cast<std::uint64_t>(cfg.expanded ? 1 : 0))
        // The footer's model · provider chip is an opaque Element to the
        // cache, so hash the FACTS behind it — otherwise switching model
        // or provider mid-session produces a composer the gate scores as
        // visually identical and the chip keeps naming the old model
        // until some other hashed axis happens to move.
        .add(std::string_view{m.d.model_id.value})
        .add(std::string_view{m.s.smart_turn_model})
        .add(provider::active().provider_id())
        .add(static_cast<std::uint64_t>(cfg.hardware_caret ? 1 : 0))
        .add(static_cast<std::uint64_t>(cfg.min_body_rows))
        .add(static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(cfg.token_estimate)))
        .add(static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(cfg.word_estimate)))
        .add(static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(cfg.line_estimate)))
        .build();
    return cfg;
}

} // namespace agentty::ui
