// symbol_update — reducer for `msg::SymbolPaletteMsg`. Parallel to
// mention.cpp (the @file picker); the only differences are the
// candidate type (SymbolEntry vs string) and the chip kind appended on
// select (Attachment::Symbol vs FileRef).

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"

#include <algorithm>
#include <utility>

#include <maya/core/overload.hpp>

#include "agentty/runtime/composer_attachment.hpp"
#include "agentty/runtime/panel/symbol.hpp"
#include "agentty/workspace/symbols.hpp"

namespace pn = agentty::ui::panel;

namespace agentty::app::detail {

using maya::overload;

Step symbol_update(Model m, msg::SymbolPaletteMsg sm) {
    return std::visit(overload{
        [&](OpenSymbolPalette) -> Step {
            // First call walks the workspace; cached after that. The
            // const-ref view is copied into the picker state so that
            // queries don't keep the global cache pinned (the cache
            // outlives the picker by design).
            symbol_palette::Open o;
            o.entries = list_workspace_symbols();
            m.ui.panel = pn::Symbol{std::move(o)};
            return done(std::move(m));
        },
        [&](CloseSymbolPalette) -> Step {
            m.ui.panel.close<pn::Symbol>();
            return done(std::move(m));
        },
        [&](SymbolPaletteInput& e) -> Step {
            auto* o = m.ui.panel.get<pn::Symbol>();
            if (o && static_cast<uint32_t>(e.ch) < 0x80
                  && e.ch >= 0x20) {
                // Fill a cold-opened snapshot once the parallel scan lands.
                if (o->entries.empty() && symbols_ready())
                    o->entries = list_workspace_symbols();
                o->query.push_back(static_cast<char>(e.ch));
                o->index = 0;
            }
            return done(std::move(m));
        },
        [&](SymbolPaletteBackspace) -> Step {
            auto* o = m.ui.panel.get<pn::Symbol>();
            if (!o) return done(std::move(m));
            if (o->query.empty()) {
                m.ui.panel.close<pn::Symbol>();
                return done(std::move(m));
            }
            o->query.pop_back();
            o->index = 0;
            return done(std::move(m));
        },
        [&](SymbolPaletteMove& e) -> Step {
            auto* o = m.ui.panel.get<pn::Symbol>();
            if (!o) return done(std::move(m));
            int sz = static_cast<int>(symbol_filtered(*o).size());
            if (sz <= 0) { o->index = 0; return done(std::move(m)); }
            o->index = std::clamp(o->index + e.delta, 0, sz - 1);
            return done(std::move(m));
        },
        [&](SymbolPaletteSelect) -> Step {
            auto* o = m.ui.panel.get<pn::Symbol>();
            if (!o) return done(std::move(m));
            const auto& matches = symbol_filtered(*o);
            if (matches.empty()
                || o->index < 0
                || o->index >= static_cast<int>(matches.size())) {
                m.ui.panel.close<pn::Symbol>();
                return done(std::move(m));
            }
            const auto& sym = o->entries[matches[
                static_cast<std::size_t>(o->index)]];
            Attachment att;
            att.kind        = Attachment::Kind::Symbol;
            att.name        = sym.name;
            att.path        = sym.path;
            att.line_number = sym.line_number;
            m.ui.panel.close<pn::Symbol>();

            std::size_t idx = m.ui.composer.attachments.size();
            m.ui.composer.attachments.push_back(std::move(att));
            auto placeholder = attachment::make_placeholder(idx);
            m.ui.composer.text.insert(m.ui.composer.cursor, placeholder);
            m.ui.composer.cursor += static_cast<int>(placeholder.size());
            return done(std::move(m));
        },
    }, sm);
}

} // namespace agentty::app::detail
