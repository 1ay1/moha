// checkpoint_update — reducer for the rewind checkpoint picker.
//
// Every user turn submitted inside a git repo pins a worktree snapshot
// and renders a checkpoint divider above it; this picker makes ALL of
// those points reachable (the palette's old "Rewind" only ever hit the
// newest). Open builds one entry per checkpointed turn, then kicks an
// async per-entry diff summary ("what has the worktree changed since
// here") so the row can show "N files · +A −D" without blocking the
// open on a big repo. Select maps the highlighted entry onto the
// existing RestoreCheckpoint flow (meta.cpp) — the destructive
// worktree+transcript rewind, unchanged.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

#include <maya/core/overload.hpp>

#include "agentty/runtime/panel/checkpoints.hpp"
#include "agentty/runtime/composer_attachment.hpp"
#include "agentty/workspace/checkpoint.hpp"

namespace pn = agentty::ui::panel;

namespace agentty::app::detail {

namespace cp = agentty::checkpoint_picker;
using maya::Cmd;
using maya::overload;

namespace {

// Flatten a prompt's first line for the row preview: chip placeholders
// become their visible labels (a raw \x01ATT:N\x01 sentinel would render
// as garbage), then clip to the first newline. Mirrors the composer-
// refill flattening in CheckpointRestored so preview and refill agree.
std::string preview_of(const Message& msg) {
    std::string flat;
    flat.reserve(msg.text.size());
    for (std::size_t i = 0; i < msg.text.size(); ) {
        if (static_cast<unsigned char>(msg.text[i]) == attachment::kSentinel) {
            auto len = attachment::placeholder_len_at(msg.text, i);
            if (len > 0) {
                auto idx = attachment::placeholder_index(msg.text, i);
                if (idx < msg.attachments.size()) {
                    flat.push_back('[');
                    flat.append(attachment::chip_label(msg.attachments[idx]));
                    flat.push_back(']');
                }
                i += len;
                continue;
            }
        }
        char c = msg.text[i++];
        if (c == '\n') break;   // first line only
        flat.push_back(c);
    }
    // Trim leading blanks so a prompt that starts with a chip/newline
    // still reads cleanly.
    std::size_t start = flat.find_first_not_of(" \t");
    if (start == std::string::npos) return "(no prompt text)";
    flat = flat.substr(start);
    // Cap the preview so a wall-of-text prompt can't dominate the row.
    // maya's Picker clips the leading cell to the row width regardless,
    // but a hard char cap here keeps the entry list cheap to lay out and
    // gives the async diffstat trailing cell room on even a wide term.
    // UTF-8-safe: back up off any continuation byte before appending the
    // ellipsis so we never slice a multibyte codepoint in half.
    constexpr std::size_t kMaxPreview = 96;
    if (flat.size() > kMaxPreview) {
        std::size_t cut = kMaxPreview;
        while (cut > 0 && (static_cast<unsigned char>(flat[cut]) & 0xC0) == 0x80)
            --cut;
        flat.erase(cut);
        flat += "\xe2\x80\xa6";   // …
    }
    return flat;
}

// Build the entry list from the current thread's checkpointed user
// turns, oldest-first (newest lands at the bottom, nearest the composer).
std::vector<cp::Entry> build_entries(const Model& m) {
    std::vector<cp::Entry> out;
    int user_turn = 0;
    for (const auto& msg : m.d.current.messages) {
        if (msg.role == Role::User) ++user_turn;
        if (msg.role != Role::User || !msg.checkpoint_id) continue;
        cp::Entry e;
        e.id      = *msg.checkpoint_id;
        e.turn    = user_turn;
        e.preview = preview_of(msg);
        e.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            msg.timestamp.time_since_epoch()).count();
        out.push_back(std::move(e));
    }
    return out;
}

// One Cmd per entry: compute its diff summary off the UI thread and
// dispatch CheckpointDiffLoaded back. checkpoint_summary shells git, so
// it must never run on the reducer. Batched so all entries load in
// parallel on the isolated pool.
Cmd<Msg> load_all_diffs(const std::vector<cp::Entry>& entries) {
    std::vector<Cmd<Msg>> parts;
    parts.reserve(entries.size());
    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
        parts.push_back(Cmd<Msg>::task_isolated(
            [i, id = entries[static_cast<std::size_t>(i)].id.value]
            (std::function<void(Msg)> dispatch) {
                auto d = workspace::checkpoint_summary(id);
                CheckpointDiffLoaded ev;
                ev.index         = i;
                ev.ok            = d.valid;
                ev.files_changed = d.files_changed;
                ev.insertions    = d.insertions;
                ev.deletions     = d.deletions;
                dispatch(Msg{std::move(ev)});
            }));
    }
    return parts.empty() ? Cmd<Msg>::none() : Cmd<Msg>::batch(std::move(parts));
}

} // namespace

