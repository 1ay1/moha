#include "moha/runtime/app/program.hpp"

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
    m.threads          = deps().load_threads();
    m.available_models = seed_models();

    auto settings = deps().load_settings();
    if (!settings.model_id.empty()) m.model_id = settings.model_id;
    m.profile = settings.profile;
    for (auto& mi : m.available_models)
        for (const auto& fav : settings.favorite_models)
            if (mi.id == fav) mi.favorite = true;

    m.current.id  = deps().new_thread_id();
    m.stream.status = "ready";
    return m;
}

} // namespace moha::app
