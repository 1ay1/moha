// tool_output.cpp — the tool-output viewer panel's reducer (^O): the
// list/body two-stage navigation, clipboard copy, and tail-follow scroll.
//
// collect_viewer_entries / resync_live_tool_viewer stay in tool.cpp beside
// the execution state they read (declared in internal.hpp) — they are the
// shared seam between the panel and the execution plumbing.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"

#include <algorithm>
#include <utility>

#include <maya/core/overload.hpp>

#include "agentty/io/clipboard.hpp"          // write_clipboard_text
#include "agentty/runtime/panel/tool_output.hpp"

namespace pn = agentty::ui::panel;

namespace agentty::app::detail {

using maya::overload;

Step tool_output_update(Model m, msg::ToolOutputMsg tm) {
    return std::visit(overload{
        [&](OpenToolOutput) -> Step {
            auto entries = collect_viewer_entries(m);
            if (entries.empty()) {
                auto cmd = set_status_toast(m, "nothing to inspect yet");
                return {std::move(m), std::move(cmd)};
            }
            m.ui.panel = pn::ToolOutput{{std::move(entries), 0, false}};
            m.ui.tool_viewer_scroll.y = 0;
            m.ui.tool_viewer_tail = true;
            return done(std::move(m));
        },
        [&](CloseToolOutput) -> Step {
            // Esc semantics: body stage → back to the list; list → unwind
            // one level (the palette that opened this, or the thread).
            if (auto* o = m.ui.panel.get<pn::ToolOutput>(); o && o->viewing) {
                o->viewing = false;
                m.ui.tool_viewer_scroll.y = 0;
                return done(std::move(m));
            }
            ascend(m);
            return done(std::move(m));
        },
        [&](ToolOutputMove& e) -> Step {
            auto* o = m.ui.panel.get<pn::ToolOutput>();
            if (!o) return done(std::move(m));
            if (o->viewing) {
                // Body stage: deltas scroll the output viewport directly.
                // max_y is paint-written-back by the Picker widget.
                auto& sc = m.ui.tool_viewer_scroll;
                sc.y = std::clamp(sc.y + e.delta, 0, std::max(0, sc.max_y));
                // Live tail-follow: scrolling UP off the bottom disengages
                // auto-tail so the user can read earlier output; scrolling
                // (or Ending) back to the bottom re-engages it.
                m.ui.tool_viewer_tail = (sc.y >= sc.max_y);
            } else {
                int sz = static_cast<int>(o->entries.size());
                if (sz > 0)
                    o->index = std::clamp(o->index + e.delta, 0, sz - 1);
            }
            return done(std::move(m));
        },
        [&](ToolOutputSelect) -> Step {
            auto* o = m.ui.panel.get<pn::ToolOutput>();
            if (!o || o->viewing) return done(std::move(m));
            if (o->index < 0
                || o->index >= static_cast<int>(o->entries.size()))
                return done(std::move(m));
            o->viewing = true;
            m.ui.tool_viewer_scroll.y = 0;
            m.ui.tool_viewer_tail = true;
            return done(std::move(m));
        },
        [&](ToolOutputStep& e) -> Step {
            // ←/→ while reading an output: hop to the neighbouring
            // entry's body directly. Clamped at the ends (no wrap — the
            // list is short and wrap-around disorients more than it
            // helps). List stage: no-op.
            auto* o = m.ui.panel.get<pn::ToolOutput>();
            if (!o || !o->viewing) return done(std::move(m));
            int sz = static_cast<int>(o->entries.size());
            if (sz <= 0) return done(std::move(m));
            int next = std::clamp(o->index + e.delta, 0, sz - 1);
            if (next != o->index) {
                o->index = next;
                m.ui.tool_viewer_scroll.y = 0;
            }
            return done(std::move(m));
        },
        [&](ToolOutputCopy) -> Step {
            auto* o = m.ui.panel.get<pn::ToolOutput>();
            if (!o) return done(std::move(m));
            if (o->index < 0
                || o->index >= static_cast<int>(o->entries.size()))
                return done(std::move(m));
            std::string body =
                o->entries[static_cast<std::size_t>(o->index)].output;
            (void)write_clipboard_text(body);   // native pbcopy/wl-copy/xclip
            auto toast = set_status_toast(m, "tool output copied to clipboard");
            return {std::move(m), maya::Cmd<Msg>::batch(
                maya::Cmd<Msg>::write_clipboard(std::move(body)),
                std::move(toast))};
        },
    }, tm);
}

} // namespace agentty::app::detail
