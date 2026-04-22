#pragma once
// moha::app::Deps — type-erased handle to the runtime's seams.
//
// MohaApp's static methods need access to the Provider, Store, and
// credentials that main() wired up. Rather than templating MohaApp on three
// type parameters (which forces every translation unit to know the concrete
// types), we use a tiny vtable-style struct that the per-domain update code
// calls into.
//
// The concrete deps are stored once at startup via install_deps().  Anything
// satisfying the relevant concept can be installed; the concrete type stays
// hidden behind std::function-style erasure.

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "moha/auth/auth.hpp"
#include "moha/domain/conversation.hpp"
#include "moha/runtime/msg.hpp"
#include "moha/provider/provider.hpp"
#include "moha/store/store.hpp"

namespace moha::app {

struct Deps {
    // ── Provider seam ────────────────────────────────────────────────────
    std::function<void(provider::Request, provider::EventSink)> stream;

    // ── Store seam (just the calls update.cpp actually makes) ────────────
    std::function<void(const Thread&)>          save_thread;
    std::function<std::vector<Thread>()>        load_threads;
    std::function<store::Settings()>            load_settings;
    std::function<void(const store::Settings&)> save_settings;
    std::function<ThreadId()>                    new_thread_id;
    std::function<std::string(std::string_view)> title_from;

    // ── Auth context (immutable for the session) ─────────────────────────
    std::string auth_header;
    auth::Style auth_style = auth::Style::ApiKey;
};

[[nodiscard]] const Deps& deps();
void install_deps(Deps d);

// Convenience: bind a Provider + Store satisfying the concepts.
template <provider::Provider P, store::Store S>
void install(P& p, S& s, std::string auth_header, auth::Style style) {
    install_deps(Deps{
        .stream = [&p](provider::Request req, provider::EventSink sink) {
            p.stream(std::move(req), std::move(sink));
        },
        .save_thread     = [&s](const Thread& t) { s.save_thread(t); },
        .load_threads    = [&s] { return s.load_threads(); },
        .load_settings   = [&s] { return s.load_settings(); },
        .save_settings   = [&s](const store::Settings& x) { s.save_settings(x); },
        .new_thread_id   = [&s] { return s.new_id(); },
        .title_from      = [&s](std::string_view t) { return s.title_from(t); },
        .auth_header     = std::move(auth_header),
        .auth_style      = style,
    });
}

} // namespace moha::app
