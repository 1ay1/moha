// login_render_probe — render each Panel-converted login state so a human
// (and CI) can look at it:
//   ./agentty_standalone_tests login_render_probe            account list
//   ./agentty_standalone_tests login_render_probe --confirm  delete armed
//   ./agentty_standalone_tests login_render_probe --key      API key input
//   ./agentty_standalone_tests login_render_probe --key-full key partly typed
//   ./agentty_standalone_tests login_render_probe --host     custom host input
//   ./agentty_standalone_tests login_render_probe --oauth    OAuth code entry
#include <cstdio>
#include <cstring>
#include <string>

#include <maya/app/inline.hpp>
#include <maya/core/render_context.hpp>

#include "agentty/runtime/login.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/view/login.hpp"

using namespace agentty;
namespace lg = agentty::ui::login;

int main(int argc, char** argv) {
    const std::string arg = argc > 1 ? argv[1] : "";

    Model m;
    if (arg == "--key" || arg == "--key-full") {
        lg::ApiKeyInput s;
        if (arg == "--key-full") {
            s.key_input = "sk-ant-api03-abcdefghij";
            s.cursor    = static_cast<int>(s.key_input.size());
        }
        m.ui.login = std::move(s);
    } else if (arg == "--host") {
        lg::CustomHostInput s;
        s.host_input = "gateway.internal:8443/v1#work";
        s.cursor     = static_cast<int>(s.host_input.size());
        m.ui.login = std::move(s);
    } else if (arg == "--oauth") {
        lg::OAuthCode s;
        s.authorize_url = "https://claude.ai/oauth/authorize?code=true&client_id=abcdef";
        m.ui.login = std::move(s);
    } else {
        lg::AccountList s;
        s.provider_label = "Anthropic";
        s.rows = {{"anthropic", "work",     true},
                  {"anthropic", "personal", false},
                  {"anthropic", "team-eu",  false}};
        s.cursor = 1;
        if (arg == "--confirm") s.confirm_remove = "personal";
        m.ui.login = std::move(s);
    }

    maya::RenderContext ctx{86, 40, maya::render_generation(), true};
    maya::RenderContextGuard g(ctx);
    auto el  = ui::login_modal(m);
    auto out = maya::render_to_string(el, 86);
    std::fputs(out.c_str(), stdout);
    return 0;
}
