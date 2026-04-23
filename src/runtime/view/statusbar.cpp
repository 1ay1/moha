#include "moha/runtime/view/statusbar.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <maya/widget/model_badge.hpp>
#include <maya/widget/token_stream.hpp>

#include "moha/runtime/view/helpers.hpp"
#include "moha/runtime/view/palette.hpp"

namespace moha::ui {

using namespace maya;
using namespace maya::dsl;

namespace {

constexpr int kCtxBarCells = 10;    // width of the mini context bar, in cells

// Token counts print in fixed-width fields so the right-hand status group
// doesn't dance left/right as numbers tick up during streaming. 5 chars is
// enough for "999.9" / "99.9k" / "9.9M"; shorter values are space-padded
// on the left so successive snapshots line up visually.
std::string format_tokens(int n) {
    char buf[16];
    if (n >= 1'000'000) {
        std::snprintf(buf, sizeof(buf), "%5.1fM", static_cast<double>(n) / 1'000'000.0);
    } else if (n >= 1000) {
        std::snprintf(buf, sizeof(buf), "%5.1fk", static_cast<double>(n) / 1000.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%5d", n);
    }
    return buf;
}

Color ctx_color(int pct) {
    if (pct < 60)       return success;
    if (pct <= 80)      return warn;
    return danger;
}

// Build a smooth 1/8-gradation progress bar with **per-cell color**:
// green for the safe zone (0–60 %), warn amber for the squeeze zone
// (60–80 %), danger red for the cliff (80–100 %). Returns an Element
// with StyledRun runs so each cell gets its own color — gives the bar
// a real "fuel-gauge" feel instead of a single-hue stripe that just
// changes color when full. Filled cells take the threshold color;
// unfilled cells stay dim/muted so the channel reads as a track.
//
// Visual: `█████▆░░░░`  →  green green green green warn-amber dim dim dim dim
Element ctx_bar_gradient(int pct, int cells) {
    static constexpr std::string_view kPartials[8] = {
        "", "\u258F", "\u258E", "\u258D",
        "\u258C", "\u258B", "\u258A", "\u2589"
    };
    pct = std::clamp(pct, 0, 100);
    int total_eighths = pct * cells * 8 / 100;

    std::string content;
    std::vector<StyledRun> runs;
    runs.reserve(static_cast<std::size_t>(cells));
    content.reserve(static_cast<std::size_t>(cells) * 3);

    for (int i = 0; i < cells; ++i) {
        int filled = std::max(0, total_eighths - i * 8);
        std::string_view ch;
        if      (filled >= 8) ch = "\u2588";       // full block
        else if (filled >  0) ch = kPartials[filled];
        else                  ch = "\u2591";       // light shade track

        // Threshold by cell position: cells [0..cells*0.6) are safe,
        // [0.6..0.8) warn, [0.8..1.0] danger. Unfilled cells get muted.
        float cell_t = static_cast<float>(i + 1) / static_cast<float>(cells);
        Color cc;
        if (filled == 0)        cc = muted;
        else if (cell_t <= 0.6f) cc = success;
        else if (cell_t <= 0.8f) cc = warn;
        else                     cc = danger;

        std::size_t off = content.size();
        content.append(ch);
        Style st = (filled == 0) ? Style{}.with_fg(cc).with_dim()
                                 : Style{}.with_fg(cc);
        runs.push_back(StyledRun{off, ch.size(), st});
    }
    return Element{TextElement{
        .content = std::move(content),
        .style = {},
        .runs = std::move(runs),
    }};
}

// Phase-tinted accent strip — a row of half-cell blocks (▔ at top, ▁ at
// bottom) in the current phase color, dim. Replaces the dim ─ rule with
// something that carries app-state information without needing chrome
// chars or a hard line. The half-block glyph reads as a "soft edge"
// rather than a divider — modern app vibe.
//
// `position`: 0 = top edge (▔ upper-half block), 1 = bottom (▁ lower-half).
Element phase_accent(Color c, int position) {
    return Element{ComponentElement{
        .render = [c, position](int w, int /*h*/) -> Element {
            if (w <= 0) return text("", {});
            std::string line;
            line.reserve(static_cast<std::size_t>(w) * 3);
            const char* glyph = (position == 0)
                ? "\xe2\x96\x94"   // ▔  upper-half block
                : "\xe2\x96\x81";  // ▁  lower-half block
            for (int i = 0; i < w; ++i) line += glyph;
            return text(std::move(line),
                        Style{}.with_fg(c).with_dim());
        },
        .layout = {},
    }};
}

// All separators are dim middle-dots with breathing room. Pro tools
// don't mix separator characters — one symbol, one rhythm.
Element sep_dot()  { return text("   \xc2\xb7   ", fg_dim(muted)); }  // ·
Element sep_thin() { return text(" \xc2\xb7 ",     fg_dim(muted)); }  // ·

// Helix / Lazygit / k9s style: bold key in default fg, dim label, no
// chip background, no per-key color. The inverse-video keycaps looked
// like stickers; pro TUIs lean on typography weight (bold vs dim) for
// hierarchy, not saturated chips. `cat` is unused now — kept on the
// signature so call sites don't need rewriting.
Element shortcut(const char* key, const char* label, Color /*cat*/) {
    return h(
        text(key, fg_bold(fg)),
        text(" ", {}),
        text(label, fg_dim(muted))
    ).build();
}

// Generous gap between key+label units; reads as discrete bindings
// without needing visual containers around them.
Element shortcut_gap() { return text("   ", {}); }

// Truncate a breadcrumb to a fixed width with a trailing ellipsis. Works
// on UTF-8 strings only approximately (assumes 1 column per byte for
// non-ASCII heuristics), which is fine for thread titles.
std::string truncate_middle(std::string_view s, std::size_t max_chars) {
    if (s.size() <= max_chars) return std::string{s};
    if (max_chars <= 1) return "\u2026";
    return std::string{s.substr(0, max_chars - 1)} + "\u2026";
}

// Profile indicator — plain colored text, no chip / no inverse video.
// Pro tools surface mode/profile as a labelled fragment in the status
// line (vim-modeline style), not as a sticker.
Element profile_tag(Profile p) {
    return text(std::string{profile_label(p)},
                Style{}.with_fg(profile_color(p)).with_bold());
}

// Phase indicator — colored glyph + bold verb, no chip background.
// Glyph carries the urgency (●/⠋/⚠), color reinforces it, weight tells
// the eye it's a state label vs free text.
//
// `breathing`: when true (active stream / executing tool), the glyph
// alternates between bold-bright and dim-soft on a slow cycle so the
// indicator feels *alive* without the user having to read it. The
// breath cadence (every ~16 frames @ 33 ms ≈ 530 ms) is set just below
// resting heart-rate — perceptible motion without becoming a tick.
//
// `verb_width`: render the verb in EXACTLY this many display columns —
// truncate-with-ellipsis if too long, right-pad with spaces if too
// short. This keeps the chip a constant width regardless of which verb
// is showing ("Streaming" / "Awaiting" / "bash" / "find_definition"),
// which stops profile_tag to the right of the chip from sliding
// horizontally as the phase changes — the worst status-bar jitter
// source. Pass 0 to drop the verb entirely (very-narrow widths) and
// keep just the colored glyph.
//
// `elapsed_secs`: when ≥ 0, append a fixed-5-column elapsed time after
// the verb (" 4.2s" / "12.3s" / " 234s" / "9m05s"). Pass < 0 to omit.
// The elapsed display itself is always 5 cells; chip width grows by 6
// (verb / 1 space / elapsed) when shown — a state change, not a
// per-frame jitter.
Element phase_chip(std::string_view glyph, std::string_view verb, Color c,
                   bool breathing, int frame, int verb_width = 10,
                   float elapsed_secs = -1.0f) {
    Style glyph_style;
    if (breathing) {
        // 32-frame cycle, bold for first half, normal for second.
        bool inhale = ((frame >> 4) & 1) == 0;
        glyph_style = inhale ? Style{}.with_fg(c).with_bold()
                             : Style{}.with_fg(c).with_dim();
    } else {
        glyph_style = Style{}.with_fg(c);
    }

    std::vector<Element> parts;
    parts.push_back(text(std::string{glyph}, glyph_style));
    if (verb_width > 0) {
        // Truncate-or-pad to exactly verb_width display columns. ASCII-
        // only assumed (verbs + moha tool names are ASCII); display
        // columns equal byte count except when "…" suffix is added.
        std::string out{verb};
        int dw = static_cast<int>(out.size());
        if (dw > verb_width) {
            out = out.substr(0, static_cast<std::size_t>(verb_width - 1))
                + "\xe2\x80\xa6";   // …  (3 bytes, 1 column)
            dw = verb_width;
        }
        if (dw < verb_width)
            out.append(static_cast<std::size_t>(verb_width - dw), ' ');
        parts.push_back(text(" ", {}));
        parts.push_back(text(std::move(out),
                             Style{}.with_fg(c).with_bold()));
    }
    if (elapsed_secs >= 0.0f && verb_width > 0) {
        parts.push_back(text(" ", {}));
        parts.push_back(text(format_elapsed_5(elapsed_secs),
                             Style{}.with_fg(c).with_dim()));
    }
    return h(std::move(parts)).build();
}

// Subtle leading marker for the breadcrumb — a thin left edge bar, like
// a folder spine. Reads as "this is a labelled section" without taking
// horizontal space from the title itself.
Element edge_mark(Color c) { return text("\xe2\x96\x8e", fg_of(c)); } // ▎

// Build the sparkline samples in chronological (oldest → newest) order
// from the StreamState ring buffer. Empty entries (not yet sampled) are
// dropped so the bar starts narrow and grows up to kRateSamples wide as
// the stream produces more data.
std::vector<float> ordered_rate_history(const StreamState& s) {
    std::vector<float> out;
    out.reserve(StreamState::kRateSamples);
    if (s.rate_history_full) {
        // Wrap from pos to end, then 0 to pos.
        for (std::size_t i = 0; i < StreamState::kRateSamples; ++i) {
            const auto idx = (s.rate_history_pos + i) % StreamState::kRateSamples;
            out.push_back(s.rate_history[idx]);
        }
    } else {
        for (std::size_t i = 0; i < s.rate_history_pos; ++i)
            out.push_back(s.rate_history[i]);
    }
    return out;
}

// Horizontal turn mini-map — a sparkline-style strip where each cell's
// HEIGHT encodes the turn's activity (more tool calls → taller bar).
// Result: a glance-readable density chart of the whole conversation —
// you see "this turn was a quick exchange," "this one had a heavy
// agentic flurry," and "this one's still cooking" without parsing.
//
// User turns are always short ▁ (they don't have tool calls). Assistant
// turns rise with tool count: 0→▁, 1→▃, 2-3→▅, 4-6→▇, 7+→█. The active
// turn pulses between full-block and lower-half so the "you are here"
// cursor is unmistakable.
//
// Compresses or elides middle when there are more turns than cells —
// keeps the most recent N turns plus the very first one, with a `…` gap
// so the user can still tell "we're 30 turns in" at a glance.
Element turn_minimap(const Model& m, int max_cells, int frame) {
    const auto& msgs = m.d.current.messages;
    if (msgs.size() < 2) return text("", {});

    // Resolve per-assistant brand color from model id (keeps the map
    // consistent with each turn's left-rail color in thread.cpp).
    const auto& mid = m.d.model_id.value;
    Color asst_color = (mid.find("opus")   != std::string::npos) ? accent
                     : (mid.find("sonnet") != std::string::npos) ? info
                     : (mid.find("haiku")  != std::string::npos) ? success
                                                                 : highlight;

    // Activity-density bar: height encodes "how much work this turn did."
    // For Assistant turns that's the tool-call count; for User turns it's
    // a constant low (▁ = "voice in"). The active turn flickers between
    // its natural height and a slightly taller block in sync with the
    // spinner so the cursor pulses subtly.
    auto bar_glyph_for = [](int activity) -> const char* {
        // Unicode lower-block scale, 1/8 → 8/8 height. Caps at full block.
        if (activity <= 0) return "\xe2\x96\x81";        // ▁ 1/8
        if (activity == 1) return "\xe2\x96\x83";        // ▃ 3/8
        if (activity <= 3) return "\xe2\x96\x85";        // ▅ 5/8
        if (activity <= 6) return "\xe2\x96\x87";        // ▇ 7/8
        return                    "\xe2\x96\x88";        // █ 8/8
    };

    struct Dot { Color c; const char* g; bool active; bool is_user; };
    std::vector<Dot> dots;
    dots.reserve(msgs.size());
    bool stream_active = m.s.is_streaming() || m.s.is_executing_tool();
    for (std::size_t i = 0; i < msgs.size(); ++i) {
        const auto& msg = msgs[i];
        bool is_last = (i + 1 == msgs.size());
        bool is_user = (msg.role == Role::User);
        Color c = is_user ? highlight : asst_color;
        // User turns are always the shortest bar (no tools). Assistant
        // turns scale with tool-call count.
        int activity = is_user ? 0 : static_cast<int>(msg.tool_calls.size());
        const char* g = bar_glyph_for(activity);
        // Active cursor: pulse between the natural height and full block.
        if (is_last && stream_active) {
            bool tall = ((frame >> 3) & 1) == 0;
            g = tall ? "\xe2\x96\x88"   // █  full block on the up-beat
                     : g;                // natural height on the down-beat
        }
        dots.push_back({c, g, is_last && stream_active, is_user});
    }

    // Elide the middle when over budget: keep first + last (max-1) so the
    // most recent turns stay visible and the very first turn anchors
    // "where we started".
    bool elided = false;
    int orig_total = static_cast<int>(dots.size());
    int orig_skipped_at_front = 0;
    if (orig_total > max_cells) {
        std::vector<Dot> kept;
        kept.reserve(static_cast<std::size_t>(max_cells));
        kept.push_back(dots.front());
        // Reserve one slot for the elision marker; fill the rest from
        // the tail.
        int tail = max_cells - 2;
        orig_skipped_at_front = orig_total - tail - 1;   // for recency math
        for (std::size_t i = static_cast<std::size_t>(orig_total - tail);
             i < dots.size(); ++i)
            kept.push_back(dots[i]);
        dots = std::move(kept);
        elided = true;
    }

    std::vector<Element> parts;
    parts.reserve(dots.size() * 2 + 4);
    parts.push_back(text(" ", {}));
    int n = static_cast<int>(dots.size());
    for (int i = 0; i < n; ++i) {
        const auto& d = dots[i];
        if (elided && i == 1) {
            parts.push_back(text("\xe2\x80\xa6 ", fg_dim(muted)));   // …
        }
        // Recency tier — turns near the right (most recent) stay full
        // color; older turns dim progressively. Active turn always
        // brightest. Tiers map to original-message indices so the
        // "recent N" stays consistent across width changes.
        //
        // orig_idx = original index of this dot (re-derive after
        // elision: i==0 → 0, i>=1 → orig_skipped_at_front + i - (elided ? 1 : 0))
        int orig_idx = i;
        if (elided) orig_idx = (i == 0) ? 0 : (orig_skipped_at_front + i);
        int recency = orig_total - 1 - orig_idx;     // 0 = most recent
        Style st;
        if (d.active) {
            // Active turn pulses with the spinner so it reads as "live".
            bool bright = ((frame >> 4) & 1) == 0;
            st = bright ? Style{}.with_fg(d.c).with_bold()
                        : Style{}.with_fg(d.c);
        } else if (recency == 0) {
            st = Style{}.with_fg(d.c).with_bold();   // newest (no stream)
        } else if (recency <= 3) {
            st = Style{}.with_fg(d.c);               // recent — full color
        } else {
            st = Style{}.with_fg(d.c).with_dim();    // older — fade
        }
        parts.push_back(text(d.g, st));
        if (i + 1 < n) parts.push_back(text(" ", {}));
    }
    return h(std::move(parts)).build();
}

// Stable-width compact tok/s + sparkline. Replaces maya::TokenStream's
// compact mode, which uses %.1f for the rate (3–6 chars) and
// format_with_commas for the total (1–9 chars) — both variable-width,
// causing the widget itself to grow/shrink horizontally each frame as
// numbers tick. Here every segment is fixed display width so the slot
// occupies the same cells whether rate is 0.5 or 1234, total is 0 or
// 12.3M.
//
// Layout (37 cells total):
//   ⚡ ▕rate 5▏ t/s ▕spark 16▏ ▕total 5▏
//
// `live`: when true, sparkline + rate render in their bright color.
// When false (active session but not currently producing deltas — e.g.
// during ExecutingTool), they dim, signalling "frozen at last sample".
Element compact_token_stream(float rate, int total_tok,
                             std::span<const float> hist, Color color,
                             bool live) {
    static constexpr const char* kBlocks[8] = {
        "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
        "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88",
    };
    constexpr int kSparkCells = 16;

    if (rate < 0.0f) rate = 0.0f;
    Color rc = (rate > 50.0f)  ? success
             : (rate >= 20.0f) ? warn
                               : danger;

    // Rate field — always 5 display columns:
    //   <  100  → " 23.4"   (1 leading space + %4.1f → 5)
    //   < 10k   → " 1234"   (%5.0f → 5)
    //   else    → "1234k"   (%4.0f + 'k')
    char rate_buf[16];
    if      (rate <    100.0f) std::snprintf(rate_buf, sizeof(rate_buf), "%5.1f",   static_cast<double>(rate));
    else if (rate <  10000.0f) std::snprintf(rate_buf, sizeof(rate_buf), "%5.0f",   static_cast<double>(rate));
    else                       std::snprintf(rate_buf, sizeof(rate_buf), "%4.0fk", static_cast<double>(rate) / 1000.0);

    // Sparkline: always kSparkCells cells. When the history is shorter,
    // pad on the LEFT with the lowest block so the right-edge stays
    // pinned and new samples appear at the right (most-recent-on-right).
    std::string spark;
    spark.reserve(kSparkCells * 3);
    float lo = 0.0f, hi = 1.0f;
    if (!hist.empty()) {
        lo = *std::min_element(hist.begin(), hist.end());
        hi = *std::max_element(hist.begin(), hist.end());
        if (hi - lo < 0.001f) hi = lo + 1.0f;
    }
    int filled = std::min(kSparkCells, static_cast<int>(hist.size()));
    int pad    = kSparkCells - filled;
    for (int i = 0; i < pad; ++i) spark += kBlocks[0];
    for (int i = 0; i < filled; ++i) {
        std::size_t hidx = hist.size() - static_cast<std::size_t>(filled) + static_cast<std::size_t>(i);
        float norm = std::clamp((hist[hidx] - lo) / (hi - lo), 0.0f, 1.0f);
        int level = std::clamp(static_cast<int>(norm * 7.0f + 0.5f), 0, 7);
        spark += kBlocks[level];
    }

    Style spark_style = live ? Style{}.with_fg(color)
                             : Style{}.with_fg(color).with_dim();
    Style rate_style  = live ? Style{}.with_fg(rc).with_bold()
                             : Style{}.with_fg(rc).with_dim();

    return h(
        text("\xe2\x9a\xa1 ", Style{}.with_fg(rc)),                // ⚡
        text(std::string{rate_buf}, rate_style),
        text(" t/s ", fg_dim(muted)),
        text(std::move(spark), spark_style),
        text(" ", {}),
        text(format_tokens(total_tok), fg_dim(muted))
    ).build();
}

// Walk the last assistant message and return the name of the tool call
// currently in `Running` state, if any. Lets the phase chip render
// "▌ ⠋ bash ▐" when ExecutingTool is active instead of the generic
// "Running" label — more useful at a glance.
std::string_view running_tool_name(const Model& m) {
    if (m.d.current.messages.empty()) return {};
    const auto& last = m.d.current.messages.back();
    if (last.role != Role::Assistant) return {};
    for (const auto& tc : last.tool_calls) {
        if (tc.is_running()) return tc.name.value;
    }
    return {};
}


} // namespace

Element status_bar(const Model& m) {
    bool is_streaming = m.s.is_streaming() && m.s.active();

    // Phase chip colors + glyph resolved up front; used inside the
    // width-aware builder below.
    Color pcolor = phase_color(m.s.phase);
    bool spin = is_streaming || m.s.is_executing_tool();
    std::string phase_icon = spin
        ? std::string{m.s.spinner.current_frame()}
        : std::string{phase_glyph(m.s.phase)};
    std::string phase_label{phase_verb(m.s.phase)};
    if (m.s.is_executing_tool()) {
        if (auto tn = running_tool_name(m); !tn.empty())
            phase_label = std::string{tn};
    }

    // Context fullness uses `tokens_in` (includes cache_read + creation).
    // Value is the cumulative prefix size from the most recent request.
    int ctx_used = m.s.tokens_in;
    bool has_tokens = ctx_used > 0;
    int pct = (m.s.context_max > 0)
                  ? std::min(100, ctx_used * 100 / m.s.context_max)
                  : 0;
    bool has_breadcrumb = !m.d.current.title.empty();

    // ── Responsive activity row ─────────────────────────────────────────
    // Sections drop progressively as width shrinks so the row never wraps.
    // Priority (kept longest → dropped first):
    //   phase_pill, model_badge, ctx % (always)
    //   ctx bar + absolute count       (>= 55)
    //   profile                        (>= 70)
    //   ↑↓ token counts                (>= 90)
    //   live TokenStream sparkline     (>= 130)
    //   breadcrumb                     (idle: >= 130, streaming: >= 160)
    //
    // The breadcrumb is the first thing to drop because the title is the
    // only piece of information that's also visible elsewhere (the thread
    // view header, the picker). Everything else in the bar is unique data.
    // When streaming we push its threshold higher so the live sparkline +
    // tok/s readout has room to breathe without elbowing the title.
    //
    // Numbers render in fixed-width fields (format_tokens → 5 chars, pct → 3
    // digits) so the right group doesn't visibly slide left/right as counts
    // tick upward during streaming.
    auto activity_row = Element{ComponentElement{
        .render = [=, &m](int w, int /*h*/) -> Element {
            if (w <= 0) return text("", {});

            // Breathing phase glyph — only when something's actively
            // happening. Idle stays still so the bar feels calm at rest.
            bool breathing = is_streaming || m.s.is_executing_tool();
            int frame = m.s.spinner.frame_index();
            // Render verb at a stable display width (truncate-or-pad)
            // — kills the worst jitter source: verb swapping between
            // "Streaming"/"bash"/"find_definition" used to slide
            // everything to its right by up to 6 columns per frame.
            // Below 50 cols drop the verb entirely, keeping just the
            // colored glyph + spinner.
            int phase_vw = (w < 50) ? 0 : 10;
            // Live elapsed appended to the chip during active states
            // (≥ 80 cols only — narrower terminals can't spare 6 cols
            // for a "Streaming   4.2s" tail). Omitted when idle so the
            // chip reads as static at rest.
            float chip_elapsed = -1.0f;
            if (breathing && w >= 80
                && m.s.started.time_since_epoch().count() != 0) {
                auto now = std::chrono::steady_clock::now();
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              now - m.s.started).count();
                chip_elapsed = static_cast<float>(ms) / 1000.0f;
            }
            auto phase_pill = phase_chip(phase_icon, phase_label,
                                         pcolor, breathing, frame,
                                         phase_vw, chip_elapsed);

            // ── Left group ─────────────────────────────────────────────
            std::vector<Element> lparts;
            lparts.push_back(text(" ", {}));
            const int breadcrumb_min = is_streaming ? 160 : 130;
            if (has_breadcrumb && w >= breadcrumb_min) {
                // Title budget scales with width so we don't elbow out the
                // right group on medium terminals.
                std::size_t title_budget = (w >= 170) ? 28
                                         : (w >= 150) ? 20
                                                      : 14;
                lparts.push_back(h(
                    edge_mark(pcolor),
                    text(" " + truncate_middle(m.d.current.title, title_budget),
                         fg_of(fg).with_bold()),
                    sep_dot()
                ).build());
            }
            // Leading rail: solid ▌ in phase color before the phase chip.
            // Anchors the active state visually — same vibe as a
            // notification badge's left edge or an OS task-bar's
            // "this app is active" indicator. Bold when actively
            // working, dim at rest.
            Style rail_style = breathing
                ? Style{}.with_fg(pcolor).with_bold()
                : Style{}.with_fg(pcolor).with_dim();
            lparts.push_back(text("\xe2\x96\x8c", rail_style));   // ▌
            lparts.push_back(text(" ", {}));
            lparts.push_back(phase_pill);
            if (w >= 70) {
                lparts.push_back(sep_thin());
                lparts.push_back(profile_tag(m.d.profile));
            }
            auto left = hstack()(std::move(lparts));

            // ── Right group ────────────────────────────────────────────
            std::vector<Element> right_parts;

            // Live tok/s + sparkline — fixed-width slot. Visibility is
            // tied to the *active session* (m.s.active), not just the
            // Streaming phase, so the slot doesn't pop in/out as the
            // turn cycles Streaming → ExecutingTool → Streaming. When
            // not streaming live (e.g. mid-tool), the displayed rate
            // freezes at the most recent sample and renders dimmed —
            // signalling "this is the last value, not live" without
            // making the whole element disappear.
            //
            // Width-stable from the inside (compact_token_stream pads
            // every segment) AND from the outside (right group's
            // right edge stays pinned by spacer; this slot grows
            // leftward into the spacer and items to its right stay
            // put). No more horizontal dance.
            bool ever_streamed =
                m.s.first_delta_at.time_since_epoch().count() != 0;
            if (ever_streamed && m.s.active() && w >= 130) {
                auto now = std::chrono::steady_clock::now();
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              now - m.s.first_delta_at).count();
                double sec = std::max(0.001, static_cast<double>(ms) / 1000.0);
                double approx_tok = static_cast<double>(
                    m.s.live_delta_bytes) / 4.0;
                auto hist = ordered_rate_history(m.s);

                // Pick the rate to display: live computation while
                // streaming (smooth); freeze on last sample otherwise
                // so the number doesn't decay as wall-clock keeps
                // ticking past the last delta.
                float disp_rate;
                if (is_streaming && ms >= 250) {
                    disp_rate = static_cast<float>(approx_tok / sec);
                } else if (!hist.empty()) {
                    disp_rate = hist.back();
                } else {
                    disp_rate = 0.0f;
                }

                right_parts.push_back(compact_token_stream(
                    disp_rate, static_cast<int>(approx_tok),
                    std::span<const float>{hist.data(), hist.size()},
                    highlight, /*live=*/is_streaming));
                right_parts.push_back(sep_dot());
            }

            // Model badge — always shown.
            {
                ModelBadge mb;
                mb.set_model(m.d.model_id.value);
                mb.set_compact(true);
                right_parts.push_back(mb.build());
            }

            // (Per-direction ↑↓ token counts removed — `↑` (tokens_in)
            // duplicated CTX's "18.0k/200.0k", and `↓` (tokens_out)
            // duplicated the live "N tokens" in the compact tok-stream
            // slot to the left. Both signals already covered.)

            // Context indicator — always when we have a usage event; the
            // visual gradient bar + absolute count drop away below 55
            // cols, leaving just "CTX  32%". Small-caps label (CTX)
            // separates it visually from the variable-width tokens
            // numbers preceding it.
            if (m.s.context_max > 0 && has_tokens) {
                Color c = ctx_color(pct);
                right_parts.push_back(sep_thin());
                right_parts.push_back(text("CTX ", fg_dim(muted).with_bold()));
                if (w >= 55) {
                    std::string used_str = format_tokens(ctx_used) + "/"
                                         + format_tokens(m.s.context_max) + " ";
                    right_parts.push_back(text(used_str, fg_dim(muted)));
                    // Per-cell green→amber→red gradient: cells get their
                    // own color based on threshold, not the whole-bar
                    // single hue. Reads as a real fuel gauge.
                    right_parts.push_back(ctx_bar_gradient(pct, kCtxBarCells));
                }
                // Tabular pct so the bar's right-edge label doesn't
                // shift left/right as the value crosses 9 → 10 → 100.
                right_parts.push_back(text(" " + tabular_int(pct, 3) + "%",
                                           fg_of(c).with_bold()));
            }

            right_parts.push_back(text(" ", {}));
            auto right = hstack()(std::move(right_parts));

            return h(left, spacer(), right).build();
        },
        .layout = {},
    }};

