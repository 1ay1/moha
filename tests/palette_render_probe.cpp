// palette_render_probe — renders the command palette AND the diff-review pane
// and asserts their layout + styling invariants. Lives in the fold suite so it
// links the full runtime object tree and renders through the real maya path.
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/view/panels.hpp"
#include "agentty/runtime/view/diff_review.hpp"
#include "agentty/runtime/panel/palette.hpp"
#include "agentty/diff/diff.hpp"
#include "agentty/runtime/panel/common.hpp"
#include <maya/app/inline.hpp>
#include <cstdio>
#include <string>

namespace pn = agentty::ui::panel;

using namespace agentty;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    if (!ok) { std::printf("  FAIL: %s\n", what); ++g_fail; }
}
static bool has(const std::string& h, const std::string& n) {
    return h.find(n) != std::string::npos;
}
static bool has_mojibake(const std::string& s) { return s.find("\xef\xbf\xbd") != std::string::npos; }
static int count_of(const std::string& h, const std::string& n) {
    int c = 0; for (std::size_t p = 0; (p = h.find(n, p)) != std::string::npos; p += n.size()) ++c;
    return c;
}

static void palette_checks() {
    Model m;
    m.ui.panel = pn::Palette{{}};
    m.d.pending_changes.push_back(FileChange{});
    auto rend = [&]{ return maya::render_to_string(ui::palette_panel(m), 82); };
    std::string out = rend();
    // `--dump` prints the frame so a human can look at it, the same way
    // embed_render_probe does. Checks still run either way.
    if (const char* d = std::getenv("PALETTE_DUMP"); d && d[0])
        std::printf("%s\n", out.c_str());
    // Sections are plain headers now: Panel renders one as caps + a rule to
    // the right edge. The old "┌─ THREAD" bracket, its │ spine down every row
    // and its └ closer were a SECOND grouping system drawn on top of that —
    // and the spine cost a badge column on every row to repeat what the
    // header above it already said.
    check(has(out, "THREAD"), "palette: section header present");
    check(has(out, "\xe2\x94\x80\xe2\x94\x80"), "palette: header rule");
    check(!has(out, "\xe2\x94\x8c\xe2\x94\x80 THREAD"), "palette: no bracket header");
    check(!has_mojibake(out), "palette: no mojibake");
    auto* o = m.ui.panel.get<agentty::ui::panel::Palette>();
    o->query = "rev";
    std::string ansi = maya::render_to_string_ansi(ui::palette_panel(m), 82);
    // Locate the Review row and confirm it carries a distinct highlight style.
    auto strip = [](const std::string& s){ std::string r; for (std::size_t i=0;i<s.size();){ if(s[i]=='\x1b'){ i=s.find('m',i); if(i==std::string::npos)break; ++i;} else r+=s[i++]; } return r; };
    std::string hot; std::size_t st=0;
    while (st<=ansi.size()){ std::size_t nl=ansi.find('\n',st); std::string ln=ansi.substr(st,nl-st); if(strip(ln).find("Review changes")!=std::string::npos){hot=ln;break;} if(nl==std::string::npos)break; st=nl+1; }
    check(!hot.empty(), "palette: Review row present (ANSI)");
    check(count_of(hot, "\x1b[") >= 4, "palette: matched chars carry a highlight style");
}

static void diff_review_checks() {
    // Two files, first has two hunks (all pending).
    Model m;
    auto a = diff::compute("src/login.cpp",
        "int f() {\n  return 0;\n}\n", "int f() {\n  return -1;\n  log();\n}\n");
    a.added = 2; a.removed = 1;
    m.d.pending_changes.push_back(a);
    auto b = diff::compute("README.md", "old\n", "new\nmore\n");
    m.d.pending_changes.push_back(b);
    m.ui.panel = ui::panel::DiffReview{{0, 0}};

    std::string out = maya::render_to_string(ui::diff_review(m), 84);
    check(has(out, "login.cpp") && has(out, "README.md"),
          "diff: file rail shows every file");
    check(has(out, "reviewed"), "diff: overall hunk progress");
    check(has(out, "hunk 1/"), "diff: labelled hunk dividers (not raw @@)");
    check(!has(out, "@@ -"), "diff: raw git @@ headers are gone");
    check(has(out, "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"), "diff: has a rule");
    check(!has_mojibake(out), "diff: no mojibake");

    // All-reviewed state: mark every hunk accepted → the apply affordance.
    for (auto& fc : m.d.pending_changes)
        for (auto& hk : fc.hunks) hk.status = Hunk::Status::Accepted;
    std::string done = maya::render_to_string(ui::diff_review(m), 84);
    check(has(done, "all reviewed"), "diff: all-reviewed banner appears");
    check(has(done, "\xe2\x96\x88"), "diff: progress bar fills (█) when done");
}

int main() {
    palette_checks();
    diff_review_checks();
    if (g_fail == 0) std::puts("palette_render_probe: OK");
    return g_fail ? 1 : 0;
}
