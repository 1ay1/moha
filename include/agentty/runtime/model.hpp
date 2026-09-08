#pragma once
// agentty::Model — the composed application state.
//
// This header imports each domain it aggregates and adds the UI-only
// sub-states that don't belong to any domain (composer, pickers, palette,
// modals).  Update / view code reach for domain-specific headers directly;
// only the runtime glue needs the full composite.

#include <optional>
#include <set>
#include <string>
#include <vector>

#include <maya/core/scroll_state.hpp>
#include <maya/element/element.hpp>
#include <maya/render/scrollback_ledger.hpp>

#include "agentty/domain/catalog.hpp"
#include "agentty/domain/smart_mode.hpp"
#include "agentty/domain/conversation.hpp"
#include "agentty/diff/diff.hpp"
#include "agentty/domain/id.hpp"
#include "agentty/domain/profile.hpp"
#include "agentty/domain/session.hpp"
#include "agentty/domain/todo.hpp"
#include "agentty/runtime/panel/slot.hpp"   // ui::panel::State (the exclusive slot)
#include "agentty/mcp/plugin_model.hpp"    // mcp::PluginModel (owned in the Model)
#include "agentty/runtime/composer_attachment.hpp"
#include "agentty/runtime/login.hpp"
#include "agentty/runtime/panel/common.hpp"
#include "agentty/runtime/view/cache.hpp"

namespace agentty {

// ============================================================================
// UI sub-states — one concern each, declared next to the Model that owns them
// ============================================================================

struct ComposerState {
    /// One queued, not-yet-sent message. Carries the same `text` +
    /// `attachments` pair the live composer does so a paste / @file
    /// chip that got queued (because the agent was busy) survives
    /// recall and resend as a chip rather than getting linearised
    /// into an inline blob the moment it left the composer. The
    /// fields here name-mirror ComposerState's text+attachments so a
    /// queue cycle is a structural swap, not a one-way collapse.
    struct QueuedMessage {
        std::string             text;
        std::vector<Attachment> attachments;
    };

    std::string text;
    int  cursor   = 0;
    bool expanded = false;
    std::vector<QueuedMessage> queued;
    /// Long pastes and @file picks live here as out-of-band bodies; the
    /// composer text holds a placeholder token (\x01ATT:N\x01) per
    /// attachment so cursor math and word-wrap stay plain-string.
    /// Submit-time expansion (attachment::expand) substitutes each
    /// placeholder with its body so the model sees the full bytes.
    std::vector<Attachment> attachments;

    /// Undo / redo. Each Snapshot is the WHOLE composer payload — text,
    /// cursor, attachments — captured before a mutating op. Cap depth
    /// at 64 entries; older snapshots are dropped FIFO. New edits clear
    /// the redo stack (standard editor semantics: branching from
    /// mid-history discards the old future).
    struct Snapshot {
        std::string             text;
        int                     cursor = 0;
        std::vector<Attachment> attachments;
    };
    std::vector<Snapshot> undo_stack;
    std::vector<Snapshot> redo_stack;
    /// True while a run of consecutive self-inserting keystrokes is
    /// being coalesced into ONE undo unit (see push_undo). Any
    /// non-typing op (paste, delete, cursor move, chip insert, submit,
    /// undo/redo) sets this false so the next typing run starts a
    /// fresh snapshot — Ctrl+Z then rewinds word-runs, not characters.
    bool undo_coalescing = false;