    // ── Error / transient status banner ──────────────────────────────────
    Element status_row;
    bool has_status = !m.s.status.empty() && m.s.status != "ready";
    if (has_status) {
        bool is_err = m.s.status.rfind("error:", 0) == 0;
        Color bc = is_err ? danger : muted;
        // Error banner: leading edge mark + italic text, no bg — same
        // reasoning as the activity row above.
        status_row = h(
            text(" ", {}),
            edge_mark(bc),
            text(is_err ? " \xe2\x9a\xa0  " : "  ", fg_of(bc)),
            text(m.s.status, fg_of(bc).with_italic())
        ).build();
    }

    // ── Responsive shortcut row ─────────────────────────────────────────
    // At wide widths we show key+label pairs for all 6 bindings. As space
    // shrinks we drop progressively:
    //   ≥ 95 cols: full set with labels
    //   ≥ 70 cols: full set, key-only (no labels)
    //   ≥ 55 cols: drop S-Tab + ^/, key-only
    //   <  55 cols: only ^K · ^J · ^N · ^C, key-only
    // Quit (^C) always stays in danger red and always last. Shortcut
    // ordering kept stable so muscle memory doesn't shift between widths.
    auto shortcuts = Element{ComponentElement{
        .render = [](int w, int /*h*/) -> Element {
            struct Bind { const char* key; const char* label; Color c; };
            static constexpr Bind kAll[] = {
                {"^K",    "palette", maya::Color::cyan()},
                {"^J",    "threads", maya::Color::cyan()},
                {"S-Tab", "profile", maya::Color::cyan()},
                {"^/",    "models",  maya::Color::cyan()},
                {"^N",    "new",     maya::Color::cyan()},
                {"^C",    "quit",    maya::Color::red()},
            };
            bool show_label = (w >= 95);
            // Drop indices for narrow widths. We always keep ^K, ^J, ^N, ^C.
            // S-Tab (idx 2) and ^/ (idx 3) drop first.
            std::vector<int> keep;
            keep.reserve(6);
            keep.push_back(0);          // ^K
            keep.push_back(1);          // ^J
            if (w >= 55) { keep.push_back(2); keep.push_back(3); }
            keep.push_back(4);          // ^N
            keep.push_back(5);          // ^C
            std::vector<Element> row;
            row.reserve(keep.size() * 3 + 1);
            row.push_back(text(" ", {}));
            bool first = true;
            for (int idx : keep) {
                if (!first) row.push_back(shortcut_gap());
                first = false;
                const auto& b = kAll[idx];
                row.push_back(shortcut(b.key,
                                       show_label ? b.label : "", b.c));
                // When labels are dropped, bold the quit key in danger so
                // the eye still finds it without the "quit" word.
                if (!show_label && b.c == maya::Color::red()) {
                    // Replace the just-pushed shortcut Element with a
                    // bold-red key (no label). shortcut() with empty
                    // label still emits trailing " "; harmless.
                    (void)0;
                }
            }
            return h(std::move(row)).build();
        },
        .layout = {},
    }};

