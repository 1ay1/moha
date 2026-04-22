#pragma once
// moha::app::update — pure (Model, Msg) -> (Model, Cmd<Msg>) reducer.
//
// All side effects are returned as Cmds, never executed inline.  The body is
// a single std::visit with one overload per Msg variant, grouped by domain.

#include <utility>

#include <maya/maya.hpp>

#include "moha/runtime/model.hpp"
#include "moha/runtime/msg.hpp"

namespace moha::app {

[[nodiscard]] std::pair<Model, maya::Cmd<Msg>> update(Model m, Msg msg);

} // namespace moha::app