    /// What the composer is currently SHOWING. Exactly one of three
    /// things, so it is spelled as a variant rather than as two integers
    /// that have to be kept mutually exclusive by hand:
    ///
    ///   Live        the user's own draft — the normal state.
    ///   History{i}  walking past USER messages (↑/↓). `i` indexes the
    ///               reverse-chronological list of prior user messages.
    ///   QueuePeek{i} editing pending queued item `i` in place (Alt+↑/↓).
    ///
    /// This was `int history_idx = -1` plus `int queue_peek_idx = -1`,
    /// with a comment on the second one stating the invariant in prose:
    /// "Mutually exclusive with history_idx — you're either walking past
    /// USER messages or editing a pending QUEUED one, never both." Two
    /// independent ints cannot express that: `history_idx >= 0 &&
    /// queue_peek_idx >= 0` was a perfectly constructible state that every
    /// reader had to avoid producing, and -1 had to be remembered as "none"
    /// at each of the ~20 sites that touched them. A variant states the
    /// exclusion in the type, so the illegal combination has no spelling.
    ///
    /// The first ↑ (or Alt+↑) snapshots the live text/attachments into
    /// draft_save so the round-trip is non-destructive. Any text-mutating
    /// op (CharInput / Backspace / Paste / Kill / chip insertion) drops
    /// back to Live and discards the snapshot — at that point the user is
    /// editing what they pulled up, treating it as their new draft.
    struct Live {};
    struct History   { int index = 0; };   // into prior user messages
    struct QueuePeek { int index = 0; };   // into `queued`
    using Browsing = std::variant<Live, History, QueuePeek>;
    Browsing                   browsing{Live{}};

    std::optional<std::string> draft_save;
    /// Companion to `draft_save`: the live draft's attachments[]
    /// captured at the same moment so a queue-peek round-trip
    /// (Alt+↑ … Alt+↓ past the tail) restores chips too, not just
    /// the text. Empty if there were no live attachments at the
    /// time of the snapshot, OR if the snapshot is for a history
    /// walk (history items never carry attachments — they're
    /// rendered turns whose chips were collapsed at submit time on
    /// previous schemas). Cleared together with `draft_save`.
    std::vector<Attachment>    draft_save_attachments;

    /// ── Loop mode (^B) ───────────────────────────────────────────
    /// When armed, the message that armed it is RE-SENT automatically
    /// every time the agent finishes a turn, until the user toggles it
    /// off (^B again, or Esc). This is the "keep hammering on this
    /// prompt" workflow — iterate on a refactor, re-run a failing test,
    /// poll until something converges — without retyping or holding
    /// Enter.
    ///
    /// `loop_text` / `loop_attachments` snapshot the payload at ARM time
    /// rather than re-reading the live composer, so the user can keep
    /// typing a follow-up while the loop runs its own fixed prompt: the
    /// thing that repeats is the thing you armed, which is the only
    /// reading that stays true once the composer has moved on.
    ///
    /// `loop_iterations` counts completed auto-sends (the first, manual
    /// send is 0) purely so the UI can show ⟳ ×N — an unbounded loop
    /// with no visible progress is indistinguishable from a hang.
    ///
    /// LOOP IS A REPEAT-ON-SUCCESS CONSTRUCT WITH ADAPTIVE BACKOFF.
    /// It re-sends after a turn that ended normally — immediately, since
    /// a completed turn already took real wall-clock time. When a turn
    /// FAILS the loop does not die (the user asked it to keep going) but
    /// it must not re-fire instantly either: a 429 answered by an instant
    /// re-send is how a rate limit deepens into a ban. So a failure arms
    /// `loop_wait_until_ms` and the next send waits.
    ///
    /// The delay is chosen by the provider's own signal where one exists:
    /// a `Retry-After` header is obeyed verbatim (the server told us when
    /// to come back — guessing anything else is strictly worse). Absent
    /// that, `loop_failures` escalates a per-class schedule and resets to
    /// 0 on the next success, so a transient blip costs seconds while a
    /// sustained outage decays to minutes instead of spinning.
    ///
    /// Only Cancelled stops the loop outright: Esc means stop.
    bool                    loop_armed = false;
    std::string             loop_text;
    std::vector<Attachment> loop_attachments;
    int                     loop_iterations = 0;
    /// Consecutive failed iterations; drives the backoff schedule and
    /// resets to 0 on any success.
    int                     loop_failures = 0;
    /// Wall-clock ms before which the loop must not re-send. 0 = ready now.
    std::int64_t            loop_wait_until_ms = 0;

