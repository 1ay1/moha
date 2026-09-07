// diff_review_test — the review-before-apply feature, end to end. Proves the
// re-wired flow: a tool's FileChange lands in m.d.pending_changes, and the
// accept / reject reducers persist the RIGHT contents to disk (accept keeps
// what the tool wrote, reject reverts to original).
//
// Background: the population path (ToolExecOutput.change → apply_tool_output →
// pending_changes) was reverted long ago, leaving the whole diff-review
// feature dead. This locks the resurrected wiring so it can't silently die
// again.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/panel/common.hpp"
#include "agentty/diff/diff.hpp"

#include <map>
#include <optional>
#include <print>
#include <string>

namespace pn = agentty::ui::panel;

using namespace agentty;
namespace detail = agentty::app::detail;
namespace pick = agentty::ui::pick;

namespace {
int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::println("  FAIL: {}", what); ++g_fail; }
}

// Captures every write_file the reducer performs, so we can assert what would
// hit disk without touching the real filesystem.
std::map<std::string, std::string> g_writes;

void install_stub_deps() {
    g_writes.clear();
    agentty::app::install_deps(agentty::app::Deps{
        .stream        = [](provider::Request, provider::EventSink) {},
        .save_thread   = [](const Thread&) {},
        .delete_thread = [](const ThreadId&) {},
        .load_threads  = [] { return std::vector<Thread>{}; },
        .load_thread   = [](const ThreadId&) { return std::optional<Thread>{}; },
        .load_settings = [] { return store::Settings{}; },
        .save_settings = [](const store::Settings&) {},
        .new_thread_id = [] { return ThreadId{}; },
        .title_from    = [](std::string_view t) { return std::string{t}; },
        .write_file    = [](const std::string& p, const std::string& c) { g_writes[p] = c; },
        .auth          = {},
    });
}

// A model with one live tool call, so apply_tool_output can settle it.
Model with_live_tool(const char* id) {
    Model m;
    Message a; a.role = Role::Assistant;
    ToolUse tc; tc.id = ToolCallId{id}; tc.name = ToolName{"edit"};
    tc.status = ToolUse::Running{};
    a.tool_calls.push_back(std::move(tc));
    m.d.current.messages.push_back(std::move(a));
    return m;
}

FileChange make_change(const char* path, const std::string& before,
                       const std::string& after) {
    auto fc = diff::compute(path, before, after);
    fc.original_contents = before;
    fc.new_contents = after;
    return fc;
}
} // namespace

