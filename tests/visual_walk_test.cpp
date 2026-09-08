// visual_walk_test — the structural frame-hash walk's contract.
//
// Three properties, each the negation of a shipped bug class:
//   1. COMPLETENESS: mutating ANY visible facet of ANY panel state changes
//      the hash — nothing to remember, nothing to forget.
//   2. SECRETS: credential bytes never influence the hash (same-length
//      overwrite of a Secret / api_key is hash-invisible; length change is
//      visible). The exemption is deliberate and pinned.
//   3. ARITY PROOF: adding a member to a type with a visual_parts list
//      breaks the build (compile-time; demonstrated by the static_asserts
//      in visual_parts.hpp — this test pins the RUNTIME halves).

#include "agtest.hpp"

#include "agentty/runtime/panel/visual_parts.hpp"

#include <string>

using namespace agentty;
namespace pn = agentty::ui::panel;

namespace {

std::uint64_t walk(const auto& v) {
    std::uint64_t H = 0;
    auto h = [&](std::uint64_t x) {
        H ^= x + 0x9e3779b97f4a7c15ull + (H << 6) + (H >> 2);
    };
    visual::mix_any(h, v);
    return H;
}

} // namespace

TEST_CASE("visual walk: every panel facet changes the hash") {
    // Models: base members (index/query/confirm_remove/provider_scope),
    // the assign_slot optional — each mutation must move the hash.
    pn::Models a;
    const auto h0 = walk(a);
    { auto b = a; b.index = 3;                CHECK(walk(b) != h0); }
    { auto b = a; b.query = "x";              CHECK(walk(b) != h0); }
    { auto b = a; b.confirm_remove = "y";     CHECK(walk(b) != h0); }
    { auto b = a; b.provider_scope = "z";     CHECK(walk(b) != h0); }
    { auto b = a; b.assign_slot = smart::ModelRole::Utility;
                                              CHECK(walk(b) != h0); }

    // The slot variant: switching alternatives moves the hash even when
    // both alternatives are default-constructed.
    ui::panel::State s1; s1 = pn::Models{};
    ui::panel::State s2; s2 = pn::Providers{};
    CHECK(walk(s1.raw()) != walk(s2.raw()));
}

TEST_CASE("visual walk: form facets all covered (the mix_form bug class)") {
    form::Form f;
    f.fields.push_back({.id = "host", .label = "Host",
                        .value = form::field::Text{"localhost", 4}});
    const auto h0 = walk(f);
    { auto g = f; g.cursor = 0; std::get<form::field::Text>(
          g.fields[0].value).value += "x";    CHECK(walk(g) != h0); }
    { auto g = f; std::get<form::field::Text>(
          g.fields[0].value).cursor = 9;      CHECK(walk(g) != h0); }
    { auto g = f; g.dirty = true;             CHECK(walk(g) != h0); }
    { auto g = f; g.note = "unsaved";         CHECK(walk(g) != h0); }
    { auto g = f; g.focus = form::focus::Editing{};
                                              CHECK(walk(g) != h0); }
    { auto g = f; g.fields[0].error = "bad";  CHECK(walk(g) != h0); }
    { auto g = f; g.fields[0].locked = true;  CHECK(walk(g) != h0); }
}

TEST_CASE("visual walk: secret bytes never reach the hash") {
    // field::Secret: same-length overwrite invisible; length change visible;
    // cursor motion visible.
    form::Form f;
    f.fields.push_back({.id = "key", .label = "API key",
                        .value = form::field::Secret{"hunter22", 3}});
    const auto h0 = walk(f);
    { auto g = f; std::get<form::field::Secret>(
          g.fields[0].value).value = "letmein9";   // same length
      CHECK(walk(g) == h0); }
    { auto g = f; std::get<form::field::Secret>(
          g.fields[0].value).value = "longer-secret";
      CHECK(walk(g) != h0); }
    { auto g = f; std::get<form::field::Secret>(
          g.fields[0].value).cursor = 7;
      CHECK(walk(g) != h0); }

    // EmbedConfig::api_key: same rule at the config layer.
    rag::embed::EmbedConfig c;
    c.api_key = "sk-aaaaaaaa";
    const auto e0 = walk(c);
    { auto d = c; d.api_key = "sk-bbbbbbbb";       // same length
      CHECK(walk(d) == e0); }
    { auto d = c; d.api_key = "sk-longer-than-before";
      CHECK(walk(d) != e0); }
    { auto d = c; d.host = "other-host";           // non-secret: visible
      CHECK(walk(d) != e0); }
}

TEST_CASE("visual walk: rag pane probe + form ride in structurally") {
    pn::Rag r;
    const auto h0 = walk(r);
    { auto q = r; q.embed.probe = rag_settings::EmbedForm::Testing{};
      CHECK(walk(q) != h0); }
    { auto q = r; q.embed.probe = rag_settings::EmbedForm::Ok{768, 42};
      CHECK(walk(q) != h0); }
    { auto q = r; q.advanced = true;          CHECK(walk(q) != h0); }
    { auto q = r; q.embed.form.dirty = true;  CHECK(walk(q) != h0); }
}

TEST_CASE("visual walk: exempt facets are exempt (From does not churn)") {
    // Opening a child over a parent stashes the parent in `from`. The
    // parent is not rendered while the child is open — the walk must NOT
    // differ for different stashed parents (else every keystroke would
    // re-walk the ancestry, and restoring would repaint spuriously).
    pn::SettingsList a, b;
    ui::panel::State sa, sb;
    sa = pn::Palette{};   // parent 1
    sb = pn::Models{};    // parent 2
    a.from = pn::From::of(pn::Snapshot{sa.raw()});
    b.from = pn::From::of(pn::Snapshot{sb.raw()});
    CHECK(walk(a) == walk(b));
}