    [[nodiscard]] bool looping() const noexcept {
        return loop_armed && !loop_text.empty();
    }

    /// Stop the loop and forget what it was repeating. Called on cancel
    /// and by the ^B toggle. Idempotent.
    void disarm_loop() noexcept {
        loop_armed = false;
        loop_text.clear();
        loop_attachments.clear();
        loop_iterations = 0;
        loop_failures = 0;
        loop_wait_until_ms = 0;
    }

    /// A turn ended normally: clear the failure streak so the next hiccup
    /// starts from the bottom of the schedule again.
    void loop_note_success() noexcept {
        loop_failures = 0;
        loop_wait_until_ms = 0;
    }

    /// May the loop send right now? False while a backoff is pending.
    [[nodiscard]] bool loop_ready(std::int64_t now_ms) const noexcept {
        return looping() && now_ms >= loop_wait_until_ms;
    }

    /// Seconds still to wait, for the UI countdown (0 when ready).
    [[nodiscard]] int loop_wait_secs(std::int64_t now_ms) const noexcept {
        if (loop_wait_until_ms <= now_ms) return 0;
        return static_cast<int>((loop_wait_until_ms - now_ms + 999) / 1000);
    }

    /// Record a failed iteration and arm the next delay.
    ///
    /// `retry_after` (the provider's own Retry-After) wins outright when
    /// present: the server stated when to return, and any guess we make
    /// is either rude or needlessly slow. Otherwise escalate by class —
    /// rate limits get a much longer floor than a transient blip, because
    /// retrying a 429 early is what turns it into a longer one. Capped so
    /// a long outage settles at a slow poll instead of growing unbounded.
    void loop_backoff(int error_class_rank, int retry_after_secs,
                      std::int64_t now_ms) noexcept {
        ++loop_failures;
        int secs;
        if (retry_after_secs > 0) {
            secs = retry_after_secs;
        } else {
            // rank: 0 = transient/other, 1 = rate-limit/auth (slower).
            const int base = error_class_rank >= 1 ? 30 : 5;
            const int cap  = error_class_rank >= 1 ? 600 : 120;
            // Double per consecutive failure: 5,10,20,40… / 30,60,120…
            secs = base;
            for (int i = 1; i < loop_failures && secs < cap; ++i) secs *= 2;
            if (secs > cap) secs = cap;
        }
        loop_wait_until_ms = now_ms + static_cast<std::int64_t>(secs) * 1000;
    }

    // ── Browsing queries ─────────────────────────────────────────
    // Small readers so call sites ask a question instead of unpacking a
    // variant inline. Each returns the index only when that alternative is
    // actually active, so "which mode" and "which item" cannot be answered
    // separately — the pairing that -1 sentinels kept splitting apart.
    [[nodiscard]] bool is_live() const noexcept {
        return std::holds_alternative<Live>(browsing);
    }
    [[nodiscard]] std::optional<int> history_index() const noexcept {
        if (auto* h = std::get_if<History>(&browsing)) return h->index;
        return std::nullopt;
    }
    [[nodiscard]] std::optional<int> queue_peek_index() const noexcept {
        if (auto* q = std::get_if<QueuePeek>(&browsing)) return q->index;
        return std::nullopt;
    }

