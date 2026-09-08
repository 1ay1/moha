// agentty::app::update — pure (Model, Msg) -> (Model, Cmd<Msg>) reducer.
//
// Top-level orchestrator: a single 10-arm std::visit that dispatches on
// the domain (msg::ComposerMsg / msg::StreamMsg / …) and forwards to
// the matching per-domain reducer in update/<domain>.cpp.
//
// The previous version of this file inlined all 79 leaf arms in one
// overload{} — sizeof(Msg) was pinned by the heaviest leaf no matter
// which path was active, the std::visit instantiated a 79×N dispatch
// table, and any leaf change forced this whole TU to rebuild (~19 s on
// modest hardware). v2 splits the work: leaves are grouped into 10
// domain sub-variants in msg.hpp; each domain has its own visit in its
// own TU, so:
//
//   • this file's std::visit is 10 arms, one tiny dispatch table.
//   • update/<domain>.cpp recompiles only when its own leaves change.
//   • call sites still construct Msg via `Msg{ComposerEnter{}}` /
//     `dispatch(StreamTextDelta{...})` — std::variant's converting
//     constructor walks each domain alternative; only the matching
//     domain accepts a given leaf, so the wrap is unambiguous.

#include "agentty/runtime/app/update.hpp"

#include <utility>

#include <maya/core/overload.hpp>

#include "agentty/runtime/app/update/internal.hpp"

namespace agentty::app {

using maya::Cmd;
using maya::overload;

// (Removed) `is_user_input` previously gated the `needs_force_redraw`
// consumer below. That flag is gone — see model.hpp's comment for the
// rationale (the maya-side renderer fix made the post-stream redraw
// unnecessary, and firing it on every first keystroke was actively
// causing the scrollback-duplication symptom it was meant to prevent).

std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
    // One-shot warmup flag: set by ThreadLoaded, consumed by maya's
    // run loop on the very next render(). Clear on every subsequent
    // reducer step so a later thread load sees a clean false→true
    // edge (maya's loop only fires warmup_render on rising edges).
    // The ThreadLoaded handler in picker.cpp will set it back true
    // for its own swap before this clear-by-next-step path runs.
    const bool is_thread_load = std::visit([](const auto& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, msg::ThreadListMsg>) {
            return std::holds_alternative<::agentty::ThreadLoaded>(x);
        }
        return false;
    }, msg);
    if (!is_thread_load) m.ui.needs_warmup_render = false;

    auto step = std::visit(overload{
        [&](msg::ComposerMsg cm)       { return detail::composer_update     (std::move(m), std::move(cm)); },
        [&](msg::StreamMsg sm)         { return detail::stream_update       (std::move(m), std::move(sm)); },
        [&](msg::ToolMsg tm)           { return detail::tool_update         (std::move(m), std::move(tm)); },
        [&](msg::ProvidersMsg pm) { return detail::providers_update(std::move(m), std::move(pm)); },
        [&](msg::ModelsMsg pm)    { return detail::models_update  (std::move(m), std::move(pm)); },
        [&](msg::ThreadListMsg tm)     { return detail::thread_list_update  (std::move(m), std::move(tm)); },
        [&](msg::PaletteMsg pm) { return detail::palette_update      (std::move(m), std::move(pm)); },
        [&](msg::MentionMsg mm) { return detail::mention_update      (std::move(m), std::move(mm)); },
        [&](msg::SymbolMsg sm)  { return detail::symbol_update       (std::move(m), std::move(sm)); },
        [&](msg::CodeBlockMsg cm)      { return detail::codeblock_update    (std::move(m), std::move(cm)); },
        [&](msg::CheckpointMsg cm)     { return detail::checkpoint_update   (std::move(m), std::move(cm)); },
        [&](msg::RagMsg rm)    { return detail::rag_settings_update (std::move(m), std::move(rm)); },
        [&](msg::SettingsListMsg sm)   { return detail::settings_list_update(std::move(m), std::move(sm)); },
        [&](msg::ForkMsg fm)           { return detail::fork_update         (std::move(m), std::move(fm)); },
        [&](msg::TodoMsg tm)           { return detail::todo_update         (std::move(m), std::move(tm)); },
        [&](msg::LoginMsg lm)          { return detail::login_update        (std::move(m), std::move(lm)); },
        [&](msg::DiffReviewMsg dm)     { return detail::diff_review_update  (std::move(m), std::move(dm)); },
        [&](msg::SmartModeMsg sm)      { return detail::smart_mode_update   (std::move(m), std::move(sm)); },
        [&](msg::MetaMsg mm)           { return detail::meta_update         (std::move(m), std::move(mm)); },
    }, msg);

    return step;
}

} // namespace agentty::app
