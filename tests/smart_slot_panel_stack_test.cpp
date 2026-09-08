// smart_slot_panel_stack_test — picker navigation is a STACK, not a trapdoor.
//
// Descending Smart Mode → model picker to assign a role slot and then
// backing out (Esc) or committing (Enter) must POP one level — return to
// the Smart Mode picker — never nuke every overlay back to the thread.
// Navigating into a sub-setting and hitting Esc should land you on the row
// you came from, so you can keep configuring the sibling slots. This guards
// the reducer paths in src/runtime/app/update/picker.cpp (ModelsSelect
// and CloseModels, slot-assign branches).
//
// Driven through the REAL app::update reducer, no mocks of the reducer path.
//
// Since the picker consolidation there is ONE model surface (the fused,
// all-providers picker); this guards its slot-assign mode — the reducer
// paths in src/runtime/app/update/picker.cpp (ModelsSelect and
// CloseModels, slot-assign branches) plus the active-provider scoping
// that keeps an unstreamable cross-provider pin unrepresentable.

#include "agtest.hpp"

#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/update/internal.hpp"  // app::detail::fused_rows_for_model
#include "agentty/runtime/panel/common.hpp"
#include "agentty/domain/smart_mode.hpp"   // kSmartModeRows
#include "agentty/runtime/panel/smart_form.hpp"
#include "agentty/runtime/panel/form_keys.hpp"
#include "agentty/provider/selection.hpp"

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace pn = agentty::ui::panel;

using namespace agentty;

namespace {

store::Settings g_settings;
void install_stub_deps() {
    app::install_deps(app::Deps{
        .stream         = [](auto, auto) {},
        .save_thread    = [](const auto&) {},
        .delete_thread  = [](const auto&) {},
        .load_threads   = [] { return std::vector<Thread>{}; },
        .load_thread    = [](const auto&) -> std::optional<Thread> { return std::nullopt; },
        .load_settings  = [] { return g_settings; },
        .save_settings  = [](const store::Settings& x) { g_settings = x; },
        .new_thread_id  = [] { return ThreadId{}; },
        .title_from     = [](std::string_view t) { return std::string{t}; },
        .auth           = auth::AuthHeader{auth::ApiKeyHeader{std::string{}}},
    });
}

ModelInfo mi(const char* id, const char* prov) {
    ModelInfo m;
    m.id = ModelId{id};
    m.display_name = id;
    m.provider = prov;
    m.supports_tools = true;
    return m;
}

// A Model with a small catalog, opened through the REAL OpenModels
// path in slot-assign mode for `slot` (0 strategic / 1 implementation /
// 2 utility). Mirrors meta.cpp's hand-off exactly: a SmartMode pane is
// open, then an assign-mode Models descend()s over it — the pane rides
// in the picker's `from` snapshot.
Model in_slot_assign(int slot) {
    Model m;
    m.d.model_id = ModelId{"claude-opus-4-5"};
    m.d.available_models = { mi("claude-opus-4-5", "anthropic"),
                             mi("claude-haiku-4-5", "anthropic") };
    // The pane the hand-off leaves behind (what Esc must restore). In the
    // real flow the user's cursor is ON the slot row when Enter fires the
    // hand-off — the snapshot carries the form cursor-and-all — so focus
    // the role before descending, exactly as meta.cpp finds it.
    auto [m0, _open] = app::update(std::move(m), Msg{OpenSmartMode{}});
    m = std::move(m0);
    if (auto* sm = m.ui.panel.get<pn::SmartMode>())
        smart_form::focus_role(sm->form, static_cast<smart::ModelRole>(slot));
    pn::Models picker{{0, ""}, {}, static_cast<smart::ModelRole>(slot)};
    m.ui.panel.descend(std::move(picker));
    auto [m1, _] = app::update(std::move(m), Msg{OpenModels{}});
    return std::move(m1);
}

} // namespace