    /// Wall-clock ms (maya anim clock) of the user's last composer
    /// interaction (any keystroke / edit / cursor move). Drives the
    /// idle blink-stop: the painted block cursor stops blinking 15 s
    /// after this, so the composer cell goes static and a GPU terminal
    /// with an aggressive repaint_delay stops compositing at idle. 0
    /// until the first interaction (blink runs normally until then).
    std::int64_t last_edit_ms = 0;
};

// Todo picker carries its own item list — separate concern from the
// open/closed state, which now lives in `open` as a typed variant.
struct TodoState {
    ui::pick::Modal       open;     // Closed | OpenModal
    std::vector<TodoItem> items;
};

// ============================================================================
// Model — the composed application state, split into three concerns:
//   d   — Domain: what the conversation is (persisted, sent to provider).
//   s   — Session: the in-flight request's state machine + cancel handle.
//   ui  — UI: picker/modal/view-virtualization state, pure ephemeral.
//
// The split lets call sites communicate their scope: a function that only
// touches `m.ui.*` can't accidentally mutate the conversation; a reducer
// fragment that reads `m.d.*` doesn't need to thread picker state through.
// ============================================================================

struct Model {
    struct Domain {
        Thread              current;
        std::vector<Thread> threads;
        Profile             profile = Profile::Write;

        std::vector<ModelInfo> available_models;
        ModelId                model_id{std::string{"claude-opus-4-5"}};

        // Fused cross-provider model picker (docs/design/unified-model-picker.md).
        // `provider_catalogs` is the MERGED, multi-provider catalog view built
        // lazily when the fused picker opens (one entry per authed provider);
        // it sits BESIDE available_models (which stays the active provider's
        // catalog the wire path + context math read). `recent_models` is the
        // MRU (provider,model) the user toggles between, newest first, capped;
        // it drives the RECENT section and ^Tab quick-swap and persists to
        // Settings.recent_models.
        std::vector<ProviderCatalog> provider_catalogs;
        std::vector<ModelRef>        recent_models;
        // Cached fused-picker row list, rebuilt by the reducer ONLY when its
        // inputs change (open / filter / catalog-loaded / favorite) so the
        // view and cursor math read a ready vector every frame instead of
        // re-enumerating providers + re-reading settings.json + re-scoring
        // every model per keystroke. Empty while the picker is closed.
        std::vector<FusedRow>        fused_rows;
        // Sign-in offers (un-authed providers) computed ONCE when the fused
        // picker opens, so per-keystroke row rebuilds never re-derive auth
        // from disk. Paired with provider_catalogs as the picker's sources.
        std::vector<SigninOffer>     fused_offers;
        // Reasoning effort tier, selected live in the model picker (←/→).
        // None = the default no-thinking wire; any other level makes the
        // Claude provider send adaptive thinking + output_config.effort.
        // Persisted in Settings; gated per-model at request-build time.
        Effort                 effort = Effort::None;

        // Smart Mode: role-based execution routing (docs/design/smart-mode.md).
        // `enabled` off by default — the whole feature is opt-in and a no-op
        // when off (internal utility calls keep their existing cheapest-model
        // default). Persisted in Settings.
        smart::RoleConfig      smart;

        std::vector<FileChange>          pending_changes;
        // Whether the persistent "N changes" review strip renders after edits.
        // Loaded from Settings at startup; toggled live via the palette. OFF by
        // default — edits still queue in pending_changes (Ctrl+R opens the
        // pane), just without the always-on banner.
        bool                             show_changes_strip = false;
        // Show the model's reasoning/thinking block in the transcript AND ask
        // Anthropic for visible thinking (interleaved-thinking beta). Loaded
        // from Settings at startup; toggled live via the palette (Ctrl+K →
        // "Reasoning"). Off by default. Provider-agnostic.
        bool                             show_reasoning = false;
        std::optional<PendingPermission> pending_permission;

        // Session-scoped "always allow" grants, keyed by tool name
        // (e.g. "shell", "write"). Set by PermissionApproveAlways;
        // consulted in kick_pending_tools BEFORE prompting. NOT
        // persisted — a grant lives for the lifetime of the process
        // run, mirroring Zed's per-session allow-list. Cleared on
        // profile change so tightening the profile re-arms prompts.
        std::set<std::string>            session_grants;

