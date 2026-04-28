#pragma once
#include <maya/widget/thread.hpp>
#include "moha/runtime/model.hpp"

namespace moha::ui {

// Build the Thread widget config from Model. Pure data extraction —
// the widget owns all rendering chrome (welcome screen, conversation
// layout, in-flight indicator, per-turn rail).
[[nodiscard]] maya::Thread::Config thread_config(const Model& m);

} // namespace moha::ui
