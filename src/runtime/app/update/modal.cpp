// Composer-submission and settings-persistence helpers for the update
// reducer. submit_message is the entry point for ComposerEnter /
// ComposerSubmit and is also called from finalize_turn when flushing
// the composer's queued-message buffer, which is why it lives in a
// shared internal header rather than an anonymous namespace.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"

#include <cctype>
#include <algorithm>
#include <chrono>
#include <optional>
#include <utility>

#include "agentty/runtime/app/cmd_factory.hpp"
#include "agentty/auth/accounts.hpp"   // entitlement scope: active account label
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/composer_attachment.hpp"
#include "agentty/tool/commands.hpp"
#include "agentty/runtime/view/helpers.hpp"
#include "agentty/provider/chatgpt/provider.hpp"
#include "agentty/provider/copilot/provider.hpp"
#include "agentty/provider/registry.hpp"
#include "agentty/provider/selection.hpp"
#include "agentty/store/store.hpp"
#include "agentty/tool/mcp_tools_backends.hpp"
#include "agentty/tool/skills.hpp"
#include "agentty/tool/subagent.hpp"
#include "agentty/workspace/checkpoint.hpp"

namespace pn = agentty::ui::panel;

namespace agentty::app::detail {

Step submit_message(Model m) {
    using maya::Cmd;
    // Composer is non-empty if it has typed text OR an attachment chip.
    // Even an "empty-looking" buffer with chips should submit — those
    // chips ARE the message (a single dropped @file or paste, with no
    // surrounding prose). The expand pass below pulls each chip's body
    // into the wire text.
    if (m.ui.composer.text.empty() && m.ui.composer.attachments.empty())
        return done(std::move(m));

    // No model resolved yet — don't start a turn. Sending `"model": ""` to a
    // local OpenAI-compatible server (llama.cpp / vLLM) is rejected, and the
    // rejection feeds the turn's retry machine, which re-fires forever: the
    // "dead loop when prompted" a freshly-selected local preset shows before
    // its /models fetch has landed and auto-selected a model. Refuse cleanly
    // and tell the user, keeping their text in the composer so nothing is
    // lost. (Hosted providers always have a model id, so this only bites the
    // local-preset-before-models-loaded window it's meant to cover.)
    if (m.d.model_id.value.empty()) {
        m.s.status = m.s.models_loading
            ? "loading models\xe2\x80\xa6 pick one (^/) before sending"
            : "no model selected \xe2\x80\x94 open the model picker (^/) first";
        m.s.status_until = std::chrono::steady_clock::now()
                         + std::chrono::seconds{4};
        return done(std::move(m));
    }
    // STALE-model guard for LOCAL providers. settings.json's per-provider
    // model map recalls the last-picked id PER SPEC STRING — so switching
    // between spellings of the same server (localhost:8080 vs 127.0.0.1:8080/v1)
    // can resurrect an id the running server doesn't serve. Local catalogs
    // are AUTHORITATIVE (llama-server serves exactly what /v1/models lists;
    // Ollama 404s an unpulled tag), so a recalled id absent from a non-empty
    // loaded list will fail every request — refuse cleanly instead. Hosted
    // TLS providers are exempt: their catalogs are advisory (aliases, dated
    // variants and gateway rewrites legitimately stream fine unlisted).
    {
        const auto& sel = provider::active();
        const bool local_openai = sel.kind == provider::Kind::OpenAI
                               && !sel.openai_endpoint.use_tls;
        if (local_openai && !m.d.available_models.empty()) {
            bool listed = false;
            for (const auto& mi : m.d.available_models)
                if (mi.id == m.d.model_id) { listed = true; break; }
            if (!listed) {
                m.s.status = "model '" + m.d.model_id.value
                           + "' isn't served by this host \xe2\x80\x94 pick one (^/)";
                m.s.status_until = std::chrono::steady_clock::now()
                                 + std::chrono::seconds{5};
                return done(std::move(m));
            }
        }
    }

    // The pending-changes review strip covers ONE window: between the agent
    // finishing its edits and the user's next message. Sending a new turn ends
    // that window — the edits are already on disk, so submitting means "I've
    // seen them, carry on" (implicit accept). Clear the queue so the strip
    // doesn't linger across turns. Explicit review (Ctrl+R → reject) still runs
    // BEFORE you'd send a new message; this only fires when you move on without
    // rejecting. ANNOUNCE it — a silent implicit accept of N files is exactly
    // the kind of decision a user should get to see happen (and learn the
    // review affordance from), even if they never act on it.
    if (!m.d.pending_changes.empty()) {
        const int files = static_cast<int>(m.d.pending_changes.size());
        m.s.status = "kept " + std::to_string(files)
                   + (files == 1 ? " edited file" : " edited files")
                   + " from last turn";
        m.s.status_until = std::chrono::steady_clock::now()
                         + std::chrono::seconds{3};
        m.d.pending_changes.clear();
    }

    // ── Slash-command expansion ──────────────────────────────────
    // `/name args` → the command's template body with $ARGUMENTS/$1..$9
    // substituted, BEFORE any queue/checkpoint/wire path sees the text —
    // every downstream consumer (queued resend, transcript, provider
    // request) uniformly gets the expanded prompt. A `/` that matches no
    // discovered command falls through unchanged (typing /etc/hosts as a
    // message must keep working). Attachment chips survive: only .text is
    // rewritten, placeholders inside it are left intact (a command body
    // does not carry chips, so expansion cannot orphan one).
    if (auto expanded = tools::commands::try_expand(m.ui.composer.text)) {
        m.ui.composer.text   = std::move(*expanded);
        m.ui.composer.cursor = static_cast<int>(m.ui.composer.text.size());
    }

    // Drain composer.text + composer.attachments into a single fully
    // expanded payload string, resetting composer fields. Used by the
    // queue-on-busy and queue-on-compact paths and by the actual
    // submit path below — all three need the same "linearise chips
    // now, attachments vector becomes empty" semantics so a Recall
    // (Up arrow) of a queued item never resurrects a placeholder
    // pointing at a dropped index.
    // Drain composer.text + composer.attachments into a chip-form
    // payload — the placeholders STAY in the text and the attachment
    // bodies travel separately. Used by:
    //   • the queue-on-busy / queue-on-compact paths (queued items
    //     keep their chips so recall + resend renders as a chip too,
    //     not a linearised blob);
    //   • the actual submit path below (the new Message gets the
    //     same chip-form text and the attachments are moved onto
    //     `Message.attachments`).
    //
    // The transport calls `attachment::expand(...)` at request-build
    // time to splice the bodies back in so the model still sees
    // literal pasted bytes / file contents — the only thing that
    // changes is what the user sees in the rendered transcript.
    auto drain_composer = [](Model& mm) {
        ComposerState::QueuedMessage out;
        out.text        = std::exchange(mm.ui.composer.text, {});
        out.attachments = std::exchange(mm.ui.composer.attachments, {});
        mm.ui.composer.text.clear();
        mm.ui.composer.attachments.clear();
        mm.ui.composer.cursor = 0;
        // Submit boundary clears the per-draft transient state. Undo
        // / redo and the history-walk index belong to the draft the
        // user just sent; carrying them into the next draft would
        // produce surprising "Ctrl+Z restores half of last turn".
        mm.ui.composer.undo_stack.clear();
        mm.ui.composer.redo_stack.clear();
        mm.ui.composer.browsing = ComposerState::Live{};
        mm.ui.composer.draft_save.reset();
        return out;
    };

    // Peeked-item submission: the user pressed Alt+↑ to load a queued
    // item, possibly edited it, and submitted. We remove the ORIGINAL
    // slot from the queue now; the drain-into-queued path below (when
    // the agent is still busy) will push the edited bytes back onto
    // the tail. If the agent is idle (rare — user would have to peek
    // while the agent was busy, then have it finish before they hit
    // Enter), the edited bytes go straight to the wire and the queue
    // just shrinks by one.
    if (const auto peek = m.ui.composer.queue_peek_index();
        peek && *peek < static_cast<int>(m.ui.composer.queued.size())) {
        m.ui.composer.queued.erase(
            m.ui.composer.queued.begin() + *peek);
        // draft_save (if any) is the live draft the user was typing
        // before they pressed Alt+↑. They've explicitly committed the
        // peeked item by submitting it, so the saved draft is now
        // homeless — drop it. (drain_composer clears the field too,
        // but only after we've decided to drain; doing it here keeps
        // the bail-out paths above tidy.)
        m.ui.composer.draft_save.reset();
        m.ui.composer.draft_save_attachments.clear();
        m.ui.composer.browsing = ComposerState::Live{};
    }

    // Belt-and-suspenders: queue if any non-Idle phase is in flight.
    // The bare check (Streaming || ExecutingTool) was correct in
    // practice — the keymap routes Esc/y/n/a to the permission modal
    // when `pending_permission.has_value()`, so an AwaitingPermission
    // phase can't reach a ComposerEnter dispatch — but `active()` /
    // `!is_idle()` makes the guarantee structural instead of relying
    // on two separate gating layers staying in sync. Future addition
    // of new phases (or a refactor that lets the composer stay live
    // during AwaitingPermission) won't silently regress to "submit
    // overwrites the active ctx".
    //
    // Also queue while a background OAuth refresh is in flight. Deps
    // still holds the pre-refresh (expired) auth header until the
    // TokenRefreshed handler swaps it; firing a stream now would 401.
    // The handler drains this queue once new creds are live.
    if (m.s.active() || m.s.oauth_refresh_in_flight) {
        m.ui.composer.queued.push_back(drain_composer(m));
        return done(std::move(m));
    }

    // No auto-compaction on submit. Earlier versions queued the
    // user's message and fired a synchronous compaction round before
    // releasing it — user hits Enter, sees nothing for 30-60 s,
    // then their message finally goes out. That was an unacceptable
    // workflow break.
    //
    // The new shape: `launch_stream` soft-trims the wire payload to
    // fit ~95% of context_max on every normal turn, so submits NEVER
    // need to wait on compaction to be safe. The user's message goes
    // out immediately. A background auto-compact may still fire at
    // the next post-turn idle boundary (see `maybe_autocompact_after_turn`
    // in finalize_turn) — that's the right moment because the user
    // is reading the model's output, not typing, and the compaction
    // happens without blocking anything they're trying to do. The
    // /compact slash command also stays available for manual control.

    Message user;
    user.role = Role::User;
    // Drain composer → chip-form text + attachments. Image
    // attachments must reach the wire as Anthropic image content
    // blocks (NOT as the "[image: ...]" prose marker); we lift
    // their bytes onto user.images here and DROP them from
    // attachments so the on-Message attachments vector only
    // contains the kinds the wire expander handles textually
    // (Paste / FileRef / Symbol). The chip placeholder for the
    // image stays in user.text — the renderer treats a placeholder
    // pointing past attachments[] as an Image chip and consults
    // user.images[] for the caption.
    auto drained = drain_composer(m);
    // Image attachments: their bytes get lifted to `user.images` so
    // the transport can encode them as Anthropic image content
    // blocks. We KEEP the Attachment entry in `drained.attachments`
    // — just with `body` moved out — so placeholder indices in
    // `user.text` remain valid (a paste followed by an @file by an
    // image would have placeholders 0, 1, 2; renumbering after erase
    // would desynchronise the text with the vector). The wire
    // expander emits a textual marker for kind==Image; the renderer
    // surfaces the same chip caption it would for any other kind.
    for (auto& att : drained.attachments) {
        if (att.kind == Attachment::Kind::Image) {
            ImageContent img;
            img.media_type = att.media_type;     // copy: path/type stays on Attachment
            img.bytes      = std::move(att.body);
            user.images.push_back(std::move(img));
        }
    }
    user.text        = std::move(drained.text);
    user.attachments = std::move(drained.attachments);

    // ── User-explicit skill activation (spec: slash-command syntax) ──
    // `/skill-name [rest of prompt]` — the harness intercepts the token,
    // splices the skill's full activation payload into the message, and
    // marks it active (so a later model-driven `skill` call dedups).
    // The model receives the instructions without having to take an
    // activation action itself. Only the FIRST token is considered, and
    // only when it exactly matches a discovered skill — `/compact` and
    // friends fall through to their existing handlers untouched.
    if (!user.text.empty() && user.text.front() == '/') {
        auto sp  = user.text.find_first_of(" \t\n");
        auto tok = user.text.substr(1, sp == std::string::npos
                                           ? std::string::npos : sp - 1);
        if (const auto* sk = tools::skills::find(tok)) {
            std::string rest = sp == std::string::npos
                ? std::string{}
                : std::string{user.text.substr(sp + 1)};
            std::string expanded;
            if (tools::skills::note_activated(sk->name)) {
                expanded  = tools::skills::activation_payload(*sk);
                expanded += "\n\nFollow the skill instructions above";
                expanded += rest.empty() ? "." : " for this task: " + rest;
            } else {
                // Already active this session — don't re-inject the body.
                expanded = "Apply the already-loaded '" + sk->name
                         + "' skill" + (rest.empty() ? "." : ": " + rest);
            }
            user.text = std::move(expanded);
        }
    }

    if (m.d.current.title.empty()) {
        // Title generation should see human-readable text, not raw
        // chip placeholders. Build a plain-text view of the user's
        // message: each `\x01ATT:N\x01` becomes `[<chip-label>]`,
        // matching what the user sees in the rendered turn.
        std::string title_src;
        title_src.reserve(user.text.size());
        std::size_t i = 0;
        while (i < user.text.size()) {
            if (static_cast<unsigned char>(user.text[i]) == attachment::kSentinel) {
                auto len = attachment::placeholder_len_at(user.text, i);
                if (len > 0) {
                    auto idx = attachment::placeholder_index(user.text, i);
                    if (idx < user.attachments.size()) {
                        title_src.push_back('[');
                        title_src.append(attachment::chip_label(user.attachments[idx]));
                        title_src.push_back(']');
                    }
                    i += len;
                    continue;
                }
            }
            title_src.push_back(user.text[i++]);
        }
        m.d.current.title = deps().title_from(title_src);
    }

    // ── Git checkpoint (Zed-agent behavior) ─────────────────────────
    // Stamp a checkpoint id on the user message and snapshot the whole
    // worktree (tracked + untracked, .gitignore respected) as a pinned
    // parentless commit BEFORE the agent starts mutating files. The
    // snapshot itself runs on an isolated worker (git add -A can take a
    // moment on a big repo); only the cheap repo-ness probe + id stamp
    // happen here. The view renders a checkpoint divider above the turn
    // (cfg.checkpoint_above), and "Rewind to checkpoint" in the palette
    // restores the files + truncates the transcript back to this point.
    // Outside a git repo this is a no-op — no id, no divider, no worker.
    std::optional<std::string> checkpoint_to_create;
    if (workspace::in_git_repo()) {
        user.checkpoint_id  = CheckpointId{user.id.value};
        checkpoint_to_create = user.id.value;
    }

    // Optional proactive grounding. It is OFF by default because automatic
    // transcript injection spends context on the user's behalf. When enabled,
    // run exactly one isolated retrieval and defer this turn's launch until it
    // settles; do not race a synchronous hedge against a duplicate fallback.
    std::string proactive_probe;
    {
        // Effective proactive gate = per-thread override (set by the fork
        // picker) if present, else the global RAG mode. FirstTurnOnly injects
        // only when the thread has no prior assistant turn yet (this user
        // message is the first). Off never injects; On always. A shell
        // AGENTTY_RAG_PROACTIVE override still wins inside proactive_enabled().
        bool proactive_on = false;
        {
            const bool first_turn = std::none_of(
                m.d.current.messages.begin(), m.d.current.messages.end(),
                [](const Message& mm) {
                    return mm.role == Role::Assistant && !mm.text.empty();
                });
            // Absent = inherit the global mode; engaged = this thread's own.
            // A total switch over the enum, so adding a RagMode is a compile
            // error here rather than a silent fall into "inherit" — which is
            // what the old chain of int comparisons did with any value it
            // didn't recognise.
            if (const auto ov = m.d.current.rag_mode_override) {
                switch (*ov) {
                    case store::RagMode::Off:           proactive_on = false;      break;
                    case store::RagMode::On:            proactive_on = true;       break;
                    case store::RagMode::FirstTurnOnly: proactive_on = first_turn; break;
                }
            } else {
                proactive_on = tools::proactive_enabled();
                if (proactive_on && tools::proactive_first_turn_only())
                    proactive_on = first_turn;
            }
        }
        // Human-readable view of the query: strip chip placeholders so the
        // retriever probes real words, not sentinel bytes. Skip when the
        // turn is a slash-command / skill activation (already handled) or a
        // plain @file drop with no prose.
        std::string probe;
        probe.reserve(user.text.size());
        for (std::size_t i = 0; i < user.text.size();) {
            if (static_cast<unsigned char>(user.text[i]) == attachment::kSentinel) {
                auto len = attachment::placeholder_len_at(user.text, i);
                if (len > 0) { i += len; continue; }
            }
            probe.push_back(user.text[i++]);
        }
        // Knowledge-shaped gate: enough words to be a real question, and
        // not an imperative file-mutation command (those want grep/edit,
        // not doc RAG). Cheap heuristics — the confidence bar inside
        // proactive_retrieve is the real filter; this just avoids wasting a
        // BM25 pass on "hi" / "edit foo.cpp" / "run the tests".
        auto word_count = [](const std::string& s) {
            int n = 0; bool in = false;
            for (char c : s) {
                bool w = std::isalnum(static_cast<unsigned char>(c));
                if (w && !in) ++n;
                in = w;
            }
            return n;
        };
        auto looks_imperative = [](const std::string& s) {
            // Lowercased first word matches a mutation/command verb.
            std::string w;
            for (char c : s) {
                if (std::isalpha(static_cast<unsigned char>(c)))
                    w += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                else if (!w.empty()) break;
            }
            static const char* verbs[] = {
                "edit","write","fix","run","add","remove","delete","create",
                "make","build","commit","refactor","rename","move","install",
                "update","change","implement","test","format","rebase","merge"};
            for (const char* v : verbs) if (w == v) return true;
            return false;
        };
        const bool slash = !user.text.empty() && user.text.front() == '/';
        if (proactive_on && !slash && !probe.empty()
            && word_count(probe) >= 3 && !looks_imperative(probe)) {
            proactive_probe = std::move(probe);
        }
    }
    // instead of being re-built every frame for the whole run, and the
    // settle-time freeze has one fewer seam to hand off.
    // CORRECTION SIGNAL (session cascade): if this new user turn looks like a
    // correction of the previous one ("no", "that's wrong", "actually…",
    // "undo", "revert"), that is ground truth the prior route was too weak —
    // nudge the SESSION effort bias up so this session thinks harder. The
    // classifier is a pure, unit-tested function (smart::), so this hot reduce
    // path just asks it.
    //
    // This used to also write a per-workspace regret keyed by turn signature
    // (RoutingMemory). That store is gone: the same signal at a second,
    // persisted timescale was never measured against the fixed policy, and it
    // silently ratcheted cost across sessions. The in-memory cascade keeps the
    // useful half — it decays every turn, is clamped, and dies with the
    // process.
    if (m.d.smart.orchestration() && smart::is_routing_correction(user.text)) {
        const int clamp = m.d.smart.bias_clamp;
        if (m.s.smart_effort_bias < clamp) ++m.s.smart_effort_bias;
    }

    m.d.current.messages.push_back(std::move(user));

    // Force the prior turn's reveal to settle BEFORE the freeze snapshot.
    // Normally the deferred settle-freeze (meta.cpp) waits for the reveal
    // to drain on its own, but a user can submit while it's still mid-
    // glide (pending_settle_freeze true). Freezing a still-`live_` widget
    // would snapshot a tree whose hash diverges from the on-screen live
    // frame — the post-stream duplicate. settle_message_md runs the
    // (now harmless) finish() so the widget is in its settled shape; the
    // freeze below then captures exactly what the next live frame would
    // paint. By submit time msg.text is final and streaming_text empty,
    // so this is shape-neutral for an already-drained turn and a clean
    // collapse for a still-animating one (the user moved on; cutting the
    // last ~100 ms of typewriter is the right call when they hit Enter).
    for (std::size_t i = m.ui.frozen_through;
         i + 1 < m.d.current.messages.size(); ++i) {
        auto& mm = m.d.current.messages[i];
        if (mm.role != Role::Assistant || mm.text.empty()) continue;
        settle_message_md(m, mm);
    }
    freeze_through(m, m.d.current.messages.size());
    // A deferred settle-freeze may still be pending from the prior turn
    // (user submitted before the next idle Tick fired). The freeze above
    // just covered it, so drop the flag to avoid a redundant no-op freeze
    // on the next Tick.
    m.ui.pending_settle_freeze = false;

    // Smart Mode: surface the per-turn routing DECISION as a first-class
    // thread card (🧠), right before the assistant reply, so the user sees in
    // detail what orchestration did. Wire-inert; std::nullopt when off.
    if (auto card = cmd::build_smart_routing_card(m))
        m.d.current.messages.push_back(std::move(*card));

    Message placeholder;
    placeholder.role = Role::Assistant;
    m.d.current.messages.push_back(std::move(placeholder));

    m.d.current.updated_at = std::chrono::system_clock::now();

    // Idle → Streaming. The fresh phase::Active replaces the prior
    // turn's context wholesale (Idle had none): zero retry counters,
    // fresh started/last_event_at stamps, default RetryState. Mirrors
    // the StreamStarted handler's reset so the post-submit render is
    // layout-identical to the post-StreamStarted render that lands
    // milliseconds later — without this, leftover status toast from
    // the prior turn (retry countdown / "Stream complete" / error
    // banner) would change status_bar height by one row when
    // StreamStarted fires, producing a visible "new turn appears at
    // viewport bottom and then realigns" two-frame flicker.
    auto now = std::chrono::steady_clock::now();
    phase::Active ctx;
    ctx.started       = now;
    ctx.last_event_at = now;
    m.s.phase         = phase::Streaming{std::move(ctx)};
    m.s.status.clear();
    m.s.status_until  = {};

    auto trim = trim_frozen_if_oversized(m);

    // ── Same-turn grounding with live feedback ──────────────────────────
    // The synchronous hedge above missed (a large/slow corpus whose dense
    // query-embed round-trip can't clear the small budget). Rather than (a)
    // freeze the submit thread waiting for it or (b) inject a STALE block on
    // some later, possibly-unrelated turn, we DEFER this turn's stream launch
    // behind the retrieval: run the un-hedged funnel on an isolated worker
    // and hold the request until it lands, then inject the block SAME-TURN
    // and launch. The phase is already Streaming{ctx} (set above), so the
    // status-bar spinner + activity indicator are live the whole time — the
    // user sees "retrieving context…", never a hung UI. A couple hundred ms
    // (occasionally a second or two) reads as the model thinking, which the
    // user has told us is fine as long as there's feedback.
    //
    // When there's no probe (fast hedge hit, or a non-knowledge query) we
    // launch immediately as before — zero added latency on the common path.
    const bool defer_for_retrieval = !proactive_probe.empty();
    maya::Cmd<Msg> launch;
    if (defer_for_retrieval) {
        m.s.status       = "retrieving context\xE2\x80\xA6";   // …
        m.s.status_until = {};   // sticky until the block lands
        // The launch is issued by the ProactiveContextReady handler once the
        // grounding is in the transcript. Here we only kick the retrieval.
        launch = Cmd<Msg>::task_isolated(
            [probe = std::move(proactive_probe)]
            (std::function<void(Msg)> dispatch) {
                auto hit = tools::proactive_retrieve_blocking(probe, /*k=*/3);
                dispatch(Msg{ProactiveContextReady{
                    hit ? std::move(hit->block) : std::string{},
                    hit ? hit->confidence : -1.0}});
            });
    } else {
        launch = cmd::launch_stream(m);
    }
    // No commit_scrollback_overflow here. Submit is not a wholesale
    // model swap — it appends to the existing transcript, so maya's
    // normal row diff handles the composer-shrink + new-turn-rows
    // transition correctly. agent_session.cpp fires commit_scrollback
    // only on the FROZEN_MAX trim; we mirror that here. Past versions
    // fired commit_scrollback_overflow on every submit to flush a
    // suspected composer-shrink seam, but the call advances prev_cells
    // past whatever the renderer thinks has overflowed — when nothing
    // has actually overflowed, the bookkeeping turns valid scrollback
    // mirror rows into "forget these" and the next diff re-emits
    // them, surfacing as duplicate cards in scrollback.
    std::vector<Cmd<Msg>> parts;
    if (!trim.is_none()) parts.push_back(std::move(trim));
    // Worktree snapshot rides the same batch as the stream launch: it
    // runs concurrently with the request's TTFB window, so by the time
    // the model asks for its first file edit the checkpoint is pinned.
    // Failure is silent by design — the rewind path re-verifies the ref
    // exists and surfaces a toast there instead.
    if (checkpoint_to_create) {
        parts.push_back(Cmd<Msg>::task_isolated(
            [id = std::move(*checkpoint_to_create)]
            (std::function<void(Msg)>) {
                (void)workspace::create_checkpoint(id);
            }));
    }
    parts.push_back(std::move(launch));
    auto cmd = parts.size() == 1
        ? std::move(parts.front())
        : Cmd<Msg>::batch(std::move(parts));
    return {std::move(m), std::move(cmd)};
}

std::string active_provider_id() {
    const auto& sel = provider::active();
    if (sel.kind == provider::Kind::OpenAI)
        return sel.openai_endpoint.label;
    if (sel.kind == provider::Kind::ExternalAcp)
        return sel.acp_agent_id;
    return std::string{provider::default_provider_id()};
}

// ── Entitlement accessors (see internal.hpp / domain/entitlement.hpp) ────
//
// (provider, account) is resolved HERE and nowhere else, so a call site
// cannot key an account-scoped fact by provider alone — the exact defect of
// the legacy account-blind bool this layer replaces.
namespace {
std::pair<std::string, std::string> entitlement_scope(std::string_view provider) {
    std::string pid = provider.empty() ? active_provider_id()
                                       : std::string{provider};
    // The registry's active label for this provider. Empty is legitimate and
    // means "the only account" — a single-account user keys under "", which
    // is why this needs no migration for the common case.
    std::string acct = auth::accounts::active_label(pid);
    return {std::move(pid), std::move(acct)};
}
} // namespace

bool entitlement_blocked(const store::Settings& s,
                         domain::entitlement::Fact f,
                         std::string_view model_id,
                         std::string_view provider) {
    const auto [pid, acct] = entitlement_scope(provider);
    return domain::entitlement::blocked(s.entitlements, f, pid, acct, model_id);
}

bool entitlement_record_blocked(store::Settings& s,
                                domain::entitlement::Fact f,
                                std::string_view model_id,
                                std::string_view provider) {
    const auto [pid, acct] = entitlement_scope(provider);
    const bool is_new = domain::entitlement::record_blocked(
        s.entitlements, f, pid, acct, model_id);
    if (is_new)
        AGT_LOG(Model, Info, "entitlement.blocked",
                "fact={} provider={} account={} model={}",
                domain::entitlement::tag(f), pid,
                acct.empty() ? std::string{"(only)"} : acct,
                model_id.empty() ? std::string_view{"(account-wide)"} : model_id);
    return is_new;
}

std::string model_for_provider(std::string_view spec) {
    const bool is_chatgpt =
        spec == "codex" || spec == "chatgpt" || spec == "codex-cli";
    const bool is_copilot = spec == "copilot";

    // 1) Recall the model the user last used on this provider.
    auto s = deps().load_settings();
    if (auto it = s.provider_models.find(std::string{spec});
        it != s.provider_models.end() && !it->second.empty()) {
        // ChatGPT's line-up is server-driven and changes over time: a slug the
        // account no longer offers (e.g. the retired `gpt-5.1-codex`) is
        // rejected on the very first turn. Only honour a recalled ChatGPT slug
        // if the LIVE catalog still lists it; otherwise fall through to the
        // account's current default so we never restore a dead model.
        if (!is_chatgpt && !is_copilot) return it->second;
        // Copilot: trust the recalled slug WITHOUT a network round-trip — this
        // runs on the UI thread during a provider switch, and hitting the live
        // catalog here is what made selecting Copilot lag. The async
        // fetch_models refetch corrects a stale slug a moment later.
        if (is_copilot) return it->second;
        // ChatGPT: validate the recall against the CACHED catalog only — the
        // same UI-thread rule as Copilot. list_models() would block on a
        // catalog HTTP fetch when the cache is cold (offline switch = full
        // timeout freeze). Cold cache ⇒ trust the recall; the async refetch
        // corrects a stale slug the moment ModelsLoaded lands.
        {
            auto cached = provider::chatgpt::list_models_cached();
            if (cached.empty()) return it->second;
            for (const auto& mi : cached)
                if (mi.id.value == it->second) return it->second;
        }
        // recalled slug is stale — drop through to default_model() below.
    }

    // 2) No recall — fall back to a sane built-in default per provider.
    //
    // DERIVED defaults first: ChatGPT's line-up is server-driven and changes
    // over time (gpt-5.4, not the stale gpt-5.1-codex we once hardcoded), so
    // it resolves from the CACHED catalog — never the network, this runs on
    // the UI thread during a switch. A cold cache returns empty and the
    // ModelsLoaded refetch auto-selects the account's real default.
    if (is_chatgpt) {
        auto cached = provider::chatgpt::list_models_cached();
        return cached.empty() ? std::string{} : cached.front().id.value;
    }
    // STATIC defaults come off the registry row. Empty (local backends,
    // aggregators) means "let the refetch auto-select the first available".
    if (const auto* row = provider::preset_for(spec.empty() ? "anthropic" : spec))
        return std::string{row->default_model};
    return {};
}

void reset_composer_draft(ComposerState& c) {
    c.text.clear();
    c.cursor = 0;
    c.attachments.clear();
    c.undo_stack.clear();
    c.redo_stack.clear();
    c.browsing = ComposerState::Live{};
    c.draft_save.reset();
    c.draft_save_attachments.clear();
    c.queued.clear();
}

void persist_settings(const Model& m) {
    // Load-modify-save: preserve provider, provider_keys, and the
    // per-provider model map that this function doesn't own. Building a
    // fresh Settings{} here would silently wipe the active provider on
    // every model-picker select.
    auto s = deps().load_settings();
    s.model_id = m.d.model_id;
    s.profile  = m.d.profile;
    // MERGE favorites, don't rebuild: `favorite_models` is one GLOBAL list
    // spanning every provider, but m.d.available_models only holds the
    // catalog of the provider loaded right now. Rebuilding the list from
    // that vector alone would silently erase every favorite belonging to
    // another backend (favorite Claude models → switch to Ollama → quit
    // persists → Anthropic favorites gone). So: reconcile only the ids
    // present in the live catalog; keep the rest untouched on disk.
    for (const auto& mi : m.d.available_models) {
        auto it = std::find(s.favorite_models.begin(),
                            s.favorite_models.end(), mi.id);
        if (mi.favorite && it == s.favorite_models.end())
            s.favorite_models.push_back(mi.id);
        else if (!mi.favorite && it != s.favorite_models.end())
            s.favorite_models.erase(it);
    }
    // Record this model as the active provider's last-used selection so a
    // later switch back to it restores exactly this model.
    if (!m.d.model_id.empty())
        s.provider_models[active_provider_id()] = m.d.model_id.value;
    s.effort = std::string{effort_wire(m.d.effort)};
    // Smart Mode: the whole config, one assignment. While the
    // AGENTTY_SMART_MODE session pin is active the in-memory `enabled` flag is
    // the ENV's value, not the user's choice — so the persisted preference is
    // kept as it was, and the pin never leaks into config.
    const bool keep_enabled = smart::tuning::enabled_override().has_value();
    const bool was_enabled  = s.smart.enabled;
    s.smart = m.d.smart;
    if (keep_enabled) s.smart.enabled = was_enabled;
    deps().save_settings(s);
    // Keep the subagent role-router (Layer 3b) in step with any Smart Mode
    // change the user just made in the overlay.
    tools::subagent::set_smart(m.d.smart);
    // …and the provider those pins are scoped to, so a worker resolves the
    // same slot the main turn would.
    tools::subagent::set_provider(active_provider_id());
}

std::pair<Model, maya::Cmd<Msg>>
commit_provider_switch(Model m, std::string_view spec,
                       auth::AuthHeader new_auth, std::string_view label,
                       std::string_view desired_model,
                       bool open_picker) {
    using maya::Cmd;
    const std::string spec_s{spec};

    // (1) File the OUTGOING model under its canonical provider id BEFORE
    //     provider::select swaps active() out from under us, so a later
    //     switch back restores exactly this model.
    const std::string outgoing_id = active_provider_id();

    // (2) Install the new selection (process-global; the stream seam reads
    //     active() at call time).
    provider::select(provider::parse_selection(spec_s));

    // (2b) Prewarm TLS/DNS to the NEWLY-active provider's host on a detached
    //      thread, so the first turn after a live switch is as fast as a cold
    //      start's prewarmed first turn. Uniform for Claude and ChatGPT/Codex;
    //      a no-op for locals / ACP. Fire-and-forget.
    provider::prewarm_active_provider();

    {
        auto settings = deps().load_settings();
        if (!m.d.model_id.empty())
            settings.provider_models[outgoing_id] = m.d.model_id.value;
        settings.provider = spec_s;
        deps().save_settings(settings);
    }

    // (3) Make a valid model active for the NEW backend. Priority:
    //     an EXPLICIT desired_model (the fused cross-provider picker
    //     pre-chose the exact target) → per-provider recall → built-in
    //     default → empty (ModelsLoaded auto-selects the first available).
    //     The explicit path is what makes a fused `Enter` an ATOMIC
    //     provider+model switch instead of "switch provider, then land on
    //     whatever the recall/default was."
    std::string next{desired_model};
    if (next.empty()) next = model_for_provider(spec_s);
    if (!next.empty()) {
        m.d.model_id    = ModelId{next};
        m.s.context_max = ui::context_max_for_model(m.d.model_id.value);
        tools::subagent::set_model(m.d.model_id.value);
    } else {
        // No model resolvable for the new backend yet (ChatGPT catalog not
        // reached, Ollama, …). CLEAR the model id — carrying the OUTGOING
        // provider's model here would make persist_settings (step 5) file a
        // cross-provider stale model under the new provider's id (the bug that
        // wrote `codex → claude-opus-*`). An empty id is skipped by
        // persist_settings and ModelsLoaded auto-selects the first available.
        m.d.model_id = ModelId{""};
        tools::subagent::set_model("");
    }

    // (4) Re-clamp the reasoning-effort tier to what the (possibly new)
    //     active model supports — a stale Xhigh/High carried onto a model
    //     without that tier (or a non-reasoning model) would otherwise show a
    //     bogus chip and get silently dropped only at request time. When the
    //     new model isn't known yet (local, empty id) this is a no-op until
    //     ModelsLoaded, which is fine — the wire path re-clamps regardless.
    //     Uniform for ChatGPT too: gpt-5.x decodes through Family::Gpt with
    //     an exact ladder, so the same clamp keeps chip == wire everywhere.
    //     EMPTY id = model unknown → do NOT clamp (resolved_caps("") has no
    //     effort support, so clamping would WIPE the user's tier rather than
    //     no-op; this is why ChatGPT — whose id is empty until ModelsLoaded —
    //     used to be excluded here). ModelsLoaded re-clamps with the real id.
    if (!m.d.model_id.value.empty())
        m.d.effort = clamp_effort(
            m.d.effort, resolved_caps(m.d.model_id.value));

    // (5) Persist the FULL settings shape (provider + per-provider model +
    //     effort + favorites) through the one owner so effort is never
    //     dropped on a hop, then swap the Deps auth and refetch models.
    persist_settings(m);

    app::switch_provider(std::move(new_auth));
    m.d.available_models.clear();
    m.s.models_loading = true;

    // Stale-token guard for the switched-TO provider. The account-picker
    // switch path (account_select) has always done this; the CROSS-provider
    // path did not — so switching openai→anthropic after the anthropic
    // token lapsed fired the first turn with a stale bearer (a guaranteed
    // 401 → reactive refresh → retry, i.e. a visibly slow first turn — or
    // a hard error when the refresh path had any other problem). Same
    // registry-driven gate: only rows that opt into proactive refresh.
    maya::Cmd<Msg> refresh_cmd = Cmd<Msg>::none();
    if (const auto* prow = provider::preset_for(spec_s);
        prow && prow->oauth_proactive_refresh && !m.s.oauth_refresh_in_flight) {
        if (auto tok = auth::oauth_proactive_refresh_token()) {
            m.s.oauth_refresh_in_flight = true;
            refresh_cmd = cmd::refresh_oauth(std::move(*tok));
        }
    }

    // Open the model picker immediately so the user sees "Loading models…"
    // the instant they switch, instead of an empty thread until they think
    // to hit /model. Done HERE, in the one shared switch helper, so EVERY
    // entry point (provider picker, custom host, api-key/chatgpt/copilot
    // login) gets it uniformly — not just the two login-modal paths the
    // feature originally patched. External ACP agents drive their own model
    // and expose no catalog (fetch_models returns empty), so opening the
    // picker there would just show a permanent "no models" box — skip them.
    //
    // BUT when the caller already pre-stashed a SPECIFIC model (the fused
    // picker: the user picked provider+model in one shot), popping the classic
    // "Loading models…" picker on top would be a jarring second picker for a
    // choice already made. Skip it — the desired model installs on ModelsLoaded
    // — and give a "Switching to X…" toast instead.
    const bool have_desired = !desired_model.empty();
    if (open_picker && provider::active().kind != provider::Kind::ExternalAcp
        && !have_desired)
        m.ui.panel = pn::FusedPicker{{0, ""}};

    // Name the DERIVED wire endpoint for OpenAI-dialect hosts so the /v1
    // defaulting is visible, not magic — the custom-host dead-loop report
    // came from users unable to see which path their spec actually dialed
    // ("host:8080/" → 404 forever with no clue). Hosted presets keep the
    // short label (their endpoint is not in question).
    std::string toast_text = have_desired
        ? "switching to " + ui::pretty_model_label(std::string{desired_model})
              + "  \xc2\xb7  " + std::string{label} + "\xe2\x80\xa6"
        : "provider \xe2\x86\x92 " + std::string{label};
    {
        const auto& sel = provider::active();
        if (!have_desired && sel.kind == provider::Kind::OpenAI
            && !sel.openai_endpoint.use_tls)
            toast_text += "  (" + sel.openai_endpoint.host + ":"
                        + std::to_string(sel.openai_endpoint.port)
                        + sel.openai_endpoint.path + ")";
    }
    auto toast = set_status_toast(m, std::move(toast_text),
                                  std::chrono::seconds{4});
    return {std::move(m),
            Cmd<Msg>::batch(std::move(toast), cmd::fetch_models(),
                            std::move(refresh_cmd))};
}

maya::Cmd<Msg> set_status_toast(Model& m, std::string text,
                                std::chrono::seconds ttl) {
    using maya::Cmd;
    m.s.status = std::move(text);
    auto now = std::chrono::steady_clock::now();
    m.s.status_until = now + ttl;
    auto stamp = m.s.status_until;
    return Cmd<Msg>::after(
        std::chrono::duration_cast<std::chrono::milliseconds>(ttl)
            + std::chrono::milliseconds{50},
        Msg{ClearStatus{stamp}});
}

} // namespace agentty::app::detail
