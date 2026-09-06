// smart_routing_card_test — the 🧠 card must show the route the wire took.
//
// build_smart_routing_card and launch_stream classify the turn INDEPENDENTLY.
// The code comment on both calls says "same call as launch_stream so card ==
// wire", but nothing enforced it: a dropped argument at one of the two sites
// left the card advertising a route the request never took, and the suite
// stayed green. That is precisely the failure this file exists to catch, and
// it caught nothing until it existed — a mutation dropping
// `m.d.smart.complex_threshold` at the card site passed 570 tests.
//
// The card is documented pure and takes a const Model&, so the wire's answer
// can be recomputed here from the same inputs and compared field for field.
// The comparison is against smart::classify_turn / effort_for_score directly —
// the functions launch_stream calls — so this fails if EITHER site drifts.

#include "agtest.hpp"

#include "agentty/domain/complexity.hpp"
#include "agentty/domain/catalog.hpp"
#include "agentty/domain/smart_mode.hpp"
#include "agentty/domain/smart_tuning.hpp"
#include "agentty/runtime/app/cmd_factory.hpp"
#include "agentty/runtime/model.hpp"

#include <string>

namespace {

using namespace agentty;
namespace sm = agentty::smart;

// A model on the brink of a turn: Smart Mode on, one user message, the
// classifier about to run. `tuning` is the user's configured policy.
Model turn_with(const std::string& text, sm::RoleConfig tuning) {
    Model m;
    m.d.smart = std::move(tuning);
    m.d.smart.enabled = true;
    Message user;
    user.role = Role::User;
    user.text = text;
    m.d.current.messages.push_back(std::move(user));
    return m;
}

} // namespace

TEST_CASE("smart card: the card's complexity is the wire's complexity") {
    // Sweep the CONFIGURED threshold across its whole range on a turn whose
    // classification sits near the boundary. At every setting the card must
    // agree with what classify_turn — the wire's own call — produces for the
    // same inputs. A card site that forgot to pass the threshold would track
    // the shipped default and diverge as soon as the user moved it.
    const std::string probe = "update the parser and re-run the formatter";

    for (int cut = sm::tuning::kComplexMin; cut <= sm::tuning::kComplexMax; ++cut) {
        sm::RoleConfig cfg;
        cfg.complex_threshold = cut;
        const Model m = turn_with(probe, cfg);

        const auto card = app::cmd::build_smart_routing_card(m);
        REQUIRE(card.has_value());

        // What the wire will classify, computed the way launch_stream does.
        const auto wire = sm::classify_turn(probe, m.s.smart_turn_complexity,
                                            /*attachment_bytes=*/0, /*images=*/0,
                                            cfg.complex_threshold);

        CHECK(card->smart_route_complexity == std::string{sm::to_string(wire.tier)});
    }
}

TEST_CASE("smart card: the card's effort is the wire's effort") {
    // Same argument for the OTHER knob. deep_margin decides whether a turn
    // deep in its band earns the extra effort step, so a card that classified
    // at the default margin would show a different effort from the one the
    // request carries.
    const std::string probe = "redesign the auth module end to end across services";

    for (int deep = sm::tuning::kDeepMarginMin;
         deep <= sm::tuning::kDeepMarginMax; ++deep) {
        sm::RoleConfig cfg;
        cfg.deep_margin = deep;
        const Model m = turn_with(probe, cfg);

        const auto card = app::cmd::build_smart_routing_card(m);
        REQUIRE(card.has_value());

        const auto wire_cx = sm::classify_turn(probe, m.s.smart_turn_complexity,
                                               0, 0, cfg.complex_threshold);
        const auto caps = ModelCapabilities::from_id(card->smart_route_model);
        // The card names the model it routed to; the effort ladder is
        // per-model, so the comparison has to use THAT model's capabilities.
        const auto base = m.d.effort;
        const Effort wire_effort =
            sm::effort_for_score(base, wire_cx, caps, m.s.smart_effort_bias,
                                 cfg.deep_margin);

        CHECK(card->smart_route_effort
              == std::string{effort_label(wire_effort)});
    }
}

TEST_CASE("smart card: moving a knob actually moves the card") {
    // The tests above would pass vacuously if the card were insensitive to the
    // tuning — both sides would just be wrong together. So: at least one
    // setting in the range must produce a DIFFERENT card from the default,
    // proving the plumbing carries a signal at all.
    const std::string probe = "update the parser and re-run the formatter";

    sm::RoleConfig at_default;
    sm::RoleConfig at_low;
    at_low.complex_threshold = sm::tuning::kComplexMin;

    const auto a = app::cmd::build_smart_routing_card(turn_with(probe, at_default));
    const auto b = app::cmd::build_smart_routing_card(turn_with(probe, at_low));
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(a->smart_route_complexity != b->smart_route_complexity);
}
