#include "moha/runtime/app/program.hpp"
#include "moha/runtime/view/helpers.hpp"

namespace moha::app {

namespace {
std::vector<ModelInfo> seed_models() {
    return {
        {ModelId{"claude-opus-4-5"},   "Claude Opus 4.5",   "anthropic", 200000, true},
        {ModelId{"claude-sonnet-4-5"}, "Claude Sonnet 4.5", "anthropic", 200000, true},
        {ModelId{"claude-haiku-4-5"},  "Claude Haiku 4.5",  "anthropic", 200000, false},
    };
}
} // namespace

Model init() {
    Model m;
    m.d.threads          = deps().load_threads();
    m.d.available_models = seed_models();

    auto settings = deps().load_settings();
    if (!settings.model_id.empty()) m.d.model_id = settings.model_id;
    // Set the per-model context window now (before any stream runs) so
    // the ctx % bar uses the right denominator from frame 1, not after
    // the user's first message lands.
    m.s.context_max = ui::context_max_for_model(m.d.model_id.value);
    m.d.profile = settings.profile;
    for (auto& mi : m.d.available_models)
        for (const auto& fav : settings.favorite_models)
            if (mi.id == fav) mi.favorite = true;

    m.d.current.id  = deps().new_thread_id();
    m.s.status = "ready";
    return m;
}

} // namespace moha::app
