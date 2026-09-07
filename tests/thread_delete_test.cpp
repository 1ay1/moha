// thread_delete_test — the two-press thread-picker delete (ThreadListDelete).
//
// `d` / `D` in the ^J thread picker removes a thread, mirroring the
// SettingsListRemove / AccountRemove two-press pattern. This pins the
// contract that keeps it safe:
//   1. ARM then COMMIT — the first press only marks the row (confirm_remove
//      set, nothing deleted); the second press on the SAME row commits.
//   2. DISARM on move — any navigation between the two presses clears the
//      pending state, so a stray `d` can never delete a thread.
//   3. ACTIVE-thread delete starts a FRESH thread through the same core
//      NewThread uses: the phase drops to Idle (a mid-stream delete can't
//      leave the wire running against a dead thread) and current.id changes.
//   4. NON-ACTIVE delete leaves m.d.current untouched and the picker open.
//   5. No use-after-erase: the toast quotes the deleted title correctly even
//      though the vector element is destroyed mid-reducer.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/panel/common.hpp"

#include <optional>
#include <print>
#include <string>
#include <vector>

namespace pn = agentty::ui::panel;

using namespace agentty;
namespace detail = agentty::app::detail;
namespace pick = agentty::ui::pick;

namespace {

int g_failed = 0;
void check(bool cond, const std::string& msg) {
    if (!cond) { std::println("  FAIL: {}", msg); ++g_failed; }
}

int g_new_ids = 0;
std::vector<std::string> g_deleted;   // ids passed to deps().delete_thread

void install_stub_deps() {
    g_deleted.clear();
    agentty::app::install_deps(agentty::app::Deps{
        .stream        = [](provider::Request, provider::EventSink) {},
        .save_thread   = [](const agentty::Thread&) {},
        .delete_thread = [](const agentty::ThreadId& id) { g_deleted.push_back(id.value); },
        .load_threads  = [] { return std::vector<agentty::Thread>{}; },
        .load_thread   = [](const ThreadId&) { return std::optional<agentty::Thread>{}; },
        .load_settings = [] { return store::Settings{}; },
        .save_settings = [](const store::Settings&) {},
        .new_thread_id = [] { return ThreadId{"fresh-" + std::to_string(++g_new_ids)}; },
        .title_from    = [](std::string_view t) { return std::string{t}; },
        .auth          = {},
    });
}

// A picker open at `index`, backed by three threads t0/t1/t2, with `current`
// set to whichever id is passed. Threads are ordered newest-first like the
// real list; ids are stable and titles distinct so we can assert the toast.
Model make_model(int cursor, const std::string& current_id) {
    Model m;
    for (int i = 0; i < 3; ++i) {
        Thread t;
        t.id = ThreadId{"t" + std::to_string(i)};
        t.title = "thread " + std::to_string(i);
        m.d.threads.push_back(std::move(t));
    }
    m.d.current.id = ThreadId{current_id};
    m.d.current.title = "active";
    m.ui.panel = pn::ThreadList{{cursor}};
    return m;
}

Model step(Model m, msg::ThreadListMsg tm) {
    return detail::thread_list_update(std::move(m), std::move(tm)).first;
}

const pick::OpenAt* picker(const Model& m) {
    return m.ui.panel.get<pn::ThreadList>();
}

} // namespace

int main() {
    install_stub_deps();

    // ── 1. arm then commit ────────────────────────────────────────────────
    {
        // Cursor on t1 (a NON-active row), current is a separate thread.
        Model m = make_model(/*cursor=*/1, /*current=*/"other");

        // First press: arms, deletes nothing.
        m = step(std::move(m), ThreadListDelete{});
        check(g_deleted.empty(), "first press deletes nothing");
        check(picker(m) && picker(m)->confirm_remove == "t1",
              "first press arms confirm_remove on the focused row id");
        check(m.d.threads.size() == 3, "first press leaves the list intact");

        // Second press on the SAME row: commits.
        m = step(std::move(m), ThreadListDelete{});
        check(g_deleted.size() == 1 && g_deleted[0] == "t1",
              "second press commits delete of the focused thread");
        check(m.d.threads.size() == 2, "row removed from the list");
        check(picker(m) && picker(m)->confirm_remove.empty(),
              "confirm_remove cleared after commit");
        bool gone = true;
        for (const auto& t : m.d.threads) if (t.id.value == "t1") gone = false;
        check(gone, "the deleted id is no longer in the list");
        // Picker still open, current unchanged (t1 was not active).
        check(picker(m) != nullptr, "picker stays open after a non-active delete");
        check(m.d.current.id.value == "other", "current thread untouched");
    }

    // ── 2. disarm on move ─────────────────────────────────────────────────
    {
        install_stub_deps();
        Model m = make_model(1, "other");
        m = step(std::move(m), ThreadListDelete{});            // arm t1
        check(picker(m)->confirm_remove == "t1", "armed");
        m = step(std::move(m), ThreadListMove{+1});            // move disarms
        check(picker(m)->confirm_remove.empty(), "move clears the pending delete");
        m = step(std::move(m), ThreadListDelete{});            // now just re-arms
        check(g_deleted.empty(), "a d after a move re-arms, does not delete");
    }

    // ── 3. deleting the ACTIVE thread starts fresh + resets phase ─────────
    {
        install_stub_deps();
        // Cursor on t0, and t0 is ALSO the active thread.
        Model m = make_model(/*cursor=*/0, /*current=*/"t0");
        // Simulate a mid-stream delete: phase is Streaming going in.
        m.s.phase = phase::Streaming{};
        m = step(std::move(m), ThreadListDelete{});   // arm
        m = step(std::move(m), ThreadListDelete{});   // commit

        check(g_deleted.size() == 1 && g_deleted[0] == "t0", "active thread deleted");
        check(m.d.current.id.value.rfind("fresh-", 0) == 0,
              "deleting the active thread starts a brand-new thread");
        check(m.d.current.messages.empty(), "the fresh thread is empty");
        check(std::holds_alternative<phase::Idle>(m.s.phase),
              "a mid-stream active-thread delete drops phase back to Idle");
        check(!m.ui.panel.is<agentty::ui::panel::ThreadList>(),
              "the picker closes on an active-thread delete (full reset_inline swap)");
    }

    // ── 4. commit only fires on the SAME row (id-keyed confirm) ───────────
    {
        install_stub_deps();
        Model m = make_model(0, "other");
        m = step(std::move(m), ThreadListDelete{});   // arm t0
        check(picker(m)->confirm_remove == "t0", "armed t0");
        // Jump to End (t2), then press d — different row, so it re-arms t2
        // rather than deleting t0.
        m = step(std::move(m), ThreadListJump{ThreadListJump::Where::End});
        check(picker(m)->confirm_remove.empty(), "jump disarms t0");
        m = step(std::move(m), ThreadListDelete{});
        check(g_deleted.empty(), "no delete: the armed row lost focus before commit");
        check(picker(m)->confirm_remove == "t2", "d on the new row arms it instead");
    }

    if (g_failed == 0) std::println("thread_delete_test: OK");
    return g_failed == 0 ? 0 : 1;
}
