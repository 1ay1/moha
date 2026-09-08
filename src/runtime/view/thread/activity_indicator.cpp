#include "agentty/runtime/view/thread/activity_indicator.hpp"

#include <vector>

#include "agentty/runtime/view/thread/activity_indicator_words.hpp"

namespace agentty::ui {

// NOTE: this TU used to also define activity_indicator_config(const Model&),
// a bottom-of-thread indicator builder. Nothing called it — the indicator's
// only live site is the in-Turn placeholder in conversation.cpp, which
// builds its Config inline (it needs run-local state: the tail message's
// stream bytes, the run's rail colour). Removed rather than left as a
// parallel half-implementation; if a second surface ever wants the row,
// build the Config where the run context lives, like conversation.cpp does.

const std::vector<std::string_view>& activity_indicator_words() {
    static const std::vector<std::string_view> pool = [] {
        std::vector<std::string_view> v;
        v.reserve(indicator_words::kPool.size());
        for (auto sv : indicator_words::kPool) v.push_back(sv);
        return v;
    }();
    return pool;
}

} // namespace agentty::ui
