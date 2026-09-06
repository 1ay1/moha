// smart_mode_test — the role resolver (Smart Mode Step 1).
//
// Pure mapping: role + parent model + effort + catalog + config → RoleProfile.
// No I/O, no wire. Verifies zero-config auto-fill, overrides, the off
// pass-through, and the single-tier no-regression guarantee.
#include "agtest.hpp"

#include "agentty/domain/smart_mode.hpp"
#include "agentty/domain/catalog.hpp"   // ModelCapabilities

namespace sm = agentty::smart;
using agentty::Effort;
using agentty::ModelInfo;
using agentty::ModelId;
using agentty::ModelCapabilities;

static ModelInfo mi(const char* id, int ctx = 200000) {
    ModelInfo m;
    m.id = ModelId{id};
    m.context_window = ctx;
    m.supports_tools = true;
    return m;
}

TEST_CASE("smart_mode") {
    // A realistic Claude catalog: Opus (flagship), Sonnet (mid), Haiku (cheap).
    std::vector<ModelInfo> claude = {
        mi("claude-opus-4-20250514"),
        mi("claude-sonnet-4-20250514"),
        mi("claude-haiku-4-20250514"),
    };
    const std::string parent = "claude-opus-4-20250514";

    // 1. Smart Mode OFF → every role is a pass-through to the parent.
    {
        sm::RoleConfig cfg;   // enabled=false
        auto s = sm::resolve_role(sm::ModelRole::Strategic, parent, Effort::High, claude, cfg);
        auto i = sm::resolve_role(sm::ModelRole::Implementation, parent, Effort::High, claude, cfg);
        auto u = sm::resolve_role(sm::ModelRole::Utility, parent, Effort::High, claude, cfg);
        CHECK(s.model == parent, "off: strategic = parent");
        CHECK(i.model == parent, "off: impl = parent");
        CHECK(u.model == parent, "off: utility = parent");
    }

    // 2. Smart Mode ON, zero-config auto-fill.
    {
        sm::RoleConfig cfg; cfg.enabled = true;
        auto s = sm::resolve_role(sm::ModelRole::Strategic, parent, Effort::High, claude, cfg);
        auto i = sm::resolve_role(sm::ModelRole::Implementation, parent, Effort::High, claude, cfg);
        auto u = sm::resolve_role(sm::ModelRole::Utility, parent, Effort::High, claude, cfg);

        CHECK(s.model == parent, "on: strategic = parent (flagship)");
        CHECK(i.model.find("sonnet") != std::string::npos, "on: impl = the mid (sonnet) model");
        CHECK(u.model.find("haiku") != std::string::npos, "on: utility = the cheap (haiku) model");
        CHECK(u.effort == Effort::None, "on: utility runs with NO reasoning budget");
        // Claude 4 Sonnet/Opus don't expose a reasoning-effort control, so the
        // resolver honestly clamps every role's effort to None on this
        // catalog — it never requests an effort a model would 400 on.
        CHECK(s.effort == Effort::None, "on: effort clamps to None for a non-reasoning model (honest)");
    }

    // 2b. Effort stepping on an EFFORT-CAPABLE catalog (o-series / gpt-5.x).
    //     Verifies Strategic keeps the user's effort and Impl steps one down.
    {
        std::vector<ModelInfo> gpt = {
            mi("gpt-5-pro"),      // flagship (effort max)
            mi("gpt-5"),          // mid workhorse
            mi("gpt-5-nano"),     // cheap
        };
        const std::string gparent = "gpt-5-pro";
        sm::RoleConfig cfg; cfg.enabled = true;
        auto s = sm::resolve_role(sm::ModelRole::Strategic, gparent, Effort::High, gpt, cfg);
        auto i = sm::resolve_role(sm::ModelRole::Implementation, gparent, Effort::High, gpt, cfg);
        // Only assert the STEP RELATIONSHIP if the models actually take effort;
        // if the catalog reports no effort support, both clamp to None and the
        // step is vacuously satisfied.
        const bool s_thinks = s.effort != Effort::None;
        if (s_thinks) {
            CHECK(s.effort == Effort::High, "gpt: strategic keeps the user's High effort");
            CHECK(static_cast<int>(i.effort) <= static_cast<int>(s.effort),
                  "gpt: impl effort is <= strategic (stepped down or equal)");
        } else {
            CHECK(i.effort == Effort::None, "gpt: no effort support → impl also None (consistent)");
        }
    }

    // 3. Explicit override wins over auto-fill (model always; effort clamped).
    {
        sm::RoleConfig cfg; cfg.enabled = true;
        cfg.utility.set = true;
        cfg.utility.model = "claude-sonnet-4-20250514";
        cfg.utility.effort = Effort::Low;
        auto u = sm::resolve_role(sm::ModelRole::Utility, parent, Effort::High, claude, cfg);
        CHECK(u.model.find("sonnet") != std::string::npos, "override: utility uses the pinned model");
        // Sonnet 4 takes no effort, so Low clamps to None — the pinned effort
        // is honoured only up to what the pinned model supports.
        CHECK(u.effort == Effort::None, "override: pinned effort clamped to what the model supports");
    }

    // 4. Single-model account → no regression: every role stays on the parent.
    {
        std::vector<ModelInfo> solo = { mi("claude-opus-4-20250514") };
        sm::RoleConfig cfg; cfg.enabled = true;
        auto i = sm::resolve_role(sm::ModelRole::Implementation, parent, Effort::High, solo, cfg);
        auto u = sm::resolve_role(sm::ModelRole::Utility, parent, Effort::High, solo, cfg);
        CHECK(i.model == parent, "solo account: impl stays on parent (no mid tier)");
        CHECK(u.model == parent, "solo account: utility stays on parent (nothing cheaper)");
    }

    // 5. The three behaviour layers gate independently, all under `enabled`.
    {
        sm::RoleConfig cfg;
        // The three layers are now the master switch — there are no per-layer
        // flags to clear. A user cannot run Smart Mode with orchestration off;
        // only the AGENTTY_SMART_NO_* developer escape hatches can do that,
        // and they are env-only by design.
        CHECK(!cfg.internal_routing() && !cfg.orchestration() && !cfg.subagent_routing(),
              "layers: disabled master → all layers inactive");
        cfg.enabled = true;
        CHECK(cfg.internal_routing() && cfg.orchestration() && cfg.subagent_routing(),
              "layers: enabled master → all three active, no sub-toggles");
        cfg.enabled = false;
        CHECK(!cfg.internal_routing() && !cfg.orchestration() && !cfg.subagent_routing(),
              "layers: master off is the single source of truth");

        // The escape hatches suppress exactly one layer each and leave the
        // others running — that is the point of having three of them.
        cfg.enabled = true;
        setenv("AGENTTY_SMART_NO_ORCHESTRATE", "1", 1);
        CHECK(cfg.internal_routing() && !cfg.orchestration() && cfg.subagent_routing(),
              "escape hatch: NO_ORCHESTRATE disables only orchestration");
        unsetenv("AGENTTY_SMART_NO_ORCHESTRATE");
        setenv("AGENTTY_SMART_NO_INTERNAL", "1", 1);
        CHECK(!cfg.internal_routing() && cfg.orchestration() && cfg.subagent_routing(),
              "escape hatch: NO_INTERNAL disables only internal routing");
        unsetenv("AGENTTY_SMART_NO_INTERNAL");
        setenv("AGENTTY_SMART_NO_SUBAGENTS", "1", 1);
        CHECK(cfg.internal_routing() && cfg.orchestration() && !cfg.subagent_routing(),
              "escape hatch: NO_SUBAGENTS disables only subagent routing");
        unsetenv("AGENTTY_SMART_NO_SUBAGENTS");
        CHECK(cfg.internal_routing() && cfg.orchestration() && cfg.subagent_routing(),
              "escape hatches are not sticky");
    }

    // 6. Cascade effort bias: a positive bias steps effort UP, negative DOWN,
    //    clamped to the model; Trivial stays None regardless.
    {
        const auto caps = ModelCapabilities::from_id("gpt-5");   // supports effort
        // Standard @ Medium base, +1 bias → High; -1 bias → Low.
        auto up = sm::effort_for_complexity(Effort::Medium, sm::Complexity::Standard, caps, +1);
        auto dn = sm::effort_for_complexity(Effort::Medium, sm::Complexity::Standard, caps, -1);
        auto mid = sm::effort_for_complexity(Effort::Medium, sm::Complexity::Standard, caps, 0);
        CHECK(static_cast<int>(up) > static_cast<int>(mid), "cascade: +bias raises effort");
        CHECK(static_cast<int>(dn) < static_cast<int>(mid), "cascade: -bias lowers effort");
        // Trivial ignores a positive bias — an ack is an ack.
        auto triv = sm::effort_for_complexity(Effort::High, sm::Complexity::Trivial, caps, +2);
        CHECK(triv == Effort::None, "cascade: trivial stays None despite +bias");
    }

    // 8. CONTINUOUS effort scaling (effort_for_score): a turn DEEP in the
    //    Complex band gets more effort than one barely into it, and the tier
    //    boundary matches the discrete effort_for_complexity exactly.
    {
        const auto caps = ModelCapabilities::from_id("gpt-5");
        // Boundary parity: a shallow-Complex score (margin 0) == discrete path.
        sm::ComplexityScore shallow{sm::Complexity::Complex, 3, 0};
        sm::ComplexityScore deep   {sm::Complexity::Complex, 8, 5};
        auto e_shallow = sm::effort_for_score(Effort::Medium, shallow, caps, 0);
        auto e_deep    = sm::effort_for_score(Effort::Medium, deep,    caps, 0);
        auto e_tier    = sm::effort_for_complexity(Effort::Medium, sm::Complexity::Complex, caps, 0);
        CHECK(e_shallow == e_tier, "scored: shallow-Complex matches the discrete tier step");
        CHECK(static_cast<int>(e_deep) > static_cast<int>(e_shallow),
              "scored: deep-Complex thinks harder than shallow-Complex");
        // Deep-Simple drops further than shallow-Simple.
        sm::ComplexityScore sh_simple{sm::Complexity::Simple, 0, 0};
        sm::ComplexityScore dp_simple{sm::Complexity::Simple, -4, 4};
        auto s_sh = sm::effort_for_score(Effort::High, sh_simple, caps, 0);
        auto s_dp = sm::effort_for_score(Effort::High, dp_simple, caps, 0);
        CHECK(static_cast<int>(s_dp) <= static_cast<int>(s_sh),
              "scored: deep-Simple drops at least as far as shallow-Simple");
        // Trivial still pins to None regardless of score/bias.
        sm::ComplexityScore triv_s{sm::Complexity::Trivial, -100, 100};
        CHECK(sm::effort_for_score(Effort::High, triv_s, caps, +2) == Effort::None,
              "scored: trivial pins to None");
    }

    // 7. resolve_subagent_role: a worker never thinks harder than its parent.
    //    reviewer→Strategic returns the parent model at parent effort; the
    //    subagent wrapper must clamp that to ≤ parent (invariant: subagent
    //    effort ≤ parent effort; parent None ⇒ worker None).
    {
        sm::RoleConfig cfg; cfg.enabled = true;
        const char* parent = "gpt-5";   // supports effort
        std::vector<ModelInfo> g5 = { mi("gpt-5"), mi("gpt-5-mini") };
        // Parent thinking Low: a Strategic-routed reviewer must not exceed Low.
        auto rev = sm::resolve_subagent_role(sm::ModelRole::Strategic, parent,
                                             Effort::Low, g5, cfg);
        CHECK(static_cast<int>(rev.effort) <= static_cast<int>(Effort::Low),
              "subagent: reviewer effort clamped to ≤ parent (Low)");
        // Parent effort OFF ⇒ every worker role is off too.
        for (auto role : {sm::ModelRole::Strategic, sm::ModelRole::Implementation,
                          sm::ModelRole::Utility}) {
            auto p = sm::resolve_subagent_role(role, parent, Effort::None, g5, cfg);
            CHECK(p.effort == Effort::None,
                  "subagent: parent effort None ⇒ worker effort None");
        }
    }

    // ── classify_turn: the ONE composed classifier ──────────────────
    {
        using C = sm::Complexity;
        // Continuation lift: "continue" after Complex work must NOT pin to
        // Trivial (that routed the resumed hard task at Effort::None).
        auto cont = sm::classify_turn("continue", C::Complex, 0, 0);
        CHECK(cont.tier == C::Standard,
              "classify_turn: 'continue' after Complex inherits Standard");
        // …but a terminal ack stays Trivial — an ack is an ack.
        auto ack = sm::classify_turn("thanks", C::Complex, 0, 0);
        CHECK(ack.tier == C::Trivial,
              "classify_turn: 'thanks' after Complex stays Trivial");
        // …and a continuation with NO hard context stays Trivial too.
        auto cold = sm::classify_turn("continue", C::Simple, 0, 0);
        CHECK(cold.tier == C::Trivial,
              "classify_turn: 'continue' after Simple stays Trivial");
        // Correction floor: "no, still broken" never routes below Standard.
        auto corr = sm::classify_turn("no, still broken", C::Simple, 0, 0);
        CHECK(static_cast<int>(corr.tier) >= static_cast<int>(C::Standard),
              "classify_turn: a correction floors at Standard");
        // Payload lift composes: "fix this" + 40KB paste → Complex-ish.
        auto pay = sm::classify_turn("fix this", C::Standard, 40 * 1024, 0);
        CHECK(static_cast<int>(pay.tier) >= static_cast<int>(C::Standard),
              "classify_turn: big payload lifts a chip-text turn");
        // is_continuation_cue: resume commands yes, terminal acks no.
        CHECK(sm::is_continuation_cue("retry"), "cue: retry resumes");
        CHECK(sm::is_continuation_cue("go ahead"), "cue: go ahead resumes");
        CHECK(!sm::is_continuation_cue("thanks"), "cue: thanks is terminal");
        CHECK(!sm::is_continuation_cue("commit"), "cue: commit is terminal");
        CHECK(!sm::is_continuation_cue(
                  "continue refactoring the parser and add tests"),
              "cue: long turns carry their own signal");
    }

    // ── AGENTTY_SMART_MODE session pin ────────────────────────
    {
        auto reset = [] {
            unsetenv("AGENTTY_SMART_MODE");
            unsetenv("AGENTTY_SMART_ENABLED");
        };
        reset();
        CHECK(!sm::tuning::enabled_override().has_value(),
              "env pin: unset → no override (settings govern)");
        setenv("AGENTTY_SMART_MODE", "1", 1);
        CHECK(sm::tuning::enabled_override() == std::optional<bool>{true},
              "env pin: AGENTTY_SMART_MODE=1 → forced on");
        setenv("AGENTTY_SMART_MODE", "0", 1);
        CHECK(sm::tuning::enabled_override() == std::optional<bool>{false},
              "env pin: AGENTTY_SMART_MODE=0 → forced off");
        setenv("AGENTTY_SMART_MODE", "false", 1);
        CHECK(sm::tuning::enabled_override() == std::optional<bool>{false},
              "env pin: 'false' → off");
        setenv("AGENTTY_SMART_MODE", "on", 1);
        CHECK(sm::tuning::enabled_override() == std::optional<bool>{true},
              "env pin: any non-falsy value → on");
        reset();
        // ONE env name. The former AGENTTY_SMART_ENABLED alias is gone: a
        // second spelling is a second source of truth, and "which wins"
        // is a question no user should have to ask.
        setenv("AGENTTY_SMART_ENABLED", "1", 1);
        CHECK(!sm::tuning::enabled_override().has_value(),
              "env pin: AGENTTY_SMART_ENABLED is NOT read (alias removed)");
        reset();
    }

    // ── Numeric routing policy: env over stored, both clamped ───────────
    // These three were env-ONLY, which made them undiscoverable and reset
    // every shell. They are persisted settings now, and the env vars became
    // session OVERRIDES rather than the only way in. Three properties:
    //
    //   1. unset env ⇒ nullopt, so the caller falls through to the stored
    //      value. env_int() would have substituted the default here and
    //      silently overridden whatever the user configured — which is the
    //      whole reason for the _env()/optional split.
    //   2. a set env value wins, clamped to the row's range.
    //   3. a malformed value reads as unset, so a typo in a shell profile
    //      leaves the configured value standing rather than resetting it.
    {
        auto reset = [] {
            unsetenv("AGENTTY_SMART_DEEP_MARGIN");
            unsetenv("AGENTTY_SMART_BIAS_CLAMP");
            unsetenv("AGENTTY_SMART_COMPLEX_THRESHOLD");
        };
        reset();
        CHECK(!sm::tuning::deep_margin_env().has_value(),
              "tuning: unset ⇒ no override, the stored value governs");
        CHECK(!sm::tuning::bias_clamp_env().has_value(),
              "tuning: unset ⇒ no override (bias clamp)");
        CHECK(!sm::tuning::complex_threshold_env().has_value(),
              "tuning: unset ⇒ no override (complex threshold)");

        setenv("AGENTTY_SMART_DEEP_MARGIN", "5", 1);
        CHECK(sm::tuning::deep_margin_env() == std::optional<int>{5},
              "tuning: a set value is read");

        // Out of range is CLAMPED, never rejected-then-forgotten: the config
        // must not be able to hold a value the UI could not produce.
        setenv("AGENTTY_SMART_DEEP_MARGIN", "999", 1);
        CHECK(sm::tuning::deep_margin_env()
                  == std::optional<int>{sm::tuning::kDeepMarginMax},
              "tuning: above range clamps to the max");
        setenv("AGENTTY_SMART_DEEP_MARGIN", "-7", 1);
        CHECK(sm::tuning::deep_margin_env()
                  == std::optional<int>{sm::tuning::kDeepMarginMin},
              "tuning: below range clamps to the min");

        setenv("AGENTTY_SMART_DEEP_MARGIN", "not-a-number", 1);
        CHECK(!sm::tuning::deep_margin_env().has_value(),
              "tuning: a malformed value reads as unset, not as a reset");

        // The bare accessors still work for callers with no config in hand,
        // and agree with the named defaults.
        reset();
        CHECK(sm::tuning::deep_margin()       == sm::tuning::kDeepMarginDefault,
              "tuning: bare accessor is the shipped default when unset");
        CHECK(sm::tuning::bias_clamp()        == sm::tuning::kBiasClampDefault,
              "tuning: bare accessor (bias clamp)");
        CHECK(sm::tuning::complex_threshold() == sm::tuning::kComplexDefault,
              "tuning: bare accessor (complex threshold)");
        reset();
    }

    // ── RoleConfig carries the resolved policy ────────────────────────
    // The domain reads these from config, not from getenv — which is what lets
    // a value set in the settings UI actually route. Defaults must match
    // tuning's, or a fresh Model would route differently from a bare call.
    {
        const sm::RoleConfig fresh;
        CHECK(fresh.deep_margin       == sm::tuning::kDeepMarginDefault,
              "RoleConfig: deep_margin defaults to the shipped value");
        CHECK(fresh.bias_clamp        == sm::tuning::kBiasClampDefault,
              "RoleConfig: bias_clamp defaults to the shipped value");
        CHECK(fresh.complex_threshold == sm::tuning::kComplexDefault,
              "RoleConfig: complex_threshold defaults to the shipped value");
    }

    // ── The tuning parameters actually MOVE the routing decision ─────────
    // A setting that persists, renders and round-trips but changes nothing is
    // worse than no setting at all. Both knobs are asserted on the WIRE path
    // (classify_turn / effort_for_score), which is the pair launch_stream and
    // build_smart_routing_card both call — so "card == wire" now covers the
    // tuning too. A card classified at the shipped default while the wire used
    // the user's threshold would advertise a route the request never took.
    {
        const auto gpt = ModelCapabilities::from_id("gpt-5");
        const char* probe = "update the parser and re-run the formatter";

        // complex_threshold moves the tier boundary. Asserted on
        // classify_score, which is where the cut is APPLIED — classify_turn
        // layers momentum, a continuation lift and a correction floor on top,
        // and for this probe those pin the result to Standard whatever the cut
        // is. That is correct product behaviour (a follow-up should not swing
        // tiers because a threshold moved) and the wrong lens for this
        // property. classify_turn threads the parameter down to the same call,
        // so covering the base covers both.
        const auto at_default = sm::classify_score(probe, sm::tuning::kComplexDefault);
        const auto at_low     = sm::classify_score(probe, /*complex_min=*/1);
        CHECK(at_low.tier == sm::Complexity::Complex,
              "WIRE: a lower cut escalates a borderline turn");
        CHECK(at_default.tier != at_low.tier,
              "WIRE: the threshold parameter is not ignored");
        CHECK(sm::classify_score(probe).tier == at_default.tier,
              "WIRE: the default parameter is the shipped classification");

        // deep_margin decides whether a deep band earns the extra step. A
        // Complex turn with margin 4 is "deep" at margin 3 and not at 8.
        const sm::ComplexityScore deep{sm::Complexity::Complex, 12, 4};
        const Effort with_small = sm::effort_for_score(Effort::Low, deep, gpt, 0,
                                                       /*deep=*/3);
        const Effort with_large = sm::effort_for_score(Effort::Low, deep, gpt, 0,
                                                       /*deep=*/8);
        CHECK(static_cast<int>(with_small) > static_cast<int>(with_large),
              "WIRE: a smaller deep-band margin buys the extra effort step");
    }

    // 10. THE EFFORT LADDER IS PER-MODEL, NOT GLOBAL.
    //
    //     Regression for a silent, severe bug: the complexity scalers stepped
    //     along a hardcoded ladder {None, Low, Medium, High, Xhigh, Max} that
    //     OMITTED `minimal` — gpt-5's bottom reasoning tier. A user on gpt-5 at
    //     minimal effort therefore had every Standard turn resolve to
    //     Effort::None: the feature whose entire job is scaling reasoning was
    //     silently switching reasoning OFF. The mirror bug hit Claude, which
    //     has NO `minimal` rung: a step down from Low landed on Minimal, and
    //     clamp_effort snapped it back UP to Low, so Implementation never
    //     actually stepped down from a Low parent.
    //
    //     The invariant both violate: a step may only ever land on a rung the
    //     model actually accepts, and a non-Trivial turn must never silently
    //     extinguish an effort the user explicitly asked for.
    {
        const auto gpt = ModelCapabilities::from_id("gpt-5");
        // o3 has a REAL ladder that lacks `minimal` (low·medium·high) — the
        // shape that exposed the phantom-rung half of the bug. Claude 4 is the
        // third shape: no effort ladder at all (off only).
        const auto o3      = ModelCapabilities::from_id("o3");
        const auto claude4 = ModelCapabilities::from_id("claude-sonnet-4-20250514");

        // Premises. If these stop holding the rest of the case is vacuous.
        REQUIRE(agentty::effort_set_of(gpt) & agentty::effort_bit(Effort::Minimal));
        REQUIRE(agentty::effort_set_of(o3) & agentty::effort_bit(Effort::Low));
        REQUIRE(!(agentty::effort_set_of(o3) & agentty::effort_bit(Effort::Minimal)));
        REQUIRE(agentty::effort_set_of(claude4) == 0);

        // (a) A Standard turn is a NO-OP on effort, at every rung including
        //     the bottom one. This is the bug in its purest form, and it must
        //     be asserted on effort_for_score — THE WIRE PATH. launch_stream
        //     and build_smart_routing_card both call that one, and unlike
        //     effort_for_complexity (which short-circuits Standard to `base`)
        //     it unconditionally evaluates step(base, tier_step + extra). With
        //     the old fixed ladder, step(minimal, 0) fell off the ladder to
        //     index 0 == None: a gpt-5 user who picked `minimal` had reasoning
        //     SILENTLY DISABLED on every ordinary turn.
        for (Effort base : {Effort::Minimal, Effort::Low, Effort::Medium,
                            Effort::High}) {
            const sm::ComplexityScore std_turn{sm::Complexity::Standard, 0, 0};
            CHECK(sm::effort_for_score(base, std_turn, gpt, 0) == base,
                  "WIRE: standard turn leaves effort untouched on every rung");
            CHECK(sm::effort_for_complexity(base, sm::Complexity::Standard, gpt, 0) == base,
                  "standard turn leaves effort untouched on every rung");
        }

        // (a2) The same on the deep-band path: a Standard turn with a large
        //      margin still resolves to exactly `base`.
        {
            const sm::ComplexityScore deep_std{sm::Complexity::Standard, 0, 9};
            CHECK(sm::effort_for_score(Effort::Minimal, deep_std, gpt, 0) == Effort::Minimal,
                  "WIRE: a deep Standard turn still does not move effort");
        }

        // (b) A Simple turn steps DOWN by exactly one rung of the model's own
        //     ladder. `None` (off) IS a legitimate bottom rung — the user can
        //     select it in the picker — so minimal→off is a correct step, not
        //     the bug. The bug was Standard doing this silently, covered above.
        CHECK(sm::effort_for_complexity(Effort::Minimal, sm::Complexity::Simple, gpt, 0)
                  == Effort::None,
              "simple turn steps down exactly one rung (minimal → off)");

        // (b2) A Complex turn on gpt-5 from `minimal` reaches `low`, not a
        //      skipped rung — the ladder's bottom is walked, not jumped.
        {
            const sm::ComplexityScore cx_turn{sm::Complexity::Complex, 0, 0};
            CHECK(sm::effort_for_score(Effort::Minimal, cx_turn, gpt, 0) == Effort::Low,
                  "WIRE: complex turn steps minimal → low");
        }

        // (c) Stepping walks the MODEL's rungs, so the same request resolves
        //     differently per provider — which is the point. On gpt-5 one step
        //     down from Low is `minimal`; on o3, which lacks that rung, Low
        //     steps straight to off. Before the fix o3's step down from Low
        //     produced the phantom Minimal, which clamp_effort snapped back UP
        //     to Low — so Implementation never stepped down from a Low parent.
        CHECK(sm::detail::effort_step_down(Effort::Low, gpt) == Effort::Minimal,
              "gpt-5: one step down from low is minimal");
        CHECK(sm::detail::effort_step_down(Effort::Low, o3) == Effort::None,
              "o3: low steps to off, not to a minimal rung it does not have");
        CHECK(static_cast<int>(sm::detail::effort_step_down(Effort::Low, o3))
                  < static_cast<int>(Effort::Low),
              "o3: a step down from low actually descends (no phantom rung)");
        CHECK(sm::detail::effort_step_down(Effort::Medium, o3) == Effort::Low,
              "o3: medium steps down to low, its true adjacent rung");

        // (d) A model with NO effort ladder always resolves to off, at every
        //     tier and bias — Smart Mode must never invent a reasoning level
        //     for a model that would reject the field.
        for (auto tier : {sm::Complexity::Trivial, sm::Complexity::Simple,
                          sm::Complexity::Standard, sm::Complexity::Complex})
            for (int b : {-2, 0, +2})
                CHECK(sm::effort_for_complexity(Effort::High, tier, claude4, b)
                          == Effort::None,
                      "a model without an effort ladder is always off");

        // (e) Every reachable step lands on a level the model ACCEPTS. A
        //     level outside effort_set_of would 400 (or be silently
        //     rewritten) at the provider.
        for (Effort base : {Effort::None, Effort::Minimal, Effort::Low,
                            Effort::Medium, Effort::High, Effort::Xhigh,
                            Effort::Max})
            for (int n = -3; n <= 3; ++n)
                for (const auto& caps : {gpt, o3, claude4}) {
                    const Effort got = sm::detail::effort_step(base, n, caps);
                    CHECK((got == Effort::None
                           || (agentty::effort_set_of(caps) & agentty::effort_bit(got))),
                          "a step never lands on a rung the model lacks");
                }

        // (f) Monotonic: stepping up never lowers effort, down never raises.
        for (Effort base : {Effort::Minimal, Effort::Low, Effort::Medium, Effort::High}) {
            CHECK(static_cast<int>(sm::detail::effort_step(base, +1, gpt))
                      >= static_cast<int>(base), "step up never lowers");
            CHECK(static_cast<int>(sm::detail::effort_step(base, -1, gpt))
                      <= static_cast<int>(base), "step down never raises");
        }
    }
}
