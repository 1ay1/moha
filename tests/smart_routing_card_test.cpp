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

TEST_CASE("smart card: an inserted proactive block cannot change the route") {
    // The load-bearing assumption behind computing the routing decision ONCE
    // per turn instead of twice.
    //
    // On a proactive turn the launch is DEFERRED: submit_turn builds the card,
    // then retrieval runs, a context message is spliced into the transcript,
    // and ProactiveContextReady fires launch_stream from a different handler.
    // If that insertion could move the classification, a decision computed at
    // submit and read at launch would be stale — and the whole reason the two
    // sites re-derive independently today is that re-deriving is trivially
    // correct under transcript mutation.
    //
    // It cannot move it: both scans walk backward for the newest message that
    // is `role == User && !is_proactive_context() && !smart_routing &&
    // !fork_note`, and the inserted block is exactly a proactive-context user
    // message. This asserts that rather than trusting the reading.
    const std::string probe = "redesign the auth module end to end across services";

    // Model is move-only (it owns a ViewCache and a ScrollbackLedger), so each
    // variant is built rather than copied.
    const auto route_of = [&](bool with_proactive, bool with_card) {
        Model m = turn_with(probe, sm::RoleConfig{});
        if (with_proactive) {
            // Exactly what ProactiveContextReady splices in: a retrieved-
            // context user message carrying a large, Complex-looking payload.
            // If the scan were not skipping it, THIS is what would classify.
            Message ctx;
            ctx.role = Role::User;
            ctx.proactive =
                Message::ProactiveContext{.confidence = 0.9, .expanded = false};
            ctx.text = "retrieved context: " + std::string(4096, 'x');
            m.d.current.messages.push_back(std::move(ctx));
        }
        if (with_card) {
            // The 🧠 card itself, the other thing that lands between submit and
            // launch. Zero-text and Role::User — classifying it would produce
            // an empty string.
            Message card;
            card.role = Role::User;
            card.smart_routing = true;
            m.d.current.messages.push_back(std::move(card));
        }
        auto c = app::cmd::build_smart_routing_card(m);
        REQUIRE(c.has_value());
        return std::tuple{c->smart_route_complexity, c->smart_route_effort,
                          c->smart_route_model};
    };

    const auto plain     = route_of(false, false);
    const auto proactive = route_of(true,  false);
    const auto both      = route_of(true,  true);

    CHECK(proactive == plain);
    CHECK(both == plain);
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