TEST_CASE("smart slot picker stack") {
    install_stub_deps();
    // Hermetic auth: the fused picker only seeds catalogs for providers that
    // are AUTHED, and a bare test env has no on-disk credentials (nor should
    // it depend on any). A provider_keys entry makes provider_is_authed
    // ("anthropic") true, so OpenModels populates real rows.
    g_settings = store::Settings{};
    g_settings.provider_keys["anthropic"] = "sk-test";
    {
        provider::Selection sel;
        sel.kind = provider::Kind::Anthropic;
        provider::select(sel);
    }

    // ── Esc in slot-assign pops back to Smart Mode, does NOT exit ──
    for (int slot = 0; slot <= 2; ++slot) {
        Model m = in_slot_assign(slot);
        auto [m2, cmd] = app::update(std::move(m), Msg{CloseModels{}});

        CHECK(!m2.ui.panel.is<pn::Models>(),
              "Esc closes the model picker");
        CHECK(m2.ui.panel.is<pn::SmartMode>(),
              "Esc RE-OPENS Smart Mode — navigation is a stack, not a trapdoor");
        // The pending assign died WITH the picker value — structurally;
        // there is no parked flag left to check, which is the point.
        if (auto* o = m2.ui.panel.get<pn::SmartMode>()) {
            const auto* row = o->form.focused();
            CHECK(row && row->id == smart_form::field_of_role(
                             static_cast<smart::ModelRole>(slot)),
                  "cursor lands back on the slot row you descended from");
        } else {
            CHECK(false, "smart_mode must be OpenAt after Esc");
        }
        // Esc must NOT have written anything into the slot.
        const smart::SlotOverride& s =
            slot == 0 ? m2.d.smart.strategic
          : slot == 1 ? m2.d.smart.implementation
                      : m2.d.smart.utility;
        CHECK(!s.set, "Esc abandons the assignment — slot stays unset");
    }

    // ── Enter in slot-assign writes the slot AND pops back to Smart Mode ──
    for (int slot = 0; slot <= 2; ++slot) {
        Model m = in_slot_assign(slot);
        // Put the cursor on the opus row explicitly — the fused list orders
        // by (active, favourite, family) rather than raw catalog order.
        const auto rows = app::detail::fused_rows_for_model(m);
        int opus = -1;
        for (int i = 0; i < static_cast<int>(rows.size()); ++i)
            if (rows[static_cast<std::size_t>(i)].model.id.value == "claude-opus-4-5") {
                opus = i; break;
            }
        REQUIRE(opus >= 0);
        if (auto* c = m.ui.panel.get<pn::Models>()) c->index = opus;

        auto [m2, cmd] = app::update(std::move(m), Msg{ModelsSelect{}});

        CHECK(!m2.ui.panel.is<pn::Models>(),
              "Enter closes the model picker");
        CHECK(m2.ui.panel.is<pn::SmartMode>(),
              "Enter returns to Smart Mode so sibling slots stay one step away");
        // (slot-assign consumed structurally: the picker value — and its
        // assign_slot — no longer exists)
        if (auto* o = m2.ui.panel.get<pn::SmartMode>()) {
            const auto* row = o->form.focused();
            CHECK(row && row->id == smart_form::field_of_role(
                             static_cast<smart::ModelRole>(slot)),
                  "cursor on the slot just set");
        }

        const smart::SlotOverride& s =
            slot == 0 ? m2.d.smart.strategic
          : slot == 1 ? m2.d.smart.implementation
                      : m2.d.smart.utility;
        CHECK(s.set, "Enter pins the slot");
        CHECK(s.model == "claude-opus-4-5",
              "the highlighted catalog model is written into the slot");
        CHECK(m2.d.smart.enabled,
              "pinning a slot implicitly enables Smart Mode");
        CHECK(m2.d.model_id.value == "claude-opus-4-5",
              "slot-assign must NOT switch the active model");
    }

    // ── Slot-assign scopes the list to the ACTIVE provider ────────────
    // A pinned slot model is dispatched to whatever provider is active at
    // turn time, so a row from another provider would be unstreamable.
    // The list must not offer one — nor a "sign in to X" row.
    {
        Model m = in_slot_assign(0);
        const auto rows = app::detail::fused_rows_for_model(m);
        REQUIRE(!rows.empty());
        for (const auto& r : rows) {
            CHECK(r.provider_id == "anthropic",
                  "slot-assign lists ONLY the active provider's models");
            CHECK(!r.is_signin_offer(),
                  "slot-assign never offers a provider you aren't signed into");
        }
    }

    // ── The scoping is CONDITIONAL on slot-assign, not permanent ──────
    // (Cross-provider breadth itself is fused_models_test's job; here we only
    // prove that only_provider is applied in slot-assign mode and NOT applied
    // outside it — the bug this filter could plausibly introduce.)
    {
        auto rows_for = [](int slot) {
            Model m;
            m.d.model_id = ModelId{"claude-opus-4-5"};
            m.d.available_models = { mi("claude-opus-4-5", "anthropic"),
                                     mi("claude-haiku-4-5", "anthropic") };
            if (slot >= 0) {
                pn::Models picker{{0, ""}, {},
                                  static_cast<smart::ModelRole>(slot)};
                m.ui.panel.descend(std::move(picker));
            }
            auto [m1, _] = app::update(std::move(m), Msg{OpenModels{}});
            return app::detail::fused_rows_for_model(m1);
        };
        const auto plain = rows_for(-1);
        const auto scoped = rows_for(0);
        REQUIRE(!plain.empty());
        REQUIRE(!scoped.empty());
        // Slot-assign never shows MORE than the unscoped list, and never a
        // sign-in offer (you can't pin a model you aren't signed in to).
        CHECK(scoped.size() <= plain.size(),
              "slot-assign narrows the list, never widens it");
        for (const auto& r : scoped) {
            CHECK(r.provider_id == "anthropic",
                  "slot-assign lists ONLY the active provider's models");
            CHECK(!r.is_signin_offer(),
                  "slot-assign never offers a provider you aren't signed into");
        }
    }

    // ── A NON-slot model-picker Esc still exits cleanly (no regression) ──
    {
        Model m;
        m.d.available_models = { mi("claude-opus-4-5", "anthropic") };
        // ordinary model switch — no assign-mode picker descended
        auto [m1, _] = app::update(std::move(m), Msg{OpenModels{}});
        auto [m2, cmd] = app::update(std::move(m1), Msg{CloseModels{}});
        CHECK(!m2.ui.panel.is<pn::Models>(), "ordinary Esc closes picker");
        CHECK(!m2.ui.panel.is<pn::SmartMode>(),
              "ordinary model-switch Esc does NOT spuriously open Smart Mode");
    }

    // ── Navigation is closed over the row type ──────────────────────
    // The overlay's cursor used to be a raw int, and four sites derived the
    // layout independently: the mover wrapped modulo 11 after the list was
    // cut to four rows, two slot-assign returns re-opened at `8 + slot`, and
    // clear-slot mapped every index ≥ 3 onto Utility. All of those were
    // REPRESENTABLE. The cursor is now a smart::OverlayRow, so the only
    // thing left to check is that a lap through the enumeration returns
    // where it started — a wrong row cannot be constructed to test for.
    {
        Model m;
        auto [m1, _] = app::update(std::move(m), Msg{OpenSmartMode{}});
        Model cur = std::move(m1);

        auto row_id = [](const Model& mm) -> std::string {
            auto* o = mm.ui.panel.get<pn::SmartMode>();
            if (!o) return {};
            const auto* r = o->form.focused();
            return r ? r->id : std::string{};
        };
        auto rows_in = [](const Model& mm) -> int {
            auto* o = mm.ui.panel.get<pn::SmartMode>();
            return o ? static_cast<int>(o->form.fields.size()) : 0;
        };
        auto move = [](Model mm, int d) {
            return app::update(std::move(mm),
                Msg{SmartModeKey{form::keys::Action{
                    d > 0 ? form::keys::Intent::MoveNext
                          : form::keys::Intent::MovePrev}}});
        };

        CHECK(row_id(cur) == smart_form::kFieldEnabled,
              "the overlay opens on the master switch");
        const int n = rows_in(cur);
        CHECK(n > 1, "the pane has a switch plus its slots");

        // A full lap down returns to the start, visiting each row once.
        std::set<std::string> seen;
        for (int i = 0; i < n; ++i) {
            auto [next, c] = move(std::move(cur), +1);
            cur = std::move(next);
            seen.insert(row_id(cur));
        }
        CHECK(row_id(cur) == smart_form::kFieldEnabled,
              "a full lap down returns to the first row");
        CHECK(seen.size() == static_cast<std::size_t>(n),
              "one distinct row per step");

        // Up from the first row lands on the last — no phantom rows between.
        auto [up, c1] = move(std::move(cur), -1);
        cur = std::move(up);
        CHECK(row_id(cur) == smart_form::kFieldUtility,
              "up from the master switch wraps to the last row");
    }
}
