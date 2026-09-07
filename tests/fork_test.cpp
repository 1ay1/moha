// fork_test.cpp — the fork reducer (fork_update / ForkThread).
//
// Fork = escape a full context window CHEAPLY. It does NOT copy the
// transcript and does NOT summarize (both still cost the window). Instead
// it creates a FRESH thread that carries only a tiny `fork_note` message
// (Role::User, fork_note=true) pointing at the parent's transcript on
// disk, which the model reads on demand. This test pins that contract:
//   1. The fork does NOT enter a compaction (no summarize — the old
//      design summarized, which sent the whole transcript to the model and
//      hit "prompt too long", and still cost context).
//   2. The fork is near-empty: it carries no user/assistant turns from the
//      parent — exactly one fork_note message (the transcript pointer).
//   3. The seeded note is a Role::User fork_note (NOT System — the
//      ChatGPT/Responses transport drops mid-thread System messages), it
//      carries the transcript path, and its wire text mentions the parent.
//   4. Fresh thread identity + forked_from provenance + the parent id
//      unchanged + a per-choice RAG override.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/app/deps.hpp"

#include <optional>
#include <print>
#include <string>
#include <vector>

using namespace agentty;
namespace detail = agentty::app::detail;

namespace {

int g_failed = 0;
void check(bool cond, const std::string& msg) {
    if (!cond) { std::println("  FAIL: {}", msg); ++g_failed; }
}

int g_thread_ids = 0;
void install_stub_deps() {
    agentty::app::install_deps(agentty::app::Deps{
        .stream        = [](provider::Request, provider::EventSink) {},
        .save_thread   = [](const agentty::Thread&) {},
        .delete_thread = [](const agentty::ThreadId&) {},
        .load_threads  = [] { return std::vector<agentty::Thread>{}; },
        .load_thread   = [](const ThreadId&) { return std::optional<agentty::Thread>{}; },
        .load_settings = [] { return store::Settings{}; },
        .save_settings = [](const store::Settings&) {},
        .new_thread_id = [] { return ThreadId{"fork-" + std::to_string(++g_thread_ids)}; },
        .title_from    = [](std::string_view) { return std::string{}; },
        .auth          = {},
    });
}

Model make_parent() {
    Model m;
    m.d.current.id = ThreadId{"parent"};
    m.d.current.title = "Some conversation";
    for (int i = 0; i < 4; ++i) {
        Message u; u.role = Role::User;      u.text = "q" + std::to_string(i);
        Message a; a.role = Role::Assistant; a.text = "a" + std::to_string(i);
        m.d.current.messages.push_back(std::move(u));
        m.d.current.messages.push_back(std::move(a));
    }
    return m;
}

Model fork_with(Model m, int choice_index) {
    auto s1 = detail::fork_update(std::move(m), OpenFork{});
    Model m1 = std::move(s1.first);
    for (int i = 0; i < choice_index; ++i) {
        auto s = detail::fork_update(std::move(m1), ForkMove{+1});
        m1 = std::move(s.first);
    }
    auto s2 = detail::fork_update(std::move(m1), ForkThread{});
    return std::move(s2.first);
}

void fresh_cheap_fork(int choice, const char* name) {
    std::println("--- fresh_cheap_fork: {} ---", name);
    Model forked = fork_with(make_parent(), choice);

    // 1. No compaction — the fork does NOT summarize (that cost the window
    //    and hit "prompt too long"). It's a fresh start. Concretely: no
    //    compaction is in flight AND the fork carries no CompactionRecord
    //    (the OLD design landed one, collapsing the wire prefix to a recap;
    //    the read-on-demand design must leave `compactions` empty so
    //    wire_messages_for_impl sends the fork's turns verbatim).
    check(!forked.s.compacting, "fork does NOT summarize (no compaction)");
    check(forked.d.current.compactions.empty(),
          "fork carries NO CompactionRecord (read-on-demand, not summarize)");

    // 2. Near-empty: the parent's 8 turns are NOT carried over. Exactly a
    //    single fork_note message (the on-disk transcript pointer).
    check(forked.d.current.messages.size() == 1,
          "fork carries no parent turns, just the note (got " +
          std::to_string(forked.d.current.messages.size()) + " messages)");
    for (const auto& msg : forked.d.current.messages) {
        // 3. The seeded note is a Role::User fork_note — NOT a System
        //    message, which the ChatGPT/Responses transport would drop.
        check(msg.role == Role::User,
              "the seeded note is a User message (survives every provider)");
        check(msg.fork_note,
              "the seeded note is flagged fork_note (quiet card + wire-kept)");
        check(!msg.text.empty(),
              "the fork note carries wire text the model actually reads");
        check(msg.text.find("fork") != std::string::npos,
              "the fork note text tells the model it's a fork");
        check(!msg.fork_transcript.empty(),
              "the fork note records the parent transcript path");
    }

    // 3. Fresh identity + provenance.
    check(forked.d.current.id.value != "parent",
          "fork has a new thread id (got '" + forked.d.current.id.value + "')");
    check(forked.d.current.forked_from == "parent",
          "fork records forked_from=parent");
    check(forked.d.current.title.rfind("Fork: ", 0) == 0,
          "fork title is prefixed (got '" + forked.d.current.title + "')");
    check(forked.d.current.rag_mode_override.has_value(),
          "fork carries a RAG-mode override");
    std::println("PASS\n");
}

void distinct_rag_modes() {
    std::println("--- distinct_rag_modes ---");
    // Each picker choice must map to a DIFFERENT mode. Compared as values,
    // not as integers: the override is an optional<RagMode>, so "absent"
    // and "a mode" are distinguishable without a sentinel.
    const auto m0 = fork_with(make_parent(), 0).d.current.rag_mode_override;
    const auto m1 = fork_with(make_parent(), 1).d.current.rag_mode_override;
    const auto m2 = fork_with(make_parent(), 2).d.current.rag_mode_override;
    check(m0 && m1 && m2, "every choice sets an override");
    auto name = [](const std::optional<agentty::RagMode>& v) {
        return v ? std::string{agentty::to_string(*v)} : std::string{"inherit"};
    };
    check(m0 != m1 && m1 != m2 && m0 != m2,
          "each RAG choice sets a distinct override (" +
          name(m0) + "," + name(m1) + "," + name(m2) + ")");
    std::println("PASS\n");
}

void empty_thread_no_fork() {
    std::println("--- empty_thread_no_fork ---");
    Model m;
    m.d.current.id = ThreadId{"empty"};
    Model after = fork_with(std::move(m), 0);
    check(after.d.current.id.value == "empty", "empty thread does not fork");
    std::println("PASS\n");
}

} // namespace

int main() {
    std::println("=== fork_test ===");
    install_stub_deps();
    fresh_cheap_fork(0, "RAG per turn");
    fresh_cheap_fork(1, "First-turn RAG");
    fresh_cheap_fork(2, "RAG off");
    distinct_rag_modes();
    empty_thread_no_fork();
    if (g_failed) { std::println("{} check(s) FAILED", g_failed); return 1; }
    std::println("All fork tests passed.");
    return 0;
}