        // Is ANY provider catalog still being fetched?
        //
        // THE predicate for "the fused picker's list is still filling in".
        // Distinct from Session::models_loading, which covers only the
        // ACTIVE provider's fetch (provider switch / startup) — the picker
        // fans out to every authed provider and tracks each one's state
        // here. Using the wrong one is how the picker's "loading …" spinner
        // ended up gated on a flag the picker never sets: it advanced during
        // provider switches and stayed frozen in the one place it exists for.
        [[nodiscard]] bool any_catalog_loading() const noexcept {
            for (const auto& c : provider_catalogs)
                if (c.state == ProviderCatalog::State::Loading) return true;
            return false;
        }
    };

    struct UI {
        ComposerState       composer;
        // ── THE exclusive panel slot ─────────────────────────────
        // Every modal overlay (pickers, palettes, viewers, diff review)
        // lives in this ONE variant: opening is assignment, which
        // structurally closes whatever was open — "two overlays open" is
        // unrepresentable, and the ~17 rival-closing writes the old 16
        // separate fields required are gone. See overlay_state.hpp for
        // the alternatives and the deliberate non-members (login,
        // permission, todo — they coexist with the slot by design;
        // panel::top() composes all four into the priority answer).
        ui::panel::State  panel;
        // Escape-based clipboard read bookkeeping: bumped when a query is
        // emitted; a successful paste stamps done = seq. The delayed
        // ClipboardQueryTimeout{seq} only fires its diagnosis when seq still
        // matches and done hasn't caught up — so a reply always cancels the
        // timeout, and rapid re-triggers can't fire stale toasts.
        std::uint64_t       clipboard_query_seq  = 0;
        std::uint64_t       clipboard_query_done = 0;
        // maya::clipboard_rx_bytes() sampled when the query was armed. The
        // timeout compares against it to tell a silent terminal from one
        // whose (large, image) reply is still streaming in.
        std::uint64_t       clipboard_rx_mark    = 0;
        // True when the in-flight query was raised by an IMAGE-paste intent
        // (Ctrl+V / Alt+V on a clipboard we could not read locally), as
        // opposed to an ordinary text paste. Set alongside the seq bump.
        //
        // Why it matters: over SSH+tmux agentty asks for BOTH dialects — OSC
        // 5522 (kitty only; carries image bytes) and OSC 52 (text only,
        // widely supported). A non-kitty terminal ignores the first and
        // answers the second, so the user who wanted to paste a screenshot
        // silently receives TEXT. The reply cancels the no-reply timeout, so
        // without this flag there is no moment at which anything can explain
        // what happened — the failure is invisible.
        bool                clipboard_wanted_image = false;
        // (Smart-Mode slot-assign state used to be parked HERE as three
        // fields — slot, advanced, from — because the hand-off destroyed the
        // SmartMode overlay. It now rides ON the assign-mode Models panel
        // (pn::Models::assign_slot) with the whole SmartMode pane in its
        // `from` snapshot: abandoning the picker abandons the mode
        // structurally, and ascend() restores the pane. Nothing to park.)

        // Effort tier changed via ←/→ in the model picker but not yet
        // flushed to disk. Persisting per keystroke is a synchronous
        // load+fsync+rename on the UI thread; instead the CycleEffort arm
        // sets this and CloseModels/Select flush once.
        bool                effort_dirty = false;
        // Plugins/MCP connection snapshot — OWNED BY THE MODEL, not read from
        // the global pool at render time. This is the architectural fix for
        // the recurring "stuck on connecting… / laggy panel" bugs: the view
        // and visual_hash are contractually pure functions of the Model, so
        // any UI truth living OUTSIDE the Model (as plugin_model() used to,
        // reaching into a process-global pool) is invisible to the render
        // gate and undriveable by the update loop. The connection itself
        // still lives in the mcp:: ConnectionPool (it owns sockets/child
        // procs), but its UI-facing VALUE snapshot is mirrored here via the
        // PluginsUpdated message (Cmd→Msg, exactly like ThreadsLoaded). The
        // view renders THIS; nothing in the view path calls plugin_model().
        mcp::PluginModel    plugins;
        bool                plugins_loading = false;  // a connect/reload Cmd is in flight
        // Tail-follow toggle for the tool viewer's LIVE row (row 0). True =
        // the body auto-scrolls to the newest streamed output (tail -f);
        // scrolling up disengages it, End / scrolling back to the bottom
        // re-engages. Only consulted while viewing a live entry.
        bool                tool_viewer_tail = true;
        TodoState           todo;
        ui::login::State    login;            // Closed | Picking | OAuthCode | OAuthExchanging | ApiKeyInput | Failed
        int                 thread_scroll = 0;
        // Terminal window focus (?1004). Starts true — the terminal
        // that launched agentty has focus. Gates the hardware caret:
        // unfocused ⇒ park + hide (no blinking bar in inactive panes).
        bool                terminal_focused = true;

