// fork_update — reducer for the fork picker.
//
// Forking ESCAPES a full context window. It branches the current thread
// into a brand-new one (new id, forked_from = parent) that starts FRESH
// with near-zero context — NOT a copy or a summary of the parent. The
// parent's full transcript is exported to a Markdown file under
// ~/.agentty/threads, and the fork is seeded with a single synthetic
// `fork_note` message (Role::User) that:
//   • tells the MODEL where that transcript lives, to be read ON DEMAND
//     with the `read` tool only when earlier context is actually needed;
//   • renders as a visible "\u2443 Forked" event card so the fresh thread is
//     not a blank screen.
// This is the entire fork mechanism: no summarization, no verbatim copy,
// no up-front token cost — just a pointer the agent can follow lazily.
//
// The picker's only choice is how proactive RAG behaves in the fork,
// stored as the fork's per-thread rag_mode_override:
//
//   RAG per turn   → override = On
//   First-turn RAG → override = FirstTurnOnly
//   RAG off        → override = Off
//
// The original thread is saved and left completely untouched — a fork is
// non-destructive by construction.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/cmd_factory.hpp"

#include <chrono>
#include <utility>

#include <maya/core/overload.hpp>

#include "agentty/runtime/panel/fork.hpp"
#include "agentty/runtime/panel/common.hpp"
#include "agentty/runtime/panel/palette.hpp"
#include "agentty/store/store.hpp"
#include "agentty/io/persistence.hpp"
#include "agentty/tool/util/fs_helpers.hpp"
#include <mcp/tools/util/fs_helpers.hpp>
#include <filesystem>
#include "agentty/tool/skills.hpp"
#include "agentty/provider/selection.hpp"   // prewarm_active_provider

namespace pn = agentty::ui::panel;

