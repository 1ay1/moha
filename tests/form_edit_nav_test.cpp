// form_edit_nav_test — arrows NAVIGATE out of an editing field.
//
// Regression for the "panel froze" report: Enter on a text-like row (Model,
// Host, Port…) starts editing, and the editing key table used to swallow
// ↑/↓ — the only exits were Enter/Esc, which nothing on screen advertised.
// Drives the REAL reducers (update) and the real view (render), with a
// watchdog so any genuine hang fails with exit 42 instead of wedging ctest.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

#include <maya/app/inline.hpp>
#include <maya/core/render_context.hpp>
#include <maya/element/builder.hpp>

#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/panel/form_keys.hpp"
#include "agentty/runtime/panel/smart_form.hpp"
#include "agentty/runtime/view/view.hpp"
#include "agentty/runtime/panel/rag.hpp"
#include "agentty/store/store.hpp"

using namespace agentty;
namespace pn = agentty::ui::panel;

static agentty::store::Settings g_settings;

static Model step(Model m, Msg msg, const char* what) {
    std::fprintf(stderr, "[step] %s\n", what);
    auto st = app::update(std::move(m), std::move(msg));
    return std::move(st.first);
}

static void render_once(const Model& m, const char* what) {
    std::fprintf(stderr, "[render] %s\n", what);
    maya::RenderContext ctx{100, 30, maya::render_generation(), false};
    maya::RenderContextGuard g(ctx);
    auto el = ui::view(m);
    (void)maya::render_to_string(el, 100);
    std::fprintf(stderr, "[render-ok] %s\n", what);
}

