#pragma once
// moha::store — the abstraction over "somewhere threads and settings live".
// The concept is pure domain; concrete adapters (FsStore, in-memory test
// stores, hypothetical cloud sync) live outside this header.

#include <concepts>
#include <string>
#include <string_view>
#include <vector>

#include "moha/domain/conversation.hpp"
#include "moha/domain/catalog.hpp"
#include "moha/domain/profile.hpp"

namespace moha::store {

// Persisted user settings — model + profile + favorites.  Lives with the
// Store concept because it's what the Store reads/writes, not because
// settings are themselves a first-class domain.
struct Settings {
    ModelId              model_id;
    Profile              profile = Profile::Write;
    std::vector<ModelId> favorite_models;
};

template <class S>
concept Store = requires(S& s, const Thread& t, const Settings& settings) {
    { s.load_threads() }     -> std::same_as<std::vector<Thread>>;
    { s.save_thread(t) }     -> std::same_as<void>;
    { s.load_settings() }    -> std::same_as<Settings>;
    { s.save_settings(settings) } -> std::same_as<void>;
    { s.new_id() }           -> std::convertible_to<ThreadId>;
    { s.title_from(std::string_view{}) } -> std::convertible_to<std::string>;
};

} // namespace moha::store