        // ── Sealed scrollback prefix (maya ScrollbackLedger) ───
        //
        // Append-only ledger of fully-built Element blocks that
        // represent the settled portion of the transcript. The view
        // hands maya's Conversation widget a borrowed pointer via
        // `Config::ledger`, which renders it through `ledger_ref` —
        // zero-copy across frames AND maya's paint pass records every
        // block's real laid-out height back into the ledger each
        // frame.
        //
        // THE ACCOUNTING INVERSION (Witness Chain — Trim Accounting):
        // the ledger replaced four parallel host-maintained vectors
        // (frozen / frozen_rows / frozen_is_separator + frozen_row_total
        // + frozen_cols) whose row counts were HOST-measured through a
        // reconstructed width. Every historical trim-corruption bug was
        // drift between that parallel measurement and what maya painted
        // (the phone-over-SSH duplication ghost, the post-resize
        // over-commit, the one-row estimate drift). Now maya measures:
        // trim commit counts are minted exclusively by
        // ledger.harvest() from paint-recorded heights, as a typed
        // ScrollbackDebt token the host cannot fabricate or adjust —
        // drift is unrepresentable, not merely tested-against.
        //
        // Block granularity (one ledger block per push):
        //   • a gap (blank+rule+blank) before each fresh-speaker turn,
        //   • a built Turn Element for a settled message (or run),
        //   • a compaction divider for a CompactionRecord boundary.
        //
        // The producer is freeze_range/freeze_through (frozen.cpp),
        // called from the settle path. Cleared (and frozen_through
        // reset) on thread switch / NewThread / OpenThread; rebuilt by
        // rehydrate_frozen() when a saved thread is loaded.
        maya::ScrollbackLedger frozen;

        // Exclusive upper bound into m.d.current.messages. Every
        // message with index < frozen_through has already been built
        // into `frozen` and need not be rendered live. The suffix
        // [frozen_through .. end) is the live tail — rebuilt every
        // frame (with the per-Turn shared_ptr Element cache keeping
        // settled-within-tail messages cheap).
        std::size_t         frozen_through = 0;

        // Running turn number for the next freshly-frozen Assistant
        // turn. The live tail reads (frozen_turn + assistant_messages_in_tail)
        // for its display numbers.
        int                 frozen_turn = 0;

        // Deferred settle-freeze. The post-stream settle (finish() on the
        // StreamingMarkdown) flips the md widget's build shape + prefix
        // generation, so the post-finish element tree's inner
        // ComponentElement hash differs from every live (pre-finish)
        // frame. If freeze_through ran in the SAME tick as finish(), the
        // frozen tree (post-finish hash) would be diffed against
        // prev_cells (last live, pre-finish hash) on the next paint = a
        // maya component-cache miss = the whole turn re-emits from the
        // top (the post-settle redraw). Instead finalize_turn settles
        // here and sets this flag WITHOUT freezing; one view() paints the
        // finished (still-unfrozen, live-tail) tree so prev_cells now
        // holds the post-finish hash; the next idle Tick sees the flag,
        // freezes the byte-and-hash-identical tree (cache HIT, no
        // re-emit), and clears the flag.
        bool                pending_settle_freeze = false;

