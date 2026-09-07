// Visual probe for the embeddings pane. Not a test — it prints the real
// rendered frame so a human can look at it. Same role as
// palette_render_probe.cpp.
//
//   ./embed_render_probe            closed dropdown
//   ./embed_render_probe --menu     dropdown open

#include <cstdio>
#include <cstdlib>
#include <string>

#include <maya/app/inline.hpp>
#include <maya/widget/panel.hpp>

#include "agentty/runtime/panel/form_keys.hpp"
#include "agentty/runtime/panel/rag.hpp"
#include "agentty/runtime/panel/smart_form.hpp"
#include "agentty/runtime/view/form_panel.hpp"
#include "agentty/runtime/view/palette.hpp"

int main(int argc, char** argv) {
    namespace eb = agentty::rag::embed;
    namespace rs = agentty::rag_settings;
    namespace sf = agentty::smart_form;

    const std::string arg = argc > 1 ? argv[1] : "";

    // Smart Mode, for eyeballing the shared form layer on a second pane.
    // --smart-advanced also shows the routing-policy rows (^A in the app).
    if (arg == "--smart" || arg == "--smart-advanced") {
        sf::Inputs in;
        in.enabled        = true;
        in.strategic      = {"Claude Opus 4.5", true};
        in.implementation = {"Claude Sonnet 4.5", false};
        in.utility        = {"Claude Haiku 4.5", false};
        in.advanced       = (arg == "--smart-advanced");
        auto f = sf::build_form(in);
        const auto out = maya::render_to_string_ansi(
            maya::Panel{agentty::ui::form_config(f, agentty::ui::success)}.build(), 84);
        std::printf("%s\n", out.c_str());
        return 0;
    }

    const bool open_menu = (arg == "--menu");
    const bool show_auto = (arg == "--auto");

    eb::EmbedConfig c;
    if (show_auto) {
        c.backend = eb::Backend::Auto;
    } else {
        c.backend = eb::Backend::OpenAI;
        c.host    = "gateway.internal";
        c.port    = 8443;
        c.tls     = true;
        c.api_key = "sk-not-printed";
    }
    c.model = "nomic-embed-text";

    const bool advanced = (arg == "--advanced" || arg == "--scroll");
    agentty::store::Settings settings;
    auto form = rs::build_form(c, agentty::store::RagMode::On, settings, advanced);
    form.subtitle = eb::describe(c);
    form.note     = "unsaved \xe2\x80\x94 ^T test, ^S save";
    form.cursor   = 0;

    // --scroll: a short viewport with the cursor near the END, which is the
    // case that actually failed on screen (the pane painted past the bottom
    // edge and the last rows were unreachable).
    maya::ScrollState scroll;
    int viewport = 0;
    if (arg == "--scroll") {
        viewport = 12;
        form.cursor = static_cast<int>(form.fields.size()) - 1;
    }

    if (open_menu) {
        auto a = agentty::form::keys::Action{agentty::form::keys::Intent::Activate};
        (void)agentty::form::keys::apply(form, a);
    }

    // --width=N renders at that column count, so the flex layout can be
    // verified at the narrow sizes where arithmetic-based versions broke.
    int width = 84;
    for (int i = 1; i < argc; ++i) {
        const std::string a{argv[i]};
        if (a.rfind("--width=", 0) == 0) width = std::atoi(a.c_str() + 8);
    }

    const auto out = maya::render_to_string_ansi(
        maya::Panel{agentty::ui::form_config(form, agentty::ui::info,
                                            viewport > 0 ? &scroll : nullptr,
                                            viewport,
                                            width)}.build(), width);
    std::printf("%s\n", out.c_str());
    return 0;
}