namespace agentty::app::detail {

namespace fp   = agentty::fork_panel;
namespace pick = agentty::ui::pick;
using maya::Cmd;
using maya::overload;

namespace {

// Map a picker Choice to the fork's per-thread RAG override.
store::RagMode rag_mode_of(fp::Choice c) {
    switch (c) {
        case fp::Choice::RagPerTurn:   return store::RagMode::On;
        case fp::Choice::FirstTurnRag: return store::RagMode::FirstTurnOnly;
        case fp::Choice::RagOff:       return store::RagMode::Off;
    }
    return store::RagMode::Off;
}

const char* label_of(fp::Choice c) {
    switch (c) {
        case fp::Choice::RagPerTurn:   return "RAG per turn";
        case fp::Choice::FirstTurnRag: return "first-turn RAG";
        case fp::Choice::RagOff:       return "RAG off";
    }
    return "";
}

} // namespace

Step fork_update(Model m, msg::ForkMsg fm) {
    return std::visit(overload{
        [&](OpenFork) -> Step {
            if (m.d.current.messages.empty())
                return {std::move(m), set_status_toast(m, "nothing to fork yet")};
            if (!m.s.is_idle() || m.s.compacting || m.s.thread_loading)
                return {std::move(m),
                        set_status_toast(m, "cannot fork while the agent is working")};
            m.ui.panel = pn::Fork{{fp::Choice::RagPerTurn}};
            m.ui.panel.close<pn::Palette>();
            return {std::move(m), Cmd<Msg>::none()};
        },
        [&](CloseFork) -> Step {
            ascend(m);   // Esc: back to the palette that opened this, or close
            return {std::move(m), Cmd<Msg>::none()};
        },
        [&](ForkMove& e) -> Step {
            if (auto* o = m.ui.panel.get<pn::Fork>())
                o->choice = fp::next_choice(o->choice, e.delta);
            return {std::move(m), Cmd<Msg>::none()};
        },
        [&](ForkThread&) -> Step {
            const auto* picked = m.ui.panel.get<pn::Fork>();
            // No int->enum cast: the cursor already IS the choice, so a
            // reordered or resized row list cannot silently remap it.
            const fp::Choice choice = picked ? picked->choice
                                             : fp::Choice::RagOff;
            m.ui.panel.close<pn::Fork>();
            if (m.d.current.messages.empty())
                return {std::move(m), set_status_toast(m, "nothing to fork yet")};
            if (!m.s.is_idle() || m.s.compacting || m.s.thread_loading)
                return {std::move(m),
                        set_status_toast(m, "cannot fork while the agent is working")};

            // 1. Persist the parent untouched.
            deps().save_thread(m.d.current);
            const std::string parent_id = m.d.current.id.value;

            // 2. Write the parent's transcript to a clean, readable file
            //    the fork can READ ON DEMAND — the whole point of forking:
            //    escape a full context window CHEAPLY. The new thread starts
            //    EMPTY (near-zero tokens); the model pulls earlier context
            //    from disk only if it needs it (exactly like manually
            //    opening a new thread and asking it to read the old one).
            const std::filesystem::path transcript =
                persistence::write_thread_transcript_md(m.d.current);

            // The transcript lives under ~/.agentty/threads — OUTSIDE the
            // workspace the read tool is sandboxed to. Allowlist that dir
            // for reads (both the agentty and mcp-cpp fs layers, since
            // tools are served through mcp-cpp) so the model can actually
            // read the file the note points it at.
            if (!transcript.empty()) {
                const auto dir = persistence::threads_dir();
                tools::util::allow_read_root(dir);
                ::mcp::tools::util::allow_read_root(dir);
            }

            // 3. Build the fork: a FRESH, EMPTY thread with provenance +
            //    per-thread RAG override. No messages carried over.
            Thread fork;
            fork.id = deps().new_thread_id();
            fork.forked_from = parent_id;
            fork.rag_mode_override = rag_mode_of(choice);
            fork.created_at = fork.updated_at = std::chrono::system_clock::now();
            fork.title = m.d.current.title.rfind("Fork: ", 0) == 0
                             ? m.d.current.title
                             : ("Fork: " + (m.d.current.title.empty()
                                                ? std::string{"conversation"}
                                                : m.d.current.title));

            // 4. Seed the fork note: a synthetic Role::User message that
            //    (a) tells the MODEL the prior context exists and how to
            //    reach it, and (b) renders as a visible "\u2443 Forked" card so
            //    the fresh thread isn't a blank screen. It is a USER (not
            //    System) message on purpose: the ChatGPT/Responses transport
            //    DROPS mid-thread System messages (folds them into the
            //    top-level `instructions`), so a System note could silently
            //    vanish and the model would never learn the transcript
            //    exists. A User message survives every provider path. The
            //    `fork_note`/`fork_transcript` flags give it a quiet card
            //    identity and keep every "newest real user turn" scan
            //    (routing / RAG / history / loop-break) from mistaking it
            //    for the user's prompt — see conversation.hpp.
            {
                Message note;
                note.role = Role::User;
                note.fork_note = true;
                if (!transcript.empty()) {
                    note.fork_transcript = transcript.string();
                    note.text =
                        "This conversation is a fork of an earlier one. Its "
                        "full transcript is saved at:\n  " + transcript.string() +
                        "\nRead it with the `read` tool (or grep it) ONLY if "
                        "you need earlier context — don't read it "
                        "pre-emptively. The fork starts fresh precisely to "
                        "reclaim the context window; pull just the slice you "
                        "need.";
                } else {
                    // Transcript write failed (rare: disk/permissions). Still
                    // seed the card so the thread isn't blank and the model
                    // knows it's a fork, just without a readable pointer.
                    note.text =
                        "This conversation is a fork of an earlier one, "
                        "started fresh to reclaim the context window. The "
                        "prior transcript could not be exported to disk, so "
                        "earlier context isn't retrievable — ask the user if "
                        "you need it.";
                }
                fork.messages.push_back(std::move(note));
            }

            // 5. Switch to the fork — a fresh empty thread, so the render is
            //    trivially cheap (nothing to re-emit). rehydrate_frozen on
            //    an ~empty thread is a no-op; reset_inline paints the clean
            //    composer.
            tools::skills::reset_activations();
            m.ui.view_cache.clear();
            m.d.current = std::move(fork);
            deps().save_thread(m.d.current);
            m.ui.panel.close<pn::ThreadList>();
            rehydrate_frozen(m);
            m.ui.needs_warmup_render = !m.ui.frozen.empty();
            // The fork's first turn hits the network fresh — warm the socket
            // now (idle TTL has usually evicted the launch-time prewarm)
            // so it doesn't re-pay the handshake. Non-blocking.
            provider::prewarm_active_provider();

            auto toast = set_status_toast(
                m, std::string{"forked \xc2\xb7 fresh context · "} +
                       label_of(choice) +
                       " · prior transcript readable on demand",
                std::chrono::seconds{5});
            return {std::move(m),
                    Cmd<Msg>::batch(std::move(toast), Cmd<Msg>::reset_inline())};
        },
    }, fm);
}

} // namespace agentty::app::detail
