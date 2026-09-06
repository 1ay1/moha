#pragma once
// agentty::app::Deps — type-erased handle to the runtime's seams.
//
// AgenttyApp's static methods need access to the Provider, Store, and
// credentials that main() wired up. Rather than templating AgenttyApp on three
// type parameters (which forces every translation unit to know the concrete
// types), we use a tiny vtable-style struct that the per-domain update code
// calls into.
//
// The concrete deps are stored once at startup via install_deps().  Anything
// satisfying the relevant concept can be installed; the concrete type stays
// hidden behind std::function-style erasure.

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "agentty/auth/auth.hpp"
#include "agentty/domain/conversation.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/provider/provider.hpp"
#include "agentty/runtime/app/settings_cache.hpp"
#include "agentty/store/store.hpp"

namespace agentty::app {

struct Deps {
    // ── Provider seam ────────────────────────────────────────────────────
    std::function<void(provider::Request, provider::EventSink)> stream;

    // ── Store seam (just the calls update.cpp actually makes) ────────────
    std::function<void(const Thread&)>          save_thread;
    std::function<void(const ThreadId&)>        delete_thread;
    // Returns thread *metadata* (empty messages) for the picker. Full
    // bodies are fetched on demand via load_thread.
    std::function<std::vector<Thread>()>        load_threads;
    std::function<std::optional<Thread>(const ThreadId&)> load_thread;
    std::function<store::Settings()>            load_settings;
    // WRITE-BEHIND. Returns immediately: the value is published to an
    // in-memory cache that `load_settings` reads, and the actual
    // load-modify-fsync-rename happens on a background worker.
    //
    // Why this is a SEAM concern and not each caller's problem: settings are
    // written from ~8 reducers (profile cycle, Smart Mode toggle, slot clear,
    // provider switch, model select, quit, RAG commit …). A reducer is a pure
    // function on the UI thread; a synchronous disk round-trip inside one
    // stalls the render loop, which is what made toggling Smart Mode hitch
    // mid-animation. Fixing that per call site is eight chances to forget —
    // and every new settings write would be a ninth. Fixing it HERE means no
    // reducer can block on settings IO even if it tries.
    //
    // Ordering: writes are applied in submission order on a single worker, so
    // a later save cannot land before an earlier one. Reads see the newest
    // submitted value immediately, so a save-then-load in the same reducer
    // observes what it just wrote.
    std::function<void(const store::Settings&)> save_settings;
    std::function<ThreadId()>                    new_thread_id;
    std::function<std::string(std::string_view)> title_from;
    // Persist a reviewed file's decided contents to disk. Used by diff-review
    // when the user rejects hunks: the file is rewritten with only the
    // ACCEPTED hunks kept (rejected ones reverted). (path, contents) — a
    // small, user-initiated write, so it runs synchronously in the reducer.
    std::function<void(const std::string&, const std::string&)> write_file;

    // ── Auth context (swapped live by update_auth / switch_provider) ─────
    // UI-THREAD readers may use this field directly (all writers run on the
    // UI thread, inside reducers). A WORKER thread must go through
    // auth_snapshot() instead — a bare read here races the UI thread's
    // move-assign during a live provider switch / login (torn std::string).
    auth::AuthHeader auth;
};

[[nodiscard]] const Deps& deps();
void install_deps(Deps d);

// Mutex-guarded copy of Deps::auth for WORKER-thread readers (e.g. the
// background model-catalog fetch). Same UI/worker split that gives
// provider::active() its lock — see selection.cpp.
[[nodiscard]] auth::AuthHeader auth_snapshot();

// Live-replace just the auth context after install. Used by the in-app
// login modal: when the user finishes signing in, the reducer dispatches
// a Cmd that calls this so the next stream pick up the new bearer
// without restarting the process. Safe to call from the UI thread —
// streams in flight cache the header at request-build time.
void update_auth(auth::AuthHeader auth);

// Live-switch the active provider after install. The provider picker
// dispatches a Cmd that calls this when the user selects a new backend:
// it installs the new `provider::Selection` (process-global) AND swaps
// `Deps::auth` to that provider's resolved credentials, so the next
// stream targets the new backend with the right key. The stream seam
// itself dispatches on `provider::active()` at call time, so no
// std::function needs replacing here. Safe to call from the UI thread.
void switch_provider(auth::AuthHeader auth);

// Convenience: bind a Provider + Store satisfying the concepts.
template <provider::Provider P, store::Store S>
void install(P& p, S& s, auth::AuthHeader auth) {
    // Settings IO goes through the write-behind cache. Wrapping HERE — the one
    // place a Deps is built — is what makes "a reducer never blocks on
    // settings IO" a property of the framework rather than a rule eight call
    // sites have to remember. See runtime/app/settings_cache.hpp.
    auto seam = settings_cache::wrap(
        [&s] { return s.load_settings(); },
        [&s](const store::Settings& x) { s.save_settings(x); });

    install_deps(Deps{
        .stream = [&p](provider::Request req, provider::EventSink sink) {
            p.stream(std::move(req), std::move(sink));
        },
        .save_thread     = [&s](const Thread& t) { s.save_thread(t); },
        .load_threads    = [&s] { return s.load_threads(); },
        .load_thread     = [&s](const ThreadId& id) { return s.load_thread(id); },
        .load_settings   = std::move(seam.load),
        .save_settings   = std::move(seam.save),
        .new_thread_id   = [&s] { return s.new_id(); },
        .title_from      = [&s](std::string_view t) { return s.title_from(t); },
        .auth            = std::move(auth),
    });
}

} // namespace agentty::app