int main() {
    const std::string before = "line one\nline two\nline three\n";
    const std::string after  = "line one\nline TWO\nline three\n";

    // ── population: a tool's FileChange lands in pending_changes ──────────
    {
        install_stub_deps();
        Model m = with_live_tool("t1");
        detail::apply_tool_output(m, ToolCallId{"t1"},
            std::expected<std::string, tools::ToolError>{"edited login.cpp"},
            make_change("login.cpp", before, after));
        check(m.d.pending_changes.size() == 1, "tool FileChange queued for review");
        check(!m.d.pending_changes.empty()
              && m.d.pending_changes[0].path == "login.cpp",
              "queued change carries the path");
        check(!m.d.pending_changes[0].hunks.empty(), "change has structured hunks");
    }

    // ── dedup: two edits to the SAME file collapse to one net entry ──────
    {
        install_stub_deps();
        Model m = with_live_tool("t1");
        detail::apply_tool_output(m, ToolCallId{"t1"},
            std::expected<std::string, tools::ToolError>{"e1"},
            make_change("f.txt", before, after));
        // second edit builds on the first's output
        const std::string after2 = "line ONE\nline TWO\nline three\n";
        Model m2 = with_live_tool("t2");
        m2.d.pending_changes = m.d.pending_changes;   // carry the queue
        detail::apply_tool_output(m2, ToolCallId{"t2"},
            std::expected<std::string, tools::ToolError>{"e2"},
            make_change("f.txt", after, after2));
        check(m2.d.pending_changes.size() == 1,
              "same-file edits dedup to one review entry");
        check(m2.d.pending_changes[0].original_contents == before,
              "dedup keeps the ORIGINAL first-seen contents");
        check(m2.d.pending_changes[0].new_contents == after2,
              "dedup reflects the newest contents");
    }

    // ── a FAILED tool queues nothing ─────────────────────────────────────
    {
        install_stub_deps();
        Model m = with_live_tool("t1");
        detail::apply_tool_output(m, ToolCallId{"t1"},
            std::unexpected(tools::ToolError::io("disk full")),
            make_change("nope.txt", before, after));
        check(m.d.pending_changes.empty(), "a failed tool queues no change");
    }

    // ── REJECT ALL reverts every file to its original on disk ────────────
    {
        install_stub_deps();
        Model m = with_live_tool("t1");
        detail::apply_tool_output(m, ToolCallId{"t1"},
            std::expected<std::string, tools::ToolError>{"ok"},
            make_change("a.txt", before, after));
        m.ui.panel = agentty::ui::panel::DiffReview{{0, 0}};
        // Two-press guard (commit 7498bf3f): from the OPEN pane the first
        // ^X arms (no write), the second executes. Palette-driven reject
        // (pane closed) executes on the first press.
        auto armed = detail::diff_review_update(std::move(m), RejectAllChanges{});
        check(g_writes.count("a.txt") == 0,
              "first reject-all press only arms, no write yet");
        check(armed.first.ui.panel.get<pn::DiffReview>()
                  && armed.first.ui.panel.get<pn::DiffReview>()->confirm_reject_all,
              "first reject-all press arms the confirm flag");
        auto s = detail::diff_review_update(std::move(armed.first), RejectAllChanges{});
        check(g_writes.count("a.txt") == 1, "reject-all wrote the file");
        check(g_writes["a.txt"] == before,
              "reject-all reverted the file to ORIGINAL contents");
        check(s.first.d.pending_changes.empty(), "queue cleared after reject-all");
        check(!s.first.ui.panel.is<agentty::ui::panel::DiffReview>(),
              "pane closes after reject-all");
    }

    // ── ACCEPT ALL keeps the tool's write (no disk revert) ───────────────
    {
        install_stub_deps();
        Model m = with_live_tool("t1");
        detail::apply_tool_output(m, ToolCallId{"t1"},
            std::expected<std::string, tools::ToolError>{"ok"},
            make_change("b.txt", before, after));
        m.ui.panel = agentty::ui::panel::DiffReview{{0, 0}};
        auto s = detail::diff_review_update(std::move(m), AcceptAllChanges{});
        check(g_writes.empty(), "accept-all writes nothing (tool already wrote)");
        check(s.first.d.pending_changes.empty(), "queue cleared after accept-all");
    }

    // ── per-hunk REJECT then Close reverts just that hunk ────────────────
    {
        install_stub_deps();
        // Two independent hunks in one file — changes far apart so the diff
        // engine emits distinct hunks (need >3 unchanged lines between them).
        const std::string b =
            "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n";
        const std::string a =
            "ONE\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\nFIFTEEN\n";
        Model m = with_live_tool("t1");
        detail::apply_tool_output(m, ToolCallId{"t1"},
            std::expected<std::string, tools::ToolError>{"ok"},
            make_change("c.txt", b, a));
        auto& fc0 = m.d.pending_changes[0];
        check(fc0.hunks.size() >= 2, "distinct edits produce >=2 hunks");
        m.ui.panel = agentty::ui::panel::DiffReview{{0, 0}};
        // Accept the first hunk, reject the second, then close.
        auto s1 = detail::diff_review_update(std::move(m), AcceptHunk{});
        auto s2 = detail::diff_review_update(std::move(s1.first), RejectHunk{});
        auto s3 = detail::diff_review_update(std::move(s2.first), CloseDiffReview{});
        check(g_writes.count("c.txt") == 1, "close persisted the mixed decision");
        // Written file keeps the accepted hunk (ONE) but reverts the rejected
        // one (FIFTEEN→15): starts with 'ONE', ends with '15'.
        const std::string& w = g_writes["c.txt"];
        check(w.rfind("ONE", 0) == 0, "accepted hunk kept (ONE at top)");
        check(w.find("FIFTEEN") == std::string::npos, "rejected hunk reverted (no FIFTEEN)");
        check(w.find("\n15\n") != std::string::npos, "rejected line restored to 15");
    }

    // ── submitting a new message clears the review window ──────────────
    // The strip covers ONE window (agent-edits → your next message); moving on
    // implicitly accepts, so the queue must not linger across turns.
    {
        install_stub_deps();
        Model m = with_live_tool("t1");
        detail::apply_tool_output(m, ToolCallId{"t1"},
            std::expected<std::string, tools::ToolError>{"ok"},
            make_change("z.txt", before, after));
        check(!m.d.pending_changes.empty(), "queued before submit");
        m.d.model_id = ModelId{"claude-sonnet-4-5"};  // submit needs a model
        m.ui.composer.text = "next question";
        auto s = detail::submit_message(std::move(m));
        check(s.first.d.pending_changes.empty(),
              "submitting a new message clears the pending-changes queue");
    }

    // ── empty model id: submit must NOT start a turn ────────────────────
    // Regression for the local-preset "dead loop when prompted": a freshly
    // selected llama.cpp/Ollama has model_id="" until its /models fetch lands.
    // Sending "model":"" is rejected by the server and re-fires the retry
    // machine forever. submit_message must refuse cleanly, keep the composer
    // text, and NOT push a user message / assistant placeholder.
    {
        install_stub_deps();
        Model m;
        m.d.model_id = ModelId{""};             // no model resolved yet
        m.ui.composer.text = "hello local model";
        const std::size_t before_n = m.d.current.messages.size();
        auto s = detail::submit_message(std::move(m));
        check(s.first.d.current.messages.size() == before_n,
              "empty-model submit pushes NO message (no dead-loop turn)");
        check(s.first.ui.composer.text == "hello local model",
              "empty-model submit keeps the composer text (nothing lost)");
    }

    // ── the FULL reducer path: ToolExecOutput → tool_update → pending_changes
    // (my other tests call apply_tool_output directly; this proves the reducer
    // arm actually forwards e.change, i.e. the dispatch-site wiring holds).
    {
        install_stub_deps();
        Model m = with_live_tool("t1");
        ToolExecOutput e{
            ToolCallId{"t1"},
            std::expected<std::string, tools::ToolError>{"edited"},
            make_change("wired.cpp", before, after)};
        auto s = detail::tool_update(std::move(m), msg::ToolMsg{std::move(e)});
        check(s.first.d.pending_changes.size() == 1,
              "ToolExecOutput through the reducer queues the change");
        check(!s.first.d.pending_changes.empty()
              && s.first.d.pending_changes[0].path == "wired.cpp",
              "the reducer forwarded the FileChange, not dropped it");
    }

    // ── multi-file edit (replace) queues EVERY touched file for review ───
    {
        install_stub_deps();
        Model m = with_live_tool("t1");
        std::vector<FileChange> multi;
        multi.push_back(make_change("a.ts", before, after));
        multi.push_back(make_change("b.ts", before, after));
        multi.push_back(make_change("c.ts", before, after));
        detail::apply_tool_output(m, ToolCallId{"t1"},
            std::expected<std::string, tools::ToolError>{"replaced across 3 files"},
            std::nullopt, std::move(multi));
        check(m.d.pending_changes.size() == 3,
              "a multi-file replace queues all 3 files");
        bool a=false,b=false,c=false;
        for (auto& fc : m.d.pending_changes) {
            if (fc.path == "a.ts") a = true;
            if (fc.path == "b.ts") b = true;
            if (fc.path == "c.ts") c = true;
        }
        check(a && b && c, "every replaced file is in the review queue");
    }

    if (g_fail == 0) std::println("diff_review_test: OK");
    return g_fail ? 1 : 0;
}
