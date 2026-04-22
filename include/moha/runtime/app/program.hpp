#pragma once
// moha::app::Program — the maya runtime binding.
//
// Forwards to the per-domain reducer / view / subscribe.  The init function
// reads settings + recent threads through the Store seam.

#include <maya/maya.hpp>

#include "moha/runtime/app/deps.hpp"
#include "moha/runtime/app/subscribe.hpp"
#include "moha/runtime/app/update.hpp"
#include "moha/runtime/model.hpp"
#include "moha/runtime/msg.hpp"
#include "moha/runtime/view/view.hpp"

namespace moha::app {

[[nodiscard]] Model init();

struct MohaApp {
    using Model = ::moha::Model;
    using Msg   = ::moha::Msg;

    static Model init() { return ::moha::app::init(); }

    static auto update(Model m, Msg msg) -> std::pair<Model, maya::Cmd<Msg>> {
        return ::moha::app::update(std::move(m), std::move(msg));
    }

    static maya::Element view(const Model& m) {
        return ::moha::ui::view(m);
    }

    static auto subscribe(const Model& m) -> maya::Sub<Msg> {
        return ::moha::app::subscribe(m);
    }
};

static_assert(maya::Program<MohaApp>);

} // namespace moha::app