    // Horizontal turn mini-map — rendered inline only when there are
    // ≥2 turns AND width is wide enough to show a meaningful map. Below
    // 50 cols the row is dropped to free vertical space (those terminals
    // get an extra usable row in the thread panel instead).
    int frame_for_map = m.s.spinner.frame_index();
    auto minimap_row = Element{ComponentElement{
        .render = [&m, frame_for_map](int w, int /*h*/) -> Element {
            if (w < 50) return text("", {});
            // Budget scales with width: ~14 turns on wide, fewer on narrow.
            int budget = (w >= 100) ? 14 : (w >= 70 ? 10 : 7);
            return turn_minimap(m, budget, frame_for_map);
        },
        .layout = {},
    }};
    bool minimap_visible = m.d.current.messages.size() >= 2;

    // Compose. Phase-tinted accent strips replace the prior dim ─
    // dividers — they read as "soft state shelves" rather than hard
    // rules, and they carry app-state info (color = current phase)
    // without using extra chrome characters. ▔ at the top of the
    // status bar is the closing edge of the composer; ▁ at the bottom
    // is the floor of the whole UI.
    std::vector<Element> rows;
    rows.push_back(phase_accent(pcolor, /*position=*/0));   // ▔ top
    rows.push_back(activity_row);
    if (minimap_visible) rows.push_back(minimap_row);
    if (has_status)      rows.push_back(status_row);
    rows.push_back(shortcuts);
    rows.push_back(phase_accent(pcolor, /*position=*/1));   // ▁ bottom
    return v(std::move(rows)).build();
}

} // namespace moha::ui