int main() {
    alarm(10);   // watchdog: any hang -> SIGALRM kills us, exit != 0
    signal(SIGALRM, [](int) {
        std::fprintf(stderr, "HANG DETECTED (watchdog fired)\n");
        _exit(42);
    });

    app::install_deps(app::Deps{
        .stream         = [](auto, auto) {},
        .save_thread    = [](const Thread&) {},
        .delete_thread  = [](const auto&) {},
        .load_threads   = [] { return std::vector<Thread>{}; },
        .load_thread    = [](const ThreadId&) { return std::optional<Thread>{}; },
        .load_settings  = [] { return g_settings; },
        .save_settings  = [](const agentty::store::Settings& s) { g_settings = s; },
        .new_thread_id  = [] { return ThreadId{"t-repro"}; },
        .title_from     = [](std::string_view t) { return std::string{t}; },
        .auth           = {},
    });

    Model m;
    m = step(std::move(m), Msg{OpenSmartMode{}}, "open smart mode");
    render_once(m, "smart mode open");

    // Enter on the focused field (the master toggle row).
    auto* o = m.ui.panel.get<pn::SmartMode>();
    if (!o) { std::fprintf(stderr, "no smart pane!\n"); return 1; }
    using K = maya::SpecialKey;
    if (auto act = form::keys::translate(o->form, maya::KeyEvent{K::Enter}))
        m = step(std::move(m), Msg{SmartModeKey{*act}}, "enter on focused field");
    render_once(m, "after enter");

    // Walk every row, pressing Enter on each, rendering after each.
    for (int i = 0; i < 12; ++i) {
        auto* s = m.ui.panel.get<pn::SmartMode>();
        if (!s) break;
        if (auto down = form::keys::translate(s->form, maya::KeyEvent{K::Down}))
            m = step(std::move(m), Msg{SmartModeKey{*down}}, "down");
        s = m.ui.panel.get<pn::SmartMode>();
        if (!s) break;
        if (auto enter = form::keys::translate(s->form, maya::KeyEvent{K::Enter}))
            m = step(std::move(m), Msg{SmartModeKey{*enter}}, "enter");
        render_once(m, "after enter walk");
    }

    // ── Rag pane: Enter opens the mode dropdown / edits text fields ───
    m = step(std::move(m), Msg{OpenRag{}}, "open rag");
    render_once(m, "rag open");
    // ── The screenshot case: Enter on the Choice row opens the dropdown,
    // then Up/Down must move the menu highlight. ───────────────────
    {
        auto* r = m.ui.panel.get<pn::Rag>();
        if (!r) { std::fprintf(stderr, "no rag pane for dropdown test\n"); return 1; }
        // Cursor is on row 0 (Proactive, a Choice). Enter -> dropdown.
        if (auto enter = form::keys::translate(r->embed.form, maya::KeyEvent{K::Enter}))
            m = step(std::move(m), Msg{RagEmbedKey{*enter}}, "dropdown: enter");
        r = m.ui.panel.get<pn::Rag>();
        if (!r) { std::fprintf(stderr, "pane gone after enter\n"); return 1; }
        std::fprintf(stderr, "choosing=%d\n", r->embed.form.choosing() ? 1 : 0);
        if (!r->embed.form.choosing()) {
            std::fprintf(stderr, "FAIL: Enter did not open the dropdown\n");
            return 1;
        }
        const auto hl_before = [&] {
            const auto* d = r->embed.form.dropdown();
            return d ? d->highlighted : -999;
        }();
        if (auto down = form::keys::translate(r->embed.form, maya::KeyEvent{K::Down}))
            m = step(std::move(m), Msg{RagEmbedKey{*down}}, "dropdown: down");
        else
            std::fprintf(stderr, "FAIL: Down not translated while choosing\n");
        r = m.ui.panel.get<pn::Rag>();
        const auto hl_after = [&] {
            const auto* d = r ? r->embed.form.dropdown() : nullptr;
            return d ? d->highlighted : -999;
        }();
        std::fprintf(stderr, "highlight %d -> %d\n", hl_before, hl_after);
        if (hl_after == hl_before) {
            std::fprintf(stderr, "FAIL: Down did not move the dropdown highlight\n");
            return 1;
        }
        render_once(m, "dropdown after down");
        // Close it again for the walk below.
        if (auto esc = form::keys::translate(r->embed.form, maya::KeyEvent{K::Escape}))
            m = step(std::move(m), Msg{RagEmbedKey{*esc}}, "dropdown: esc");
    }

    // ── THE reported freeze: Enter on a text field starts editing; ↑/↓
    // must still navigate (leave the field + move), not go dead. ─────
    {
        auto* r = m.ui.panel.get<pn::Rag>();
        if (!r) { std::fprintf(stderr, "no rag pane for edit test\n"); return 1; }
        // Walk down to the Model row (a Text field).
        for (int i = 0; i < 2; ++i)
            if (auto down = form::keys::translate(r->embed.form, maya::KeyEvent{K::Down})) {
                m = step(std::move(m), Msg{RagEmbedKey{*down}}, "to text row");
                r = m.ui.panel.get<pn::Rag>();
            }
        const int at = r->embed.form.cursor;
        if (auto enter = form::keys::translate(r->embed.form, maya::KeyEvent{K::Enter}))
            m = step(std::move(m), Msg{RagEmbedKey{*enter}}, "enter on text row");
        r = m.ui.panel.get<pn::Rag>();
        std::fprintf(stderr, "editing=%d at=%d\n", r->embed.form.editing() ? 1 : 0, at);
        // Now the screenshot keystroke: Down, while editing.
        if (auto down = form::keys::translate(r->embed.form, maya::KeyEvent{K::Down}))
            m = step(std::move(m), Msg{RagEmbedKey{*down}}, "down while editing");
        else
            std::fprintf(stderr, "note: Down not translated while editing\n");
        r = m.ui.panel.get<pn::Rag>();
        std::fprintf(stderr, "cursor %d -> %d, editing now=%d\n",
                     at, r->embed.form.cursor, r->embed.form.editing() ? 1 : 0);
        if (r->embed.form.cursor == at) {
            std::fprintf(stderr, "FAIL: Down while editing did not move the cursor\n");
            return 1;
        }
        render_once(m, "after down-while-editing");
    }

    for (int i = 0; i < 16; ++i) {
        auto* r = m.ui.panel.get<pn::Rag>();
        if (!r) break;
        if (auto enter = form::keys::translate(r->embed.form, maya::KeyEvent{K::Enter}))
            m = step(std::move(m), Msg{RagEmbedKey{*enter}}, "rag enter");
        render_once(m, "rag after enter");
        r = m.ui.panel.get<pn::Rag>();
        if (!r) break;
        // Esc out of any edit/dropdown Enter started, then move on.
        if (auto esc = form::keys::translate(r->embed.form, maya::KeyEvent{K::Escape}))
            m = step(std::move(m), Msg{RagEmbedKey{*esc}}, "rag esc");
        r = m.ui.panel.get<pn::Rag>();
        if (!r) break;
        if (auto down = form::keys::translate(r->embed.form, maya::KeyEvent{K::Down}))
            m = step(std::move(m), Msg{RagEmbedKey{*down}}, "rag down");
        render_once(m, "rag after down");
    }

    // ── Stale-snapshot batch: subscribe.cpp translates a whole input
    // batch (a paste) against ONE FormFocus snapshot. Simulate the tail
    // of "x\nzz": the Enter leaves the field, but the following chars
    // still arrive as Insert intents. They must NOT mutate the row. ──
    {
        auto* r = m.ui.panel.get<pn::Rag>();
        if (!r) { std::fprintf(stderr, "no rag pane for race test\n"); return 1; }
        // Walk until the cursor is on a text-like row (previous sections
        // left it wherever their walk ended — possibly a Toggle, whose
        // Enter would FLIP it instead of editing).
        for (int i = 0; i < 20; ++i) {
            const auto* fr = r->embed.form.focused();
            if (fr && fr->is_text_like() && !fr->locked) break;
            if (auto down = form::keys::translate(r->embed.form, maya::KeyEvent{K::Down})) {
                m = step(std::move(m), Msg{RagEmbedKey{*down}}, "race: seek text row");
                r = m.ui.panel.get<pn::Rag>();
            }
        }
        // Start editing the focused text row, insert 'x', then Enter (leave).
        if (auto enter = form::keys::translate(r->embed.form, maya::KeyEvent{K::Enter}))
            m = step(std::move(m), Msg{RagEmbedKey{*enter}}, "race: start edit");
        r = m.ui.panel.get<pn::Rag>();
        // Snapshot taken HERE (editing=true) — like subscribe.cpp's capture.
        const bool snap_editing  = r->embed.form.editing();
        const bool snap_choosing = r->embed.form.choosing();
        auto tr = [&](maya::KeyEvent ev) {
            return form::keys::translate(snap_editing, snap_choosing, ev);
        };
        auto text_of = [](const agentty::form::Field* fld) -> std::string {
            if (!fld) return {};
            if (const auto* t = std::get_if<agentty::form::field::Text>(&fld->value))
                return t->value;
            if (const auto* p = std::get_if<agentty::form::field::Path>(&fld->value))
                return p->value;
            return {};
        };
        if (auto a = tr(maya::KeyEvent{maya::CharKey{U'x'}}))
            m = step(std::move(m), Msg{RagEmbedKey{*a}}, "race: insert x");
        if (auto a = tr(maya::KeyEvent{K::Enter}))
            m = step(std::move(m), Msg{RagEmbedKey{*a}}, "race: enter (leaves)");
        // Value AFTER the edit ended:
        r = m.ui.panel.get<pn::Rag>();
        const auto val_after_leave = text_of(r->embed.form.focused());
        // Sanity: the in-edit insert must have LANDED, or this test is
        // checking nothing. (Catches the probe reading the wrong variant.)
        if (val_after_leave.find('x') == std::string::npos) {
            std::fprintf(stderr, "FAIL: setup insert never landed (val='%s')\n",
                         val_after_leave.c_str());
            return 1;
        }
        // The paste tail: still translated with the STALE snapshot.
        if (auto a = tr(maya::KeyEvent{maya::CharKey{U'z'}}))
            m = step(std::move(m), Msg{RagEmbedKey{*a}}, "race: stale insert z");
        r = m.ui.panel.get<pn::Rag>();
        const auto val_now = text_of(r->embed.form.focused());
        std::fprintf(stderr, "race: '%s' -> '%s' (editing=%d)\n",
                     val_after_leave.c_str(), val_now.c_str(),
                     r->embed.form.editing() ? 1 : 0);
        if (val_now != val_after_leave) {
            std::fprintf(stderr, "FAIL: stale Insert mutated a left field\n");
            return 1;
        }
    }

    // ── Home/End/PgDn: jump keys work while browsing, and clamp. ────
    {
        auto* r = m.ui.panel.get<pn::Rag>();
        if (!r) { std::fprintf(stderr, "no rag pane for jump test\n"); return 1; }
        auto key = [&](maya::SpecialKey k) {
            if (auto a = form::keys::translate(r->embed.form, maya::KeyEvent{k})) {
                m = step(std::move(m), Msg{RagEmbedKey{*a}}, "jump key");
                r = m.ui.panel.get<pn::Rag>();
            }
        };
        key(K::End);
        const int at_end = r->embed.form.cursor;
        key(K::Down);   // wrap-guard: End then Down wraps to top by move();
        key(K::End);    // back to the end
        if (r->embed.form.cursor != at_end) {
            std::fprintf(stderr, "FAIL: End not idempotent (%d vs %d)\n",
                         r->embed.form.cursor, at_end);
            return 1;
        }
        key(K::PageDown);   // at the end: must CLAMP, not wrap
        if (r->embed.form.cursor != at_end) {
            std::fprintf(stderr, "FAIL: PgDn at the end wrapped (%d vs %d)\n",
                         r->embed.form.cursor, at_end);
            return 1;
        }
        key(K::Home);
        const int at_home = r->embed.form.cursor;
        if (at_home >= at_end) {
            std::fprintf(stderr, "FAIL: Home did not go above End (%d vs %d)\n",
                         at_home, at_end);
            return 1;
        }
        std::fprintf(stderr, "jumps: home=%d end=%d ok\n", at_home, at_end);
    }

    // ── The adopt chain: palette → (a command's panel) → Esc must restore
    // the palette with its typed query intact. ThreadList: opens even with
    // zero threads (renders its empty state), so the chain always runs. ─
    {
        Model m2;
        m2 = step(std::move(m2), Msg{OpenPalette{}}, "chain: open palette");
        for (char c : std::string{"threads"})
            m2 = step(std::move(m2), Msg{PaletteInput{static_cast<char32_t>(c)}},
                      "chain: type");
        m2 = step(std::move(m2), Msg{PaletteSelect{}}, "chain: select");
        if (!m2.ui.panel.is<pn::ThreadList>()) {
            std::fprintf(stderr, "FAIL: 'threads' did not open the thread list\n");
            return 1;
        }
        m2 = step(std::move(m2), Msg{CloseThreadList{}}, "chain: esc");
        if (!m2.ui.panel.is<pn::Palette>()) {
            std::fprintf(stderr, "FAIL: Esc from thread list did not restore the palette\n");
            return 1;
        }
        const auto* p = m2.ui.panel.get<pn::Palette>();
        if (p->query != "threads") {
            std::fprintf(stderr, "FAIL: restored palette lost the query ('%s')\n",
                         p->query.c_str());
            return 1;
        }
        std::fprintf(stderr, "chain: palette restored with query='threads'\n");
        // And Esc AGAIN closes to the thread — the chain has exactly the
        // depth the user built, no phantom levels.
        m2 = step(std::move(m2), Msg{ClosePalette{}}, "chain: esc 2");
        if (!m2.ui.panel.is<pn::None>()) {
            std::fprintf(stderr, "FAIL: second Esc did not reach the thread\n");
            return 1;
        }
        std::fprintf(stderr, "chain: second esc closed to thread\n");
    }

    // ── Async-probe race: Test launches a worker; the user EDITS the
    // config before the completion lands. The stale TestDone must NOT
    // stamp Ok/dim onto a config the probe never saw. ─────────────
    {
        auto* r = m.ui.panel.get<pn::Rag>();
        if (!r) { std::fprintf(stderr, "no rag pane for probe race\n"); return 1; }
        // Walk to an editable text row — the previous section left the
        // cursor at End (an Action/locked row where Insert no-ops, which
        // would make this test assert nothing).
        for (int i = 0; i < 24; ++i) {
            const auto* fr = r->embed.form.focused();
            if (fr && fr->is_text_like() && !fr->locked) break;
            if (auto up = form::keys::translate(r->embed.form, maya::KeyEvent{K::Up})) {
                m = step(std::move(m), Msg{RagEmbedKey{*up}}, "probe race: seek");
                r = m.ui.panel.get<pn::Rag>();
            }
        }
        const auto gen_at_launch = ++r->embed.probe_gen;   // simulate launch
        r->embed.probe = agentty::rag_settings::EmbedForm::Testing{};
        // The user edits a field → invalidate_probe bumps the generation.
        if (auto enter = form::keys::translate(r->embed.form, maya::KeyEvent{K::Enter}))
            m = step(std::move(m), Msg{RagEmbedKey{*enter}}, "probe race: edit");
        if (auto a = form::keys::translate(true, false, maya::KeyEvent{maya::CharKey{U'x'}}))
            m = step(std::move(m), Msg{RagEmbedKey{*a}}, "probe race: type");
        // The worker's answer arrives, stamped with the OLD generation.
        m = step(std::move(m),
                 Msg{RagEmbedTestDone{true, 768, 42, "", gen_at_launch}},
                 "probe race: stale done");
        r = m.ui.panel.get<pn::Rag>();
        if (std::holds_alternative<agentty::rag_settings::EmbedForm::Ok>(r->embed.probe)) {
            std::fprintf(stderr,
                         "FAIL: stale probe completion verified an edited config\n");
            return 1;
        }
        // And a CURRENT-generation completion still lands.
        const auto gen_now = ++r->embed.probe_gen;
        r->embed.probe = agentty::rag_settings::EmbedForm::Testing{};
        m = step(std::move(m),
                 Msg{RagEmbedTestDone{true, 768, 42, "", gen_now}},
                 "probe race: fresh done");
        r = m.ui.panel.get<pn::Rag>();
        if (!std::holds_alternative<agentty::rag_settings::EmbedForm::Ok>(r->embed.probe)) {
            std::fprintf(stderr, "FAIL: fresh probe completion was dropped\n");
            return 1;
        }
        std::fprintf(stderr, "probe race: stale dropped, fresh landed\n");
    }

    std::fprintf(stderr, "ALL OK\n");
    return 0;
}
