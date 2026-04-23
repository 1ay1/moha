// Composer-submission, thread-virtualization, and settings-persistence
// helpers for the update reducer. submit_message is the entry point for
// ComposerEnter / ComposerSubmit and is also called from finalize_turn when
// flushing the composer's queued-message buffer, which is why it lives in a
// shared internal header rather than an anonymous namespace.

#include "moha/runtime/app/update/internal.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

#include "moha/runtime/app/cmd_factory.hpp"
#include "moha/runtime/app/deps.hpp"
#include "moha/store/store.hpp"

namespace moha::app::detail {

namespace {

// Row-height estimate for a message — used only by the virtualization slice
// to tell maya how many rows of prev-frame to commit to scrollback.
// Conservative; an imperfect estimate costs at most one visible refresh of
// the window on the slice frame.
int estimate_message_rows(const Message& msg) {
    constexpr int kEstWidth = 100;
    int rows = 3;
    if (!msg.text.empty()) {
        const int w = std::max(1, kEstWidth - 4);
        rows += static_cast<int>(msg.text.size()) / w + 1;
        int nl = 0;
        for (char c : msg.text) if (c == '\n') ++nl;
        rows += nl;
    }
    for (const auto& tc : msg.tool_calls) {
        rows += 4;
        const auto& out = tc.output();
        if (!out.empty()) {
            int nl = 0;
            for (char c : out) if (c == '\n') ++nl;
            rows += std::min(nl, 10);
        }
    }
    return rows;
}

} // namespace

maya::Cmd<Msg> maybe_virtualize(Model& m) {
    using maya::Cmd;
    const int total = static_cast<int>(m.d.current.messages.size());
    const int visible = total - m.ui.thread_view_start;
    // Only slice in discrete chunks — a one-per-turn slice would refresh
    // the visible area every turn, whereas chunking it causes one refresh
    // every kSliceChunk turns.
    if (visible <= kViewWindow + kSliceChunk) return Cmd<Msg>::none();

    int committed_rows = 0;
    for (int i = m.ui.thread_view_start; i < m.ui.thread_view_start + kSliceChunk; ++i)
        committed_rows += estimate_message_rows(m.d.current.messages[i]);
    m.ui.thread_view_start += kSliceChunk;
    return Cmd<Msg>::commit_scrollback(committed_rows);
}

Step submit_message(Model m) {
    using maya::Cmd;
    if (m.ui.composer.text.empty()) return done(std::move(m));

    if (m.s.is_streaming() || m.s.is_executing_tool()) {
        m.ui.composer.queued.push_back(std::exchange(m.ui.composer.text, {}));
        m.ui.composer.cursor = 0;
        return done(std::move(m));
    }

    Message user;
    user.role = Role::User;
    user.text = std::exchange(m.ui.composer.text, {});
    m.ui.composer.cursor = 0;
    if (m.d.current.title.empty())
        m.d.current.title = deps().title_from(user.text);
    m.d.current.messages.push_back(std::move(user));

    Message placeholder;
    placeholder.role = Role::Assistant;
    m.d.current.messages.push_back(std::move(placeholder));

    m.d.current.updated_at = std::chrono::system_clock::now();
    m.s.phase = phase::Streaming{};
    m.s.active = true;
    m.s.truncation_retries = 0;
    auto virt = maybe_virtualize(m);
    auto launch = cmd::launch_stream(m);
    auto cmd = virt.is_none()
        ? std::move(launch)
        : Cmd<Msg>::batch(std::vector<Cmd<Msg>>{std::move(virt), std::move(launch)});
    return {std::move(m), std::move(cmd)};
}

void persist_settings(const Model& m) {
    store::Settings s;
    s.model_id = m.d.model_id;
    s.profile  = m.d.profile;
    for (const auto& mi : m.d.available_models)
        if (mi.favorite) s.favorite_models.push_back(mi.id);
    deps().save_settings(s);
}

} // namespace moha::app::detail
