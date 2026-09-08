// settings_add_render_probe — the settings-list add-prompt, mid-edit:
//   ./agentty_standalone_tests settings_add_render_probe            short input
//   ./agentty_standalone_tests settings_add_render_probe --long     pasted long line (must WINDOW, not overflow)
//   ./agentty_standalone_tests settings_add_render_probe --mid      caret mid-string (←←←)
//   ./agentty_standalone_tests settings_add_render_probe --armed    browsing, delete armed
#include <cstdio>
#include <string>

#include <maya/app/inline.hpp>
#include <maya/core/render_context.hpp>

#include "agentty/runtime/model.hpp"
#include "agentty/runtime/panel/settings/list.hpp"
#include "agentty/runtime/view/panels.hpp"

using namespace agentty;
namespace pn = agentty::ui::panel;

int main(int argc, char** argv) {
    const std::string arg = argc > 1 ? argv[1] : "";

    Model m;
    pn::SettingsList o;
    o.concern = settings::Category::Plugins;
    if (arg == "--armed") {
        o.confirm_remove = "some-server";
    } else {
        o.input_active = true;
        if (arg == "--long") {
            o.input = "my-server uvx --from git+https://github.com/example/"
                      "some-very-long-repository-name mcp-server --flag one "
                      "--flag two --project";
            o.cursor = static_cast<int>(o.input.size());
        } else if (arg == "--mid") {
            o.input  = "my-server npx -y @example/mcp-thing";
            o.cursor = 9;   // just after "my-server"
        } else {
            o.input  = "my-server npx -y @example/mcp-thing";
            o.cursor = static_cast<int>(o.input.size());
        }
    }
    m.ui.panel = std::move(o);

    maya::RenderContext ctx{86, 40, maya::render_generation(), true};
    maya::RenderContextGuard g(ctx);
    auto el  = ui::settings_list_panel(m);
    auto out = maya::render_to_string(el, 86);
    std::fputs(out.c_str(), stdout);
    return 0;
}
