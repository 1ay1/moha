// symbol_update — reducer for `msg::SymbolMsg`. Parallel to
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

Step symbol_update(Model m, msg::SymbolMsg sm) {
    return std::visit(overload{
        // (No OpenSymbol arm: the panel opens from the COMPOSER's `#`
        // handler, which builds pn::Symbol directly — see composer.cpp.)
        [&](CloseSymbol) -> Step {
            ascend(m);   // usually → thread (opened by typing #), or a parent
            return done(std::move(m));
        },
        [&](SymbolInput& e) -> Step {
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
        [&](SymbolBackspace) -> Step {
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
        [&](SymbolMove& e) -> Step {
            auto* o = m.ui.panel.get<pn::Symbol>();
            if (!o) return done(std::move(m));
            int sz = static_cast<int>(symbol_filtered(*o).size());
            if (sz <= 0) { o->index = 0; return done(std::move(m)); }
            o->index = std::clamp(o->index + e.delta, 0, sz - 1);
            return done(std::move(m));
        },
        [&](SymbolSelect) -> Step {
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