        // Post-freeze settling window. When the deferred settle-freeze
        // fires, the live tail collapses into the frozen prefix in ONE
        // reducer step. If that tail had overflowed the viewport (the
        // common case for a long reply), maya's renderer needs SEVERAL
        // frames to fully reconcile the shrink: detect the shrink-while-
        // overflowed prefix, commit the off-viewport rows, demote to
        // Stale, then soft-repaint the corrected viewport on the
        // FOLLOWING render. At fps=0 the tick subscription drops the
        // instant pending_settle_freeze clears, so without an explicit
        // settling window maya might get only ONE post-collapse frame —
        // not enough for the detect→commit→demote→repaint chain — and a
        // stranded duplicate turn lingers until the next user keystroke.
        // agent_session never has this problem: its always-on 30fps clock
        // keeps rendering frames after MessageStop, so the reconciliation
        // chain always completes. We emulate that by keeping the tick
        // alive for a few frames after the freeze. Set to kSettleCooldown
        // when the freeze fires; decremented each Tick; the tick
        // subscription stays armed while > 0.
        int                 settle_cooldown_ticks = 0;

        // One-shot: re-run trim_frozen_if_oversized on the first Tick
        // after a thread rehydrate. The rehydrate budget walk works on
        // ESTIMATED heights; the first paint stamps every sealed block's
        // REAL laid-out height into the ledger (record_paint), and on
        // tool-heavy threads the real total can exceed the estimate
        // several-fold (per-panel chrome the estimate's flat caps miss).
        // The deferred trim closes the loop against ground truth — same
        // provable drop_front + harvest + commit_scrollback path the
        // settle-freeze trim uses. Set by ThreadLoaded (which also arms
        // the Tick via settle_cooldown_ticks); consumed by the Tick arm.
        bool                pending_rehydrate_trim = false;

        // One-shot hint to maya's run loop: "the next view() result
        // contains a heavy frozen scrollback that hasn't been painted
        // yet on this thread; please pre-warm the component cache
        // before the wire-bound render." Set after rehydrate_frozen()
        // populates m.ui.frozen on thread swap; cleared by the reducer
        // step that produced the post-warmup model (so warmup fires
        // exactly once per swap).
        //
        // Maya's run<P> loop detects this via the optional Program
        // hook `static bool needs_warmup(const Model&)` — see app.hpp
        // detail::HasNeedsWarmup. When true, an off-wire warmup_render
        // populates the hash-keyed component cache so the user-visible
        // render hits the cell-blit fast path: converts 80–660 ms cold
        // paint to <1 ms warm paint on tool-heavy thread resume.
        bool                needs_warmup_render = false;

        // Cross-frame widget state cache. The only consumers now are:
        //   • StreamingMarkdown — keeps a per-Message widget instance
        //     alive across frames so its block boundary cache survives
        //     between live renders.
        //   • Agent-timeline panel freeze — snapshots the AgentTimeline
        //     Element once every tool call is terminal, then serves
        //     that frozen Element until the message gets pushed into
        //     m.ui.frozen.
        // No Turn-level Element cache anymore: settled turns live as
        // raw Element values inside m.ui.frozen, the live tail
        // rebuilds each frame (bounded by the active turn).
        mutable ui::ViewCache view_cache;