Step checkpoint_update(Model m, msg::CheckpointMsg cm) {
    return std::visit(overload{
        [&](OpenCheckpointPicker) -> Step {
            // A rewind rewrites the worktree + transcript — only offer it
            // from a settled session (mirrors the RestoreCheckpoint gate).
            if (!m.s.is_idle() || m.s.compacting || m.s.thread_loading) {
                return {std::move(m),
                        set_status_toast(m, "cannot rewind while the agent is working")};
            }
            if (!workspace::in_git_repo()) {
                return {std::move(m),
                        set_status_toast(m, "checkpoints need a git repo")};
            }
            auto entries = build_entries(m);
            if (entries.empty()) {
                return {std::move(m),
                        set_status_toast(m, "no checkpoints in this thread yet")};
            }
            // Open on the newest (last) entry — the most common rewind
            // target, and the one the old single-shot path always took.
            const int last = static_cast<int>(entries.size()) - 1;
            auto diffs = load_all_diffs(entries);
            m.ui.panel = pn::Checkpoints{{std::move(entries), last}};
            m.ui.checkpoints_scroll = Model::UI::routed_scroll();
            return {std::move(m), std::move(diffs)};
        },
        [&](CloseCheckpointPicker) -> Step {
            m.ui.panel.close<pn::Checkpoints>();
            return done(std::move(m));
        },
        [&](CheckpointPickerMove& e) -> Step {
            auto* o = m.ui.panel.get<pn::Checkpoints>();
            if (!o || o->entries.empty()) return done(std::move(m));
            const int n = static_cast<int>(o->entries.size());
            o->index = (o->index + e.delta % n + n) % n;
            return done(std::move(m));
        },
        [&](CheckpointDiffLoaded& e) -> Step {
            auto* o = m.ui.panel.get<pn::Checkpoints>();
            if (!o) return done(std::move(m));   // picker closed mid-load
            if (e.index < 0 || e.index >= static_cast<int>(o->entries.size()))
                return done(std::move(m));
            auto& en = o->entries[static_cast<std::size_t>(e.index)];
            if (!e.ok) {
                en.diff_state = cp::Entry::DiffState::Failed;
                return done(std::move(m));
            }
            en.diff_state    = cp::Entry::DiffState::Ready;
            en.files_changed = e.files_changed;
            en.insertions    = e.insertions;
            en.deletions     = e.deletions;
            en.clean         = (e.files_changed == 0);
            return done(std::move(m));
        },
        [&](CheckpointPickerSelect) -> Step {
            auto* o = m.ui.panel.get<pn::Checkpoints>();
            if (!o || o->entries.empty()
                || o->index < 0 || o->index >= static_cast<int>(o->entries.size())) {
                m.ui.panel.close<pn::Checkpoints>();
                return done(std::move(m));
            }
            auto id = o->entries[static_cast<std::size_t>(o->index)].id;
            m.ui.panel.close<pn::Checkpoints>();
            // Hand off to the existing, battle-tested rewind: it re-gates on
            // Idle, restores files on an isolated worker, then truncates the
            // transcript + refills the composer in CheckpointRestored.
            return agentty::app::update(std::move(m),
                                        Msg{RestoreCheckpoint{std::move(id)}});
        },
    }, cm);
}

} // namespace agentty::app::detail
