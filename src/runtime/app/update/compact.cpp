// Auto-compaction reducer arms. Three entry points, all funnelled
// through `request_compact` so the auto-trigger and the manual
// /compact command share the same state-mutating code path.
//
// Trigger threshold: 85 % of the model's context cap. See the design
// note in include/moha/domain/session.hpp for why this is the right
// number — earlier compaction wastes the cache, later wastes nothing.

#include "moha/runtime/app/update/internal.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

#include "moha/runtime/app/cmd_factory.hpp"
#include "moha/runtime/app/deps.hpp"

namespace moha::app::detail {

namespace {

// Trigger when input tokens cross this fraction of the model's
// context window. Picked higher than the 50 % "common wisdom" because
// each compaction is a cache-bust event — doing it once at 85 %
// beats doing it twice at 50 %, and most threads finish before they
// ever cross the line.
constexpr double kCompactTriggerRatio = 0.85;

// Always preserve the user's first message (the original task brief)
// and the last `kKeepTail` messages verbatim so the model still has
// the immediate working set in full fidelity.
constexpr std::size_t kKeepTail = 6;

// Don't bother compacting if the eligible window is shorter than this
// — the round-trip to Haiku costs more tokens than it saves.
constexpr std::size_t kMinWindow = 4;

// Find the cut point: the largest index <= len - kKeepTail such that
// `messages[cut].role == User`. We always cut at a user boundary so
// the post-compaction conversation alternates U,A,U,A correctly:
//   [U_task, A_summary, U_keep_start, ..., A_last]
// The synthetic summary lives as an Assistant turn, restoring the
// alternation Anthropic's API requires.
[[nodiscard]] std::size_t pick_cut_point(const std::vector<Message>& msgs) {
    if (msgs.size() <= kKeepTail + 1) return 0;
    std::size_t target = msgs.size() - kKeepTail;
    while (target > 1 && msgs[target].role != Role::User) --target;
    return target;
}

} // namespace

maya::Cmd<Msg> auto_compact_if_due(Model& m) {
    if (m.s.is_compacting()) return maya::Cmd<Msg>::none();
    if (m.s.context_max <= 0) return maya::Cmd<Msg>::none();
    const auto threshold = static_cast<int>(m.s.context_max * kCompactTriggerRatio);
    if (m.s.tokens_in < threshold) return maya::Cmd<Msg>::none();
    if (!m.s.is_idle()) return maya::Cmd<Msg>::none();
    if (m.d.current.messages.size() < kMinWindow + kKeepTail + 1)
        return maya::Cmd<Msg>::none();
    // Schedule via a Msg round-trip so the user-initiated /compact
    // command and the auto-trigger share the same reducer arm. ~1 ms
    // delay is small enough to feel synchronous; using `after` instead
    // of an inline call keeps the reducer pure.
    return maya::Cmd<Msg>::after(std::chrono::milliseconds{1},
                                 Msg{CompactRequested{}});
}

Step request_compact(Model m) {
    if (m.s.is_compacting()) {
        return {std::move(m), set_status_toast(m, "compacting…", std::chrono::seconds{3})};
    }
    auto cut = pick_cut_point(m.d.current.messages);
    if (cut <= 1) {
        // Nothing to compact — too short, or no clean cut point.
        auto cmd = set_status_toast(m, "nothing to compact yet",
                                    std::chrono::seconds{3});
        return {std::move(m), std::move(cmd)};
    }

    // Slice [1, cut) — preserves messages[0] (the original task brief)
    // verbatim. We send the slice plus messages[0] to Haiku so it has
    // the framing context for the summary.
    std::vector<Message> for_summarizer;
    for_summarizer.reserve(cut);
    for_summarizer.push_back(m.d.current.messages.front());
    for (std::size_t i = 1; i < cut; ++i)
        for_summarizer.push_back(m.d.current.messages[i]);

    m.s.compaction = compact::Running{std::chrono::steady_clock::now()};
    m.s.status = "compacting…";
    // No status_until — the toast persists until CompactCompleted
    // overwrites it.

    auto compact_cmd = cmd::compact_thread(std::move(for_summarizer),
                                           /*first=*/1, /*last=*/cut);
    return {std::move(m), std::move(compact_cmd)};
}

Step apply_compact(Model m, CompactCompleted&& ev) {
    // Always clear the in-flight lock — even on failure, so the next
    // trigger isn't deadlocked.
    m.s.compaction = compact::Idle{};

    if (!ev.result) {
        // Soft-fail: leave the conversation untouched, surface a toast
        // so the user knows the auto-compact didn't take effect. They
        // can manually retry via /compact or simply continue typing.
        std::string msg = "compact failed: " + ev.result.error();
        auto cmd = set_status_toast(m, std::move(msg), std::chrono::seconds{6});
        return {std::move(m), std::move(cmd)};
    }

    auto& msgs = m.d.current.messages;
    // Defensive bounds check — between the compact request being issued
    // and CompactCompleted landing, the conversation may have grown
    // (new user submit, new tool turn). We splice into the *original*
    // recorded range as long as it's still within bounds; everything
    // appended after `last` is preserved naturally because the splice
    // doesn't touch it.
    if (ev.last > msgs.size() || ev.first >= ev.last) {
        auto cmd = set_status_toast(m,
            "compact: conversation moved during summarisation, skipped",
            std::chrono::seconds{4});
        return {std::move(m), std::move(cmd)};
    }

    // Build the synthesised assistant turn that replaces the elided
    // window. Frame it explicitly so the model knows what it's reading.
    Message summary_msg;
    summary_msg.role = Role::Assistant;
    summary_msg.text =
        "<conversation-summary>\n"
        "(Earlier turns elided to save context; the assistant wrote this "
        "summary of itself for hand-off.)\n\n"
        + std::move(*ev.result)
        + "\n</conversation-summary>";
    summary_msg.timestamp = std::chrono::system_clock::now();

    // Splice: erase [first, last) and insert summary_msg at `first`.
    msgs.erase(msgs.begin() + static_cast<std::ptrdiff_t>(ev.first),
               msgs.begin() + static_cast<std::ptrdiff_t>(ev.last));
    msgs.insert(msgs.begin() + static_cast<std::ptrdiff_t>(ev.first),
                std::move(summary_msg));

    // Token counter is best-effort: we don't know the exact new
    // input-token count until the next StreamUsage, but a coarse
    // estimate keeps the status bar honest in the meantime. Assume
    // the summary takes ~500 tokens and credit the savings.
    int messages_removed = static_cast<int>(ev.last - ev.first - 1);
    if (messages_removed > 0)
        m.s.tokens_in = std::max(0,
            m.s.tokens_in - messages_removed * 1500);  // ~1.5k tok/turn ballpark

    deps().save_thread(m.d.current);

    auto cmd = set_status_toast(m,
        "compacted " + std::to_string(messages_removed + 1)
        + " turns into a summary",
        std::chrono::seconds{4});
    return {std::move(m), std::move(cmd)};
}

} // namespace moha::app::detail