        // ── Scroll state for modal pickers ─────────────────────────────
        //
        // Storage for the scroll state of each modal picker. The picker
        // widget (maya::Picker) reads & mutates these via a borrowed
        // pointer in Picker::Config; the host owns the storage so the
        // adapter rule holds (maya owns chrome + behavior; agentty owns
        // model state).
        //
        // `mutable` is required because the view function takes
        // `const Model&` but maya's paint-time writeback mutates
        // max_y / bar_v_bounds / viewport_bounds. Same logical-const
        // pattern as view_cache above: filling a render-side cache /
        // bounds slot doesn't change observable Model behavior.
        //
        // Persisted across open→close→open by default. The reducer can
        // reset `.y = 0` on semantic transitions if desired (e.g. when
        // the filter query changes the match set).
        //
        // auto_dispatch = false: these pickers are selection-driven. The
        // reducer owns the cursor (ModelsMove / ThreadListMove /
        // PaletteMove / …) and the Picker widget auto-scrolls the
        // viewport to keep the selected row visible every build. Leaving
        // auto_dispatch on (the default) would ALSO feed every ↑/↓/PageUp
        // arrow straight into ScrollState::handle, bumping scroll.y in
        // parallel — which then fights the widget's selection-follow
        // clamp. The visible symptom is arrow keys appearing to do
        // nothing until the offset saturates against max_y ("press 4-5
        // times before it registers once"). Scroll position here is a
        // pure function of the selected index, so the raw-key dispatch
        // is interference, not input.
        //
        // NB: maya's ScrollState gained a user-declared destructor (the
        // UAF fix that unregisters it from the live-state registry), so
        // it is no longer an aggregate and `{.auto_dispatch = false}`
        // designated init no longer compiles. routed_scroll() builds one
        // with auto_dispatch cleared without touching aggregate-ness.
        static maya::ScrollState routed_scroll() noexcept {
            maya::ScrollState s;
            s.auto_dispatch = false;
            return s;
        }
        mutable maya::ScrollState fused_picker_scroll     = routed_scroll();
        mutable maya::ScrollState provider_picker_scroll  = routed_scroll();
        mutable maya::ScrollState thread_list_scroll      = routed_scroll();
        mutable maya::ScrollState command_palette_scroll  = routed_scroll();
        mutable maya::ScrollState mention_palette_scroll  = routed_scroll();
        mutable maya::ScrollState symbol_palette_scroll   = routed_scroll();
        mutable maya::ScrollState code_blocks_scroll      = routed_scroll();
        mutable maya::ScrollState checkpoints_scroll      = routed_scroll();
        mutable maya::ScrollState rag_settings_scroll     = routed_scroll();
        mutable maya::ScrollState smart_mode_scroll       = routed_scroll();
        mutable maya::ScrollState fork_scroll              = routed_scroll();
        mutable maya::ScrollState todo_scroll             = routed_scroll();
        mutable maya::ScrollState tool_viewer_scroll      = routed_scroll();
        mutable maya::ScrollState account_list_scroll     = routed_scroll();
        // The Ctrl+K settings list (Plugins/Commands/Agents/Hooks). The
        // Plugins pane can exceed any viewport (125 advertised tools ⇒ 135+
        // rows), so it scrolls like every other picker.
        mutable maya::ScrollState settings_list_scroll    = routed_scroll();
    };

    Domain      d;
    StreamState s;
    UI          ui;

    // Is a LOADING SPINNER currently on screen?
    //
    // THE gate for the picker's catalog-fetch animation — and the reason it
    // is a Model-level predicate rather than a Domain one: a spinner is only
    // worth a 33 ms tick when the surface that draws it is VISIBLE. Keying
    // on catalog state alone kept the whole event loop waking at frame rate
    // after the picker closed, which is both wasted work and, worse, latency:
    // an idle loop that is already inside a timed poll answers the next
    // keystroke a frame late. (Measured: arrow-key p90 12.7 ms / max 55 ms
    // during navigation, against a p50 of 0.1 ms.)
    //
    // Pairs with Session::active() at every call site — see the THREE gates
    // that must agree: subscribe.cpp (is a Tick delivered), update/meta.cpp
    // (does the spinner advance) and app/program.hpp (does the frame change
    // the visual hash).
    [[nodiscard]] bool loading_spinner_visible() const noexcept {
        return ui.panel.is<ui::panel::Models>()
            && d.any_catalog_loading();
    }
};

} // namespace agentty
