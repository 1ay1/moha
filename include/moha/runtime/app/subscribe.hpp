#pragma once
// moha::app::subscribe — input → Msg routing.
//
// Pure function of Model: snapshots which modal (if any) owns the keyboard,
// then routes keys / paste / tick into the right Msg.

#include <maya/maya.hpp>

#include "moha/runtime/model.hpp"
#include "moha/runtime/msg.hpp"

namespace moha::app {

[[nodiscard]] maya::Sub<Msg> subscribe(const Model& m);

} // namespace moha::app
