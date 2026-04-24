#include "moha/runtime/app/deps.hpp"

#include <stdexcept>

namespace moha::app {

namespace {
Deps* g_deps = nullptr;
}

const Deps& deps() {
    if (!g_deps) throw std::logic_error("moha::app::deps() called before install_deps()");
    return *g_deps;
}

void install_deps(Deps d) {
    static Deps storage;
    storage = std::move(d);
    g_deps = &storage;
}

void update_auth(std::string header, auth::Style style) {
    if (!g_deps) return;
    g_deps->auth_header = std::move(header);
    g_deps->auth_style  = style;
}

} // namespace moha::app
