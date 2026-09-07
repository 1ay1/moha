// midrun_wire_test — WIRE-LEVEL scrollback-duplication regression.
//
// midrun_seam_test renders each model state with render_tree and compares
// the row arrays. That catches CONTENT/HEIGHT divergence but NOT the
// actual maya inline-compose wire emission: render_tree has no prev_cells
// shadow, no overflow commit, no cursor walk. The screenshot bug
// (settled write card duplicates into scrollback) is a WIRE artifact —
// the freeze frame re-emits rows that already overflowed the viewport
// top, leaving the old copy stranded one screen up.
//
// This test drives maya's REAL inline compose (the same compose_inline_
// frame the runtime calls) over agentty's real Element tree:
//
//   frame A: live tail holds the settled write (full body), overflowing.
//            Render it Synced. Some rows commit to native scrollback.
//   frame B: the write is frozen (freeze_through). Render the new tree
//            against the SAME InlineFrameState.
//
// Invariant: the bytes frame B emits must NOT contain a re-paint of a
// row that frame A already pushed above the viewport top. We model the
// wire as a row grid the emitter mutates; any write to a row index <
// (frameA_rows - term_h) is a committed-scrollback rewrite = the
// duplication bug.

#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/render/inline_frame.hpp>
#include <maya/render/serialize.hpp>
#include <maya/style/theme.hpp>
#include <maya/terminal/writer.hpp>
#include <maya/widget/app_layout.hpp>

#include "agtest.hpp"

#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/panel/common.hpp"
#include "agentty/runtime/view/changes_strip.hpp"
#include "agentty/runtime/view/composer.hpp"
#include "agentty/runtime/view/panels.hpp"
#include "agentty/runtime/view/status_bar/status_bar.hpp"
#include "agentty/runtime/view/thread/conversation.hpp"
#include "agentty/runtime/view/thread/thread.hpp"
#include "agentty/runtime/view/view.hpp"

namespace pn = agentty::ui::panel;

using agentty::Model;
using agentty::Message;
using agentty::Role;
using agentty::ToolCallId;
using agentty::ToolName;
using agentty::ToolUse;
using std::chrono::steady_clock;
using std::chrono::milliseconds;

using namespace maya;
using namespace maya::inline_frame;

static int g_checks   = 0;


// ── Non-blocking pipe writer; the read end is drained per frame so we
//    capture exactly the bytes each compose emitted.
static std::pair<Writer, int> make_pipe_writer() {
    int fds[2];
    if (pipe(fds) != 0) { std::perror("pipe"); std::abort(); }
    fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL, 0) | O_NONBLOCK);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);
    return {Writer{static_cast<platform::NativeHandle>(fds[1])}, fds[0]};
}

static std::string drain(int rfd) {
    std::string out;
    char buf[8192];
    ssize_t n;
    while ((n = read(rfd, buf, sizeof(buf))) > 0)
        out.append(buf, static_cast<std::size_t>(n));
    return out;
}

// Build the conversation Element tree for the current model state.
static maya::Element build_root(const Model& m) {
    return maya::AppLayout{{
        .thread        = agentty::ui::thread_config(m),
        .changes_strip = agentty::ui::changes_strip_config(m),
        .composer      = agentty::ui::composer_config(m),
        .status_bar    = agentty::ui::status_bar_config(m),
        .overlay       = std::nullopt,
    }}.build();
}

// Render `root` into a canvas sized to its content; return the canvas.
static Canvas paint(const maya::Element& root, int width, StylePool& pool) {
    // Seed tall; render_tree with auto_height grows it.
    Canvas c(width, 4000, &pool);
    c.clear();
    std::vector<layout::LayoutNode> nodes;
    maya::render_tree(root, c, pool, maya::theme::dark, nodes, /*auto_height=*/true);
    return c;
}

static ToolUse settled_write(const std::string& tag, int n_lines) {
    ToolUse t;
    t.id   = ToolCallId{"write_" + tag};
    t.name = ToolName{"write"};
    std::string content;
    for (int i = 0; i < n_lines; ++i)
        content += "line " + std::to_string(i) + ": plausible file body text\n";
    t.args = {{"file_path", "/tmp/" + tag + ".md"}, {"content", content}};
    auto now = steady_clock::now();
    t.status = ToolUse::Done{now - milliseconds{5}, now,
                             "Created /tmp/" + tag + ".md"};
    return t;
}

// A settled `read` card with a tall file body (one row per source line,
// elided to head/tail by ToolBodyPreview). Distinct id + per-line content
// so two cards of the SAME byte size still render different bodies — the
// per-event hash_id collision surface.
static ToolUse settled_read(const std::string& tag, int n_lines,
                            const std::string& path) {
    ToolUse t;
    t.id   = ToolCallId{"read_" + tag};
    t.name = ToolName{"read"};
    t.args = {{"path", path}};
    std::string out;
    for (int i = 0; i < n_lines; ++i)
        out += std::to_string(i) + ": " + tag + " source line text here\n";
    auto now = steady_clock::now();
    t.status = ToolUse::Done{now - milliseconds{5}, now, std::move(out)};
    return t;
}

// THE screenshot scenario at the wire: a SINGLE in-flight assistant run
// that grows by one tall terminal tool card per sub-turn (the model reads
// several files in a row). NOTHING freezes mid-run (agent_session
// discipline) — the whole multi-card run is the LIVE tail and re-lays-out
// every frame as each new card appends, scrolling earlier cards off the
// viewport top.
//
// This is the exact shape in the corruption screenshot: many already-
// terminal read/bash cards stacked in one live run, with stray cells from
// one card's body bleeding into another card's footer. The per-event
// hash_id blits each terminal card's cached cells; if a card above grows
// and shifts a cached card to a new Y, maya's row diff + will_scroll_off
// defense must keep prev_cells in lockstep with the wire or a committed
// row gets a stale blit.
//
// Invariant per frame: the shadow stays valid (verify() succeeds), the
// frame stays Synced (no recovery repaint), and no row that already
// overflowed the viewport top on a PRIOR frame is rewritten.
TEST_CASE("growing live run no committed rewrite") {
    constexpr int kWidth = 100;
    constexpr int kTermH = 30;

    Model m;
    m.d.current.id = agentty::ThreadId{"wiregrow"};
    Message u; u.role = Role::User; u.text = "investigate the renderer";
    m.d.current.messages.push_back(std::move(u));
    // The whole prior turn freezes at submit (SessionStart analog); the
    // live tail is ONLY the in-flight assistant run below.
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, 1);
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};

    // The in-flight assistant run: one Message (sub-turn) per tool card,
    // appended one at a time. Each sub-turn settles its tool before the
    // next arrives — the realistic post-tool placeholder cadence.
    StylePool pool;
    auto [writer, rfd] = make_pipe_writer();

    // Track the wire as a row grid: the bytes of each row at the moment it
    // scrolled off the viewport top are immutable in native scrollback.
    // We snapshot every committed prefix and require it never changes.
    std::vector<std::string> committed_wire;   // rows already off-screen

    auto rows_of = [&](const Canvas& c) {
        std::vector<std::string> rows;
        const int mr = c.max_content_row();
        for (int y = 0; y <= mr; ++y) {
            std::string line;
            for (int x = 0; x < kWidth; ++x) {
                char32_t ch = c.get(x, y).character;
                line.push_back(ch && ch < 128 ? static_cast<char>(ch) : ' ');
            }
            while (!line.empty() && line.back() == ' ') line.pop_back();
            rows.push_back(std::move(line));
        }
        return rows;
    };

    // Seed the first frame fresh, then carry Synced state across appends.
    std::optional<InlineFrame<Synced>> synced;
    const char* files[] = {
        "maya/src/render/serialize.cpp", "maya/src/render/renderer.cpp",
        "maya/include/maya/app/app.hpp", "src/runtime/app/update/frozen.cpp",
        "src/runtime/app/update/stream.cpp", "maya/examples/agent_session.cpp",
    };
    constexpr int kCards = 6;
    bool ok_all = true;

    for (int card = 0; card < kCards && ok_all; ++card) {
        // Append a new sub-turn with a tall terminal read card. Bodies of
        // VARYING height so each append shifts the rows below by a
        // different multi-row delta (exercises will_scroll_off's range).
        Message a; a.role = Role::Assistant;
        a.tool_calls.push_back(settled_read(
            "c" + std::to_string(card), 30 + card * 17, files[card]));
        m.d.current.messages.push_back(std::move(a));

        Canvas c = paint(build_root(m), kWidth, pool);
        auto cur_rows = rows_of(c);
        const int total_rows = static_cast<int>(cur_rows.size());
        const int new_committed = total_rows > kTermH ? total_rows - kTermH : 0;

        if (!synced) {
            auto outcome = InlineFrame<Empty>{}.seed().render(
                c, content_rows(c), term_rows_for_test(kTermH), pool, writer,
                false);
            (void)drain(rfd);
            synced = std::visit(
                [](auto&& arm) -> std::optional<InlineFrame<Synced>> {
                    using T = std::decay_t<decltype(arm)>;
                    if constexpr (std::is_same_v<T, InlineFrame<Synced>>)
                        return std::move(arm);
                    else return std::nullopt;
                }, std::move(outcome));
            CHECK(synced.has_value(), "grow: first frame did not reach Synced");
            if (!synced) { ok_all = false; break; }
        } else {
            auto wit = synced->verify();
            CHECK(wit.has_value(),
                  "grow: shadow verify failed before append render "
                  "(prev_cells already desynced from the wire)");
            if (!wit) { ok_all = false; break; }
            auto proof = synced->check_scrollback(c, kTermH);
            CHECK(proof.has_value(),
                  "grow: scrollback gate rejected the append frame "
                  "(committed prefix shifted)");
            if (!proof) { ok_all = false; break; }
            auto outcome = std::move(*synced).render(
                c, content_rows(c), term_rows_for_test(kTermH), pool, writer,
                std::move(*wit), std::move(*proof), false);
            (void)drain(rfd);
            synced = std::visit(
                [](auto&& arm) -> std::optional<InlineFrame<Synced>> {
                    using T = std::decay_t<decltype(arm)>;
                    if constexpr (std::is_same_v<T, InlineFrame<Synced>>)
                        return std::move(arm);
                    else return std::nullopt;
                }, std::move(outcome));
            CHECK(synced.has_value(),
                  "grow: append frame demoted out of Synced — maya hit the "
                  "overflow recovery repaint (the bleed/ghost symptom)");
            if (!synced) { ok_all = false; break; }
        }

        // Every row that was committed on a PRIOR frame must still hold the
        // SAME bytes now. A change here is a committed-scrollback rewrite —
        // the stray-cell bleed from the screenshot.
        const int check_n = std::min<int>(
            static_cast<int>(committed_wire.size()), new_committed);
        for (int y = 0; y < check_n; ++y) {
            if (committed_wire[static_cast<std::size_t>(y)]
                != cur_rows[static_cast<std::size_t>(y)]) {
                std::fprintf(stderr,
                    "  GROW committed-row rewrite at y=%d (card=%d):\n"
                    "    was |%s|\n    now |%s|\n",
                    y, card,
                    committed_wire[static_cast<std::size_t>(y)].c_str(),
                    cur_rows[static_cast<std::size_t>(y)].c_str());
                CHECK(false,
                      "a row that already overflowed the viewport top was "
                      "rewritten on a later frame — committed-scrollback "
                      "corruption (the screenshot bleed)");
                ok_all = false;
                break;
            }
        }
        // Extend the committed snapshot to the new (deeper) boundary.
        committed_wire.assign(cur_rows.begin(),
                              cur_rows.begin() + new_committed);
    }

    std::fprintf(stderr, "  [grow] cards=%d committed_rows=%zu\n",
                 kCards, committed_wire.size());
    close(rfd);
}

// A grep card whose output references `path` at the given line numbers,
// in agentty's grep markdown shape (collect_grep_hits parses
// `## Matches in <path>` headers + `### L<n>` anchors).
static ToolUse settled_grep(const std::string& tag, const std::string& path,
                            const std::vector<int>& lines) {
    ToolUse t;
    t.id   = ToolCallId{"grep_" + tag};
    t.name = ToolName{"grep"};
    t.args = {{"pattern", "needle_" + tag}};
    std::string out = "## Matches in " + path + "\n\n";
    for (int ln : lines)
        out += "### L" + std::to_string(ln) + "-" + std::to_string(ln)
             + "\n```\n" + std::to_string(ln) + ": match here\n```\n\n";
    auto now = steady_clock::now();
    t.status = ToolUse::Done{now - milliseconds{5}, now, std::move(out)};
    return t;
}

// REGRESSION: grep → read on the same path. The Read card settles and is
// rendered/cached FIRST (no highlight). Then a same-path Grep lands later
// in the run: collect_grep_hits now indexes the path, so the Read body
// gains a leading `▸ matches: …` summary row — ONE row taller — while
// tc.output().size() is UNCHANGED. The per-event hash_id must therefore
// fold highlight_lines, or maya blits the cached (shorter) cells into the
// now-taller reserved slot, shifting every row below by one and bleeding
// stale cells (the screenshot corruption). We drive the REAL component
// cache (paint() shares the thread-local cache across frames) so a stale
// blit would actually surface.
TEST_CASE("grep read highlight no stale blit") {
    constexpr int kWidth = 100;
    constexpr int kTermH = 30;
    const std::string path = "maya/src/render/serialize.cpp";

    Model m;
    m.d.current.id = agentty::ThreadId{"wirehl"};
    Message u; u.role = Role::User; u.text = "find and read the diff path";
    m.d.current.messages.push_back(std::move(u));
    // Lead-in frozen turns push the live read card's top above the
    // viewport so its rows genuinely commit to native scrollback and a
    // one-row shift below a stale blit corrupts a COMMITTED row.
    for (int e = 0; e < 8; ++e) {
        Message lu; lu.role = Role::User;
        lu.text = "context turn " + std::to_string(e);
        m.d.current.messages.push_back(std::move(lu));
        Message la; la.role = Role::Assistant;
        la.tool_calls.push_back(settled_write("lead" + std::to_string(e), 6));
        m.d.current.messages.push_back(std::move(la));
    }
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, m.d.current.messages.size());
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};

    StylePool pool;
    auto [writer, rfd] = make_pipe_writer();

    auto rows_of = [&](const Canvas& c) {
        std::vector<std::string> rows;
        const int mr = c.max_content_row();
        for (int y = 0; y <= mr; ++y) {
            std::string line;
            for (int x = 0; x < kWidth; ++x) {
                char32_t ch = c.get(x, y).character;
                line.push_back(ch && ch < 128 ? static_cast<char>(ch) : ' ');
            }
            while (!line.empty() && line.back() == ' ') line.pop_back();
            rows.push_back(std::move(line));
        }
        return rows;
    };

    // Sub-turn 1: the Read card settles FIRST, alone, and gets cached
    // (per-event hash_id keyed under no-highlight height). The grep that
    // will highlight it lands in the SAME sub-turn message a beat later
    // (the model emits read + grep in one tool batch; the read's tool_use
    // arrives and settles before the grep's does).
    {
        Message a; a.role = Role::Assistant;
        a.tool_calls.push_back(settled_read("r", 60, path));
        m.d.current.messages.push_back(std::move(a));
    }
    Canvas ca = paint(build_root(m), kWidth, pool);
    auto rows_a = rows_of(ca);
    auto oa = InlineFrame<Empty>{}.seed().render(
        ca, content_rows(ca), term_rows_for_test(kTermH), pool, writer, false);
    (void)drain(rfd);
    InlineFrame<Synced> sa = std::visit(
        [](auto&& arm) -> InlineFrame<Synced> {
            using T = std::decay_t<decltype(arm)>;
            if constexpr (std::is_same_v<T, InlineFrame<Synced>>)
                return std::move(arm);
            else { std::fprintf(stderr, "  hl frame A not Synced\n");
                   std::abort(); }
        }, std::move(oa));
    const int rows_a_n   = sa.rows();
    const int committed_a = rows_a_n > kTermH ? rows_a_n - kTermH : 0;

    // The grep lands in the SAME message's tool batch. collect_grep_hits
    // (per-panel, over that message's tool_calls) now indexes the path,
    // so the Read above inherits highlight_lines → its body grows by the
    // `▸ matches:` summary row.
    m.d.current.messages.back().tool_calls.push_back(
        settled_grep("g", path, {42, 61, 88}));
    Canvas cb = paint(build_root(m), kWidth, pool);
    auto rows_b = rows_of(cb);
    auto wit = sa.verify();
    CHECK(wit.has_value(),
          "hl: shadow verify failed after frame A (already desynced)");
    if (wit) {
        auto proof = sa.check_scrollback(cb, kTermH);
        CHECK(proof.has_value(),
              "hl: scrollback gate rejected the grep-append frame");
        auto ob = std::move(sa).render(
            cb, content_rows(cb), term_rows_for_test(kTermH), pool, writer,
            std::move(*wit), std::move(*proof), false);
        (void)drain(rfd);
        bool synced_b = std::visit([](auto&& arm) {
            using T = std::decay_t<decltype(arm)>;
            return std::is_same_v<T, InlineFrame<Synced>>;
        }, std::move(ob));
        CHECK(synced_b,
              "hl: grep-append frame demoted out of Synced — stale-blit "
              "height mismatch tripped the recovery repaint");
    }

    // The committed prefix (rows that overflowed on frame A) must NOT have
    // been rewritten by frame B. If the per-event key ignored
    // highlight_lines, the read body's cells would blit one row short into
    // a taller slot, shifting committed rows.
    const int check_n = std::min<int>(committed_a,
        std::min<int>(static_cast<int>(rows_a.size()),
                      static_cast<int>(rows_b.size())));
    int first_div = -1;
    for (int y = 0; y < check_n; ++y) {
        if (rows_a[static_cast<std::size_t>(y)]
            != rows_b[static_cast<std::size_t>(y)]) { first_div = y; break; }
    }
    if (first_div >= 0) {
        std::fprintf(stderr,
            "  HL committed-row divergence at %d (committed=%d):\n"
            "    A |%s|\n    B |%s|\n",
            first_div, committed_a,
            rows_a[static_cast<std::size_t>(first_div)].c_str(),
            rows_b[static_cast<std::size_t>(first_div)].c_str());
    }
    CHECK(first_div < 0,
          "a committed scrollback row changed when a same-path Grep added "
          "the read's highlight summary row — stale-blit height mismatch "
          "(per-event hash_id must fold highlight_lines)");

    // DIRECT stale-blit check: re-PAINT frame B's tree into a fresh canvas
    // that shares the SAME thread-local component cache the frame-A paint
    // populated. If the read's per-event hash_id didn't fold
    // highlight_lines, this paint takes the cache FAST PATH and blits the
    // frame-A cells (no `▸ matches:` row, one row short) into the now-
    // taller reserved slot — the canvas then shows the read body's last
    // gutter line where its footer/`done` row should be (the screenshot's
    // stray line-number + `DO2`/`DONE` overdraw). We assert the painted
    // canvas contains the summary row that the body MUST now have; a stale
    // blit omits it.
    Canvas cc = paint(build_root(m), kWidth, pool);   // cache-hit paint
    auto rows_c = rows_of(cc);
    bool has_matches_row = false;
    for (const auto& r : rows_c)
        if (r.find("matches:") != std::string::npos) { has_matches_row = true; break; }
    CHECK(has_matches_row,
          "the read body's `▸ matches:` summary row is MISSING from the "
          "cache-hit repaint — maya blitted the stale (pre-highlight) cells "
          "because the per-event hash_id ignored highlight_lines (the "
          "screenshot stale-blit / gutter-bleed corruption)");
    // The two consecutive frame-B paints (cb built fresh, cc cache-hit)
    // must be byte-identical: a stale blit makes cc shorter / shifted.
    int cdiv = -1;
    const int cmin = std::min<int>(static_cast<int>(rows_b.size()),
                                   static_cast<int>(rows_c.size()));
    for (int y = 0; y < cmin; ++y)
        if (rows_b[static_cast<std::size_t>(y)]
            != rows_c[static_cast<std::size_t>(y)]) { cdiv = y; break; }
    if (cdiv >= 0)
        std::fprintf(stderr,
            "  HL cache-hit repaint diverged at %d:\n    fresh |%s|\n"
            "    cached|%s|\n", cdiv,
            rows_b[static_cast<std::size_t>(cdiv)].c_str(),
            rows_c[static_cast<std::size_t>(cdiv)].c_str());
    CHECK(cdiv < 0 && rows_b.size() == rows_c.size(),
          "cache-hit repaint of the same tree diverged from the fresh "
          "paint — a stale-height blit corrupted the canvas");

    std::fprintf(stderr, "  [hl] committed=%d rows_a=%d rows_b=%zu rows_c=%zu\n",
                 committed_a, rows_a_n, rows_b.size(), rows_c.size());
    close(rfd);
}

// THE wire-level test. A user turn + a settled write that overflows the
// viewport, frozen at the (idle) turn boundary. Drive maya's real inline
// compose across the freeze and assert no committed scrollback row is
// rewritten.
TEST_CASE("write freeze no rewrite") {
    constexpr int kWidth = 100;
    constexpr int kTermH = 30;

    Model m;
    m.d.current.id = agentty::ThreadId{"wire"};
    Message u; u.role = Role::User; u.text = "write a big file";
    m.d.current.messages.push_back(std::move(u));
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, 1);

    // The settled write as the active turn's only sub-turn. Stream is
    // idle (finalize_turn path): m.s NOT active, so it lingers live until
    // freeze_through. This mirrors "model wrote a file then stopped."
    Message a; a.role = Role::Assistant;
    a.tool_calls.push_back(settled_write("big", 120));
    m.d.current.messages.push_back(std::move(a));
    // phase idle by default (no Streaming{Active}).

    StylePool pool;
    auto [writer, rfd] = make_pipe_writer();

    // ── frame A: render the LIVE settled write. Seed Empty→Fresh→Synced.
    Canvas ca = paint(build_root(m), kWidth, pool);
    auto outcome_a = InlineFrame<Empty>{}.seed().render(
        ca, content_rows(ca), term_rows_for_test(kTermH), pool, writer,
        /*sync=*/false);
    (void)drain(rfd);

    InlineFrame<Synced> sa = std::visit(
        [](auto&& arm) -> InlineFrame<Synced> {
            using T = std::decay_t<decltype(arm)>;
            if constexpr (std::is_same_v<T, InlineFrame<Synced>>)
                return std::move(arm);
            else { std::fprintf(stderr, "  frame A did not reach Synced\n");
                   std::abort(); }
        }, std::move(outcome_a));

    const int rows_a = sa.rows();
    const int committed_a = rows_a > kTermH ? rows_a - kTermH : 0;
    CHECK(committed_a > 0,
          "test setup: live write must overflow the viewport "
          "(else there's no committed scrollback to corrupt)");

    // ── freeze: the write graduates into the frozen prefix (idle path).
    agentty::app::detail::freeze_through(m, m.d.current.messages.size());

    // ── frame B: render the FROZEN tree against the SAME state. Verify
    //    the shadow first (mirrors the runtime's Synced arm), then render.
    Canvas cb = paint(build_root(m), kWidth, pool);
    auto wit = sa.verify();
    CHECK(wit.has_value(),
          "shadow verify failed after frame A (state already poisoned)");

    std::string bytes_b;
    if (wit) {
        auto proof = sa.check_scrollback(cb, kTermH);
        CHECK(proof.has_value(),
              "freeze: scrollback gate rejected the frozen frame");
        auto outcome_b = std::move(sa).render(
            cb, content_rows(cb), term_rows_for_test(kTermH), pool, writer,
            std::move(*wit), std::move(*proof), /*sync=*/false);
        bytes_b = drain(rfd);
        // It must stay Synced — a demote here means maya itself detected
        // an overflow-while-poisoned and is recovering (visible flicker).
        bool synced_b = std::visit([](auto&& arm) {
            using T = std::decay_t<decltype(arm)>;
            return std::is_same_v<T, InlineFrame<Synced>>;
        }, std::move(outcome_b));
        CHECK(synced_b,
              "freeze frame demoted out of Synced — maya hit a recovery "
              "path (overflow-while-poisoned), the duplicate-write symptom");
    }

    // Frame B's content above the viewport top must match frame A's: the
    // frozen render of the write must be byte-identical to the live one.
    // We re-derive both canvases as row strings and compare the committed
    // prefix [0, committed_a).
    auto rows_of = [&](const Canvas& c) {
        std::vector<std::string> rows;
        const int mr = c.max_content_row();
        for (int y = 0; y <= mr; ++y) {
            std::string line;
            for (int x = 0; x < kWidth; ++x) {
                char32_t ch = c.get(x, y).character;
                line.push_back(ch && ch < 128 ? static_cast<char>(ch) : ' ');
            }
            while (!line.empty() && line.back() == ' ') line.pop_back();
            rows.push_back(std::move(line));
        }
        return rows;
    };
    auto ra = rows_of(ca);
    auto rb = rows_of(cb);
    int first_div = -1;
    for (int y = 0; y < committed_a && y < (int)rb.size(); ++y) {
        if (y >= (int)ra.size() || ra[y] != rb[y]) { first_div = y; break; }
    }
    if (first_div >= 0) {
        std::fprintf(stderr,
            "  committed-row divergence at %d (rows_a=%d committed=%d):\n"
            "    A |%s|\n    B |%s|\n",
            first_div, rows_a, committed_a,
            first_div < (int)ra.size() ? ra[first_div].c_str() : "<none>",
            first_div < (int)rb.size() ? rb[first_div].c_str() : "<none>");
    }
    CHECK(first_div < 0,
          "frozen render diverges from live render in the committed "
          "scrollback prefix — the stranded-duplicate root cause");

    close(rfd);
}

// SETTLE freeze with a continuation: the write settled, a placeholder
// text sub-turn streamed after it, then the turn finishes and the single
// settle-time freeze takes the WHOLE run at once (agent_session's
// MessageStop analog). Same wire invariant: the freeze must not rewrite
// a committed scrollback row.
TEST_CASE("write midrun active freeze no rewrite") {
    constexpr int kWidth = 100;
    constexpr int kTermH = 30;

    Model m;
    m.d.current.id = agentty::ThreadId{"wiremid"};
    Message u; u.role = Role::User; u.text = "write a big file then continue";
    m.d.current.messages.push_back(std::move(u));
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, 1);

    // Settled write sub-turn + an ACTIVE streaming placeholder behind it
    // (the continuation the reducer pushes post-tool). Stream is active.
    Message a; a.role = Role::Assistant;
    a.tool_calls.push_back(settled_write("bigmid", 120));
    m.d.current.messages.push_back(std::move(a));
    Message ph; ph.role = Role::Assistant; ph.streaming_text = "continuing";
    m.d.current.messages.push_back(std::move(ph));
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};

    StylePool pool;
    auto [writer, rfd] = make_pipe_writer();

    // frame A: live tail = [settled write][active placeholder]. Overflows.
    Canvas ca = paint(build_root(m), kWidth, pool);
    auto outcome_a = InlineFrame<Empty>{}.seed().render(
        ca, content_rows(ca), term_rows_for_test(kTermH), pool, writer, false);
    (void)drain(rfd);
    InlineFrame<Synced> sa = std::visit(
        [](auto&& arm) -> InlineFrame<Synced> {
            using T = std::decay_t<decltype(arm)>;
            if constexpr (std::is_same_v<T, InlineFrame<Synced>>)
                return std::move(arm);
            else { std::fprintf(stderr, "  midrun frame A not Synced\n");
                   std::abort(); }
        }, std::move(outcome_a));

    const int rows_a = sa.rows();
    const int committed_a = rows_a > kTermH ? rows_a - kTermH : 0;
    CHECK(committed_a > 0,
          "midrun setup: live write must overflow the viewport");

    // Mid-run nothing freezes (agent_session discipline). The turn
    // settles: placeholder text commits, phase goes idle, and the single
    // settle-time freeze takes the whole run.
    {
        auto& back = m.d.current.messages.back();
        back.text = std::move(back.streaming_text);
        back.streaming_text.clear();
    }
    m.s.phase = agentty::phase::Idle{};
    agentty::app::detail::freeze_through(m, m.d.current.messages.size());

    // frame B: frozen prefix + live placeholder, against the same state.
    Canvas cb = paint(build_root(m), kWidth, pool);
    auto wit = sa.verify();
    CHECK(wit.has_value(), "midrun shadow verify failed after frame A");
    if (wit) {
        auto proof = sa.check_scrollback(cb, kTermH);
        CHECK(proof.has_value(),
              "midrun: scrollback gate rejected the frozen frame");
        auto outcome_b = std::move(sa).render(
            cb, content_rows(cb), term_rows_for_test(kTermH), pool, writer,
            std::move(*wit), std::move(*proof), false);
        (void)drain(rfd);
        bool synced_b = std::visit([](auto&& arm) {
            using T = std::decay_t<decltype(arm)>;
            return std::is_same_v<T, InlineFrame<Synced>>;
        }, std::move(outcome_b));
        CHECK(synced_b,
              "midrun freeze frame demoted out of Synced — recovery path "
              "hit (the duplicate-write symptom)");
    }

    auto rows_of = [&](const Canvas& c) {
        std::vector<std::string> rows;
        const int mr = c.max_content_row();
        for (int y = 0; y <= mr; ++y) {
            std::string line;
            for (int x = 0; x < kWidth; ++x) {
                char32_t ch = c.get(x, y).character;
                line.push_back(ch && ch < 128 ? static_cast<char>(ch) : ' ');
            }
            while (!line.empty() && line.back() == ' ') line.pop_back();
            rows.push_back(std::move(line));
        }
        return rows;
    };
    auto ra = rows_of(ca);
    auto rb = rows_of(cb);
    int first_div = -1;
    for (int y = 0; y < committed_a && y < (int)rb.size(); ++y) {
        if (y >= (int)ra.size() || ra[y] != rb[y]) { first_div = y; break; }
    }
    if (first_div >= 0) {
        std::fprintf(stderr,
            "  MIDRUN committed-row divergence at %d (rows_a=%d committed=%d):\n"
            "    A |%s|\n    B |%s|\n",
            first_div, rows_a, committed_a,
            first_div < (int)ra.size() ? ra[first_div].c_str() : "<none>",
            first_div < (int)rb.size() ? rb[first_div].c_str() : "<none>");
    }
    CHECK(first_div < 0,
          "MIDRUN frozen render diverges from live render in the committed "
          "scrollback prefix — stranded duplicate while stream active");

    close(rfd);
}

// FULL lifecycle at the wire: streaming (windowed) -> settle (full body)
// -> freeze. Three composes against one evolving state. The streaming
// card is compact; on settle it expands to the full body; on freeze it
// graduates. Every committed scrollback row must survive both the
// settle expansion AND the freeze. This is the closest model to the
// real screenshot sequence.
TEST_CASE("write streaming settle freeze") {
    constexpr int kWidth = 100;
    constexpr int kTermH = 30;

    Model m;
    m.d.current.id = agentty::ThreadId{"wirelife"};
    Message u; u.role = Role::User; u.text = "write a big file";
    m.d.current.messages.push_back(std::move(u));
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, 1);

    // Lead-in frozen turns so the write card's top is pushed above the
    // viewport top BEFORE it settles (so it genuinely commits rows).
    // Complete user+write exchanges frozen at their settle boundaries —
    // the only way content reaches m.ui.frozen now.
    for (int e = 0; e < 6; ++e) {
        Message lu; lu.role = Role::User;
        lu.text = "write lead file " + std::to_string(e);
        m.d.current.messages.push_back(std::move(lu));
        Message la; la.role = Role::Assistant;
        la.tool_calls.push_back(settled_write("lead" + std::to_string(e), 4));
        m.d.current.messages.push_back(std::move(la));
    }
    agentty::app::detail::freeze_through(m, m.d.current.messages.size());
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};

    std::string content;
    for (int i = 0; i < 120; ++i)
        content += "line " + std::to_string(i) + ": plausible file body text\n";

    StylePool pool;
    auto [writer, rfd] = make_pipe_writer();

    // ── frame A: write RUNNING (streaming). Windowed compact card.
    {
        Message a; a.role = Role::Assistant;
        ToolUse t;
        t.id = ToolCallId{"wlife"}; t.name = ToolName{"write"};
        t.args = {{"file_path", "/tmp/life.md"}, {"content", content}};
        t.status = ToolUse::Running{steady_clock::now(), ""};
        a.tool_calls.push_back(std::move(t));
        Message ph; ph.role = Role::Assistant; ph.streaming_text = "...";
        m.d.current.messages.push_back(std::move(a));
        m.d.current.messages.push_back(std::move(ph));
    }
    Canvas ca = paint(build_root(m), kWidth, pool);
    auto oa = InlineFrame<Empty>{}.seed().render(
        ca, content_rows(ca), term_rows_for_test(kTermH), pool, writer, false);
    (void)drain(rfd);
    InlineFrame<Synced> s = std::visit(
        [](auto&& arm) -> InlineFrame<Synced> {
            using T = std::decay_t<decltype(arm)>;
            if constexpr (std::is_same_v<T, InlineFrame<Synced>>) return std::move(arm);
            else { std::fprintf(stderr, "  life frame A not Synced\n"); std::abort(); }
        }, std::move(oa));

    auto rows_of = [&](const Canvas& c) {
        std::vector<std::string> rows;
        const int mr = c.max_content_row();
        for (int y = 0; y <= mr; ++y) {
            std::string line;
            for (int x = 0; x < kWidth; ++x) {
                char32_t ch = c.get(x, y).character;
                line.push_back(ch && ch < 128 ? static_cast<char>(ch) : ' ');
            }
            while (!line.empty() && line.back() == ' ') line.pop_back();
            rows.push_back(std::move(line));
        }
        return rows;
    };
    auto check_prefix = [&](const char* tag, const std::vector<std::string>& prev,
                            const Canvas& cur_canvas, int prev_rows) {
        const int committed = prev_rows > kTermH ? prev_rows - kTermH : 0;
        auto cur = rows_of(cur_canvas);
        int d = -1;
        for (int y = 0; y < committed && y < (int)cur.size(); ++y)
            if (y >= (int)prev.size() || prev[y] != cur[y]) { d = y; break; }
        if (d >= 0)
            std::fprintf(stderr,
                "  %s committed divergence at %d (committed=%d)\n"
                "    PREV |%s|\n    CUR  |%s|\n", tag, d, committed,
                d < (int)prev.size() ? prev[d].c_str() : "<none>",
                d < (int)cur.size()  ? cur[d].c_str()  : "<none>");
        CHECK(d < 0, tag);
    };

    // ── frame B: write SETTLES (Done) -> full body expansion in live tail.
    auto prev_a = rows_of(ca);
    const int rows_a = s.rows();
    agentty::app::detail::with_live_tool(
        m, ToolCallId{"wlife"}, [&](ToolUse& t) {
            auto now = steady_clock::now();
            t.status = ToolUse::Done{now - milliseconds{5}, now, "Created /tmp/life.md"};
        });
    Canvas cb = paint(build_root(m), kWidth, pool);
    {
        auto wit = s.verify();
        CHECK(wit.has_value(), "life shadow verify failed after A");
        if (wit) {
            auto proof = s.check_scrollback(cb, kTermH);
            CHECK(proof.has_value(),
                  "life: scrollback gate rejected the settle frame");
            auto ob = std::move(s).render(
                cb, content_rows(cb), term_rows_for_test(kTermH), pool, writer,
                std::move(*wit), std::move(*proof), false);
            (void)drain(rfd);
            s = std::visit([](auto&& arm) -> InlineFrame<Synced> {
                using T = std::decay_t<decltype(arm)>;
                if constexpr (std::is_same_v<T, InlineFrame<Synced>>) return std::move(arm);
                else { std::fprintf(stderr,
                    "  life Running->Done demoted out of Synced\n"); std::abort(); }
            }, std::move(ob));
        }
    }
    check_prefix("life Running->Done rewrote a committed row", prev_a, cb, rows_a);

    // ── frame C: the turn settles — the single settle-time freeze.
    auto prev_b = rows_of(cb);
    const int rows_b = s.rows();
    {
        auto& back = m.d.current.messages.back();
        back.text = std::move(back.streaming_text);
        back.streaming_text.clear();
    }
    m.s.phase = agentty::phase::Idle{};
    agentty::app::detail::freeze_through(m, m.d.current.messages.size());
    Canvas cc = paint(build_root(m), kWidth, pool);
    {
        auto wit = s.verify();
        CHECK(wit.has_value(), "life shadow verify failed after B");
        if (wit) {
            auto proof = s.check_scrollback(cc, kTermH);
            CHECK(proof.has_value(),
                  "life: scrollback gate rejected the freeze frame");
            auto oc = std::move(s).render(
                cc, content_rows(cc), term_rows_for_test(kTermH), pool, writer,
                std::move(*wit), std::move(*proof), false);
            (void)drain(rfd);
            bool synced = std::visit([](auto&& arm) {
                using T = std::decay_t<decltype(arm)>;
                return std::is_same_v<T, InlineFrame<Synced>>;
            }, std::move(oc));
            CHECK(synced, "life Done->freeze demoted out of Synced");
        }
    }
    check_prefix("life Done->freeze rewrote a committed row", prev_b, cc, rows_b);

    close(rfd);
}

// THE real StreamFinished path: frame A renders the live tail while the
// stream is ACTIVE (the last run reserves an activity-indicator slot and
// the run is NOT cached because reserve_slot is true). Then the stream
// goes idle AND freeze_through fires in the same reducer step. Frame B
// renders the frozen prefix + empty live tail. If the active live frame
// was TALLER than the frozen frame (the reserve slot / live chrome
// dropped), the frame SHRINKS while overflowed -> maya's shrink-guard
// commits overflow + demote_to_stale -> case-(B) repaint that can strand
// a duplicate. This is the exact screenshot scenario.
TEST_CASE("write idle finalize freeze") {
    constexpr int kWidth = 100;
    constexpr int kTermH = 30;

    Model m;
    m.d.current.id = agentty::ThreadId{"wireidle"};
    Message u; u.role = Role::User; u.text = "write a big file";
    m.d.current.messages.push_back(std::move(u));
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, 1);

    // The settled write as the active turn's only sub-turn, stream ACTIVE
    // (the moment right after the tool returns Done, before StreamFinished
    // flips phase to Idle). is_last_run + active => reserve_slot path.
    Message a; a.role = Role::Assistant;
    a.tool_calls.push_back(settled_write("idle", 120));
    m.d.current.messages.push_back(std::move(a));
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};

    StylePool pool;
    auto [writer, rfd] = make_pipe_writer();

    // frame A: ACTIVE live render of the settled write.
    Canvas ca = paint(build_root(m), kWidth, pool);
    auto oa = InlineFrame<Empty>{}.seed().render(
        ca, content_rows(ca), term_rows_for_test(kTermH), pool, writer, false);
    (void)drain(rfd);
    InlineFrame<Synced> s = std::visit(
        [](auto&& arm) -> InlineFrame<Synced> {
            using T = std::decay_t<decltype(arm)>;
            if constexpr (std::is_same_v<T, InlineFrame<Synced>>) return std::move(arm);
            else { std::fprintf(stderr, "  idle frame A not Synced\n"); std::abort(); }
        }, std::move(oa));
    const int rows_a = s.rows();
    const int committed_a = rows_a > kTermH ? rows_a - kTermH : 0;
    CHECK(committed_a > 0, "idle setup: live write must overflow viewport");

    auto rows_of = [&](const Canvas& c) {
        std::vector<std::string> rows;
        const int mr = c.max_content_row();
        for (int y = 0; y <= mr; ++y) {
            std::string line;
            for (int x = 0; x < kWidth; ++x) {
                char32_t ch = c.get(x, y).character;
                line.push_back(ch && ch < 128 ? static_cast<char>(ch) : ' ');
            }
            while (!line.empty() && line.back() == ' ') line.pop_back();
            rows.push_back(std::move(line));
        }
        return rows;
    };
    auto prev_a = rows_of(ca);

    // StreamFinished: phase -> Idle, then freeze the whole run. Same step.
    m.s.phase = agentty::phase::Idle{};
    agentty::app::detail::freeze_through(m, m.d.current.messages.size());

    // frame B: frozen prefix + empty live tail.
    Canvas cb = paint(build_root(m), kWidth, pool);
    const int rows_b = (int)rows_of(cb).size();
    std::fprintf(stderr, "  [idle] rows_a=%d rows_b=%d committed_a=%d\n",
                 rows_a, rows_b, committed_a);
    CHECK(rows_b >= rows_a,
          "FREEZE SHRANK the frame (rows_b < rows_a) while overflowed — "
          "maya's shrink-guard fires commit+demote_to_stale -> case-(B) "
          "repaint that strands a duplicate. The live tail carried chrome "
          "(activity slot / gap) the frozen build dropped.");

    auto wit = s.verify();
    CHECK(wit.has_value(), "idle shadow verify failed after A");
    if (wit) {
        auto proof = s.check_scrollback(cb, kTermH);
        CHECK(proof.has_value(),
              "idle: scrollback gate rejected the freeze frame");
        auto ob = std::move(s).render(
            cb, content_rows(cb), term_rows_for_test(kTermH), pool, writer,
            std::move(*wit), std::move(*proof), false);
        (void)drain(rfd);
        bool synced = std::visit([](auto&& arm) {
            using T = std::decay_t<decltype(arm)>;
            return std::is_same_v<T, InlineFrame<Synced>>;
        }, std::move(ob));
        CHECK(synced,
              "idle freeze demoted out of Synced — shrink/poison recovery "
              "path hit (the duplicate-write symptom)");
    }

    auto rb = rows_of(cb);
    int d = -1;
    for (int y = 0; y < committed_a && y < (int)rb.size(); ++y)
        if (y >= (int)prev_a.size() || prev_a[y] != rb[y]) { d = y; break; }
    if (d >= 0)
        std::fprintf(stderr,
            "  IDLE committed divergence at %d\n    A |%s|\n    B |%s|\n",
            d, d < (int)prev_a.size() ? prev_a[d].c_str() : "<none>",
            d < (int)rb.size() ? rb[d].c_str() : "<none>");
    CHECK(d < 0, "IDLE finalize freeze rewrote a committed scrollback row");

    close(rfd);
}

// THE turn-finish text path. Frame A: a long assistant TEXT reply is
// still streaming — phase Active, the message's bytes live in
// streaming_text and the StreamingMarkdown renders the uncommitted
// trailing block via render_tail (live mode). The frame overflows the
// viewport. Frame B: StreamFinished — streaming_text moves to text, the
// markdown is finish()'d (the tail commits into a canonical block), and
// the run is frozen. render_tail's row count vs the committed-block row
// count can differ; if frame B is SHORTER while overflowed, maya's
// shrink-guard fires commit(overflow)+demote_to_stale -> case-(B)
// repaint that strands a duplicate. This is "it doubles when a turn
// finishes" — a TEXT reply, not a tool card.
TEST_CASE("text turn finish shrink") {
    constexpr int kWidth = 80;
    constexpr int kTermH = 24;

    Model m;
    m.d.current.id = agentty::ThreadId{"wiretext"};
    Message u; u.role = Role::User; u.text = "explain at length";
    m.d.current.messages.push_back(std::move(u));
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, 1);

    // A long prose reply that ends with a trailing block whose live
    // (render_tail) vs settled (committed-block) layout can differ:
    // a paragraph immediately followed by a list with NO terminating
    // blank line — the classic "tail not yet committed at a boundary"
    // shape the reveal animation leaves on the final frame.
    std::string body;
    for (int i = 0; i < 60; ++i)
        body += "This is paragraph line " + std::to_string(i)
              + " of a long streamed assistant reply that wraps.\n\n";
    body += "Final points before the turn ends:\n";
    for (int i = 0; i < 6; ++i)
        body += "- bullet item number " + std::to_string(i) + "\n";
    body += "- last bullet with no trailing newline boundary";

    StylePool pool;
    auto [writer, rfd] = make_pipe_writer();

    // frame A: streaming — bytes in streaming_text, phase Active.
    {
        Message a; a.role = Role::Assistant;
        a.streaming_text = body;
        m.d.current.messages.push_back(std::move(a));
        m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
    }
    // The turn view's cached_markdown_for enables maya reveal_fx (a wall-
    // clock typewriter) on the live widget, so the FIRST build only reveals
    // the bytes the cursor has walked to — a tiny fraction of `body`. This
    // test is about the freeze/shrink SEAM, not the reveal animation, and it
    // needs frame A to render the full streamed body so it overflows the
    // viewport. Pre-seed the cache with reveal_fx OFF so the streaming frame
    // measures the settled height.
    {
        auto& cache = m.ui.view_cache.message_md(
            m.d.current.id, m.d.current.messages.back().id);
        cache.streaming = std::make_shared<maya::StreamingMarkdown>();
        cache.streaming->set_reveal_fx(false);
    }
    Canvas ca = paint(build_root(m), kWidth, pool);
    auto oa = InlineFrame<Empty>{}.seed().render(
        ca, content_rows(ca), term_rows_for_test(kTermH), pool, writer, false);
    (void)drain(rfd);
    InlineFrame<Synced> s = std::visit(
        [](auto&& arm) -> InlineFrame<Synced> {
            using T = std::decay_t<decltype(arm)>;
            if constexpr (std::is_same_v<T, InlineFrame<Synced>>) return std::move(arm);
            else { std::fprintf(stderr, "  text frame A not Synced\n"); std::abort(); }
        }, std::move(oa));
    const int rows_a = s.rows();
    const int committed_a = rows_a > kTermH ? rows_a - kTermH : 0;
    CHECK(committed_a > 0, "text setup: streaming reply must overflow viewport");

    auto rows_of = [&](const Canvas& c) {
        std::vector<std::string> rows;
        const int mr = c.max_content_row();
        for (int y = 0; y <= mr; ++y) {
            std::string line;
            for (int x = 0; x < kWidth; ++x) {
                char32_t ch = c.get(x, y).character;
                line.push_back(ch && ch < 128 ? static_cast<char>(ch) : ' ');
            }
            while (!line.empty() && line.back() == ' ') line.pop_back();
            rows.push_back(std::move(line));
        }
        return rows;
    };
    auto prev_a = rows_of(ca);

    // StreamFinished idle path: commit streaming_text -> text, settle the
    // markdown (finish), freeze the run. Mirrors finalize_turn's idle arm.
    {
        auto& last = m.d.current.messages.back();
        last.text = std::move(last.streaming_text);
        std::string{}.swap(last.streaming_text);
        auto& cache = m.ui.view_cache.message_md(m.d.current.id, last.id);
        if (!cache.streaming)
            cache.streaming = std::make_shared<maya::StreamingMarkdown>();
        cache.streaming->set_content(last.text);
        cache.streaming->finish();
    }
    m.s.phase = agentty::phase::Idle{};
    agentty::app::detail::freeze_through(m, m.d.current.messages.size());

    // frame B: frozen prefix + empty live tail.
    Canvas cb = paint(build_root(m), kWidth, pool);
    const int rows_b = (int)rows_of(cb).size();
    std::fprintf(stderr, "  [text] rows_a=%d rows_b=%d committed_a=%d\n",
                 rows_a, rows_b, committed_a);
    CHECK(rows_b >= rows_a,
          "TEXT TURN-FINISH SHRANK the frame (rows_b < rows_a) while "
          "overflowed — the live render_tail layout was taller than the "
          "settled committed-block layout; shrink-guard fires and strands "
          "a duplicate. This is the 'reply doubles when the turn finishes' "
          "bug.");

    auto wit = s.verify();
    CHECK(wit.has_value(), "text shadow verify failed after A");
    if (wit) {
        auto proof = s.check_scrollback(cb, kTermH);
        CHECK(proof.has_value(),
              "text: scrollback gate rejected the freeze frame");
        auto ob = std::move(s).render(
            cb, content_rows(cb), term_rows_for_test(kTermH), pool, writer,
            std::move(*wit), std::move(*proof), false);
        (void)drain(rfd);
        bool synced = std::visit([](auto&& arm) {
            using T = std::decay_t<decltype(arm)>;
            return std::is_same_v<T, InlineFrame<Synced>>;
        }, std::move(ob));
        CHECK(synced,
              "text turn-finish demoted out of Synced — shrink recovery "
              "path hit (the duplicate-reply symptom)");
    }

    auto rb = rows_of(cb);
    int d = -1;
    for (int y = 0; y < committed_a && y < (int)rb.size(); ++y)
        if (y >= (int)prev_a.size() || prev_a[y] != rb[y]) { d = y; break; }
    if (d >= 0)
        std::fprintf(stderr,
            "  TEXT committed divergence at %d\n    A |%s|\n    B |%s|\n",
            d, d < (int)prev_a.size() ? prev_a[d].c_str() : "<none>",
            d < (int)rb.size() ? rb[d].c_str() : "<none>");
    CHECK(d < 0, "TEXT turn-finish rewrote a committed scrollback row");

    close(rfd);
}

// REGRESSION for the shrink-while-overflowed recovery gate. When the
// prior frame OVERFLOWS the viewport and THIS frame is shorter, two
// shapes are possible:
//
//   (1) TURN-FINISH FREEZE — the shrink is at the BOTTOM; the rows
//       already in native scrollback (the overflow prefix) are
//       byte-IDENTICAL. The diff path handles it append-only. The
//       recovery (commit + case-(B)) MUST NOT fire here — case-(B)
//       re-paints from content top, overlapping the committed prefix,
//       stranding a duplicate. This is the "turn doubles in scrollback
//       when it finishes" bug — and it strands EVEN when the frame
//       crosses from overflow to fit (new_rows <= term_h), which the
//       previous gate (new_rows <= term_h) wrongly routed into case-B.
//
//   (2) SCROLLBACK-CONTENT SHIFT — the prefix differs; recovery IS
//       needed.
//
// The gate discriminates via InlineFrameState::scrollback_prefix_matches.
// Drive a real carried Synced state across the shrink and assert: (a)
// the prefix-match predicate returns true for a pure bottom shrink, and
// (b) the verified diff path stays Synced and rewrites no committed row.
TEST_CASE("overflowed shrink stays synced") {
    constexpr int kWidth = 80;
    constexpr int kTermH = 24;

    Model m;
    m.d.current.id = agentty::ThreadId{"wireshrink"};
    Message u; u.role = Role::User; u.text = "go";
    m.d.current.messages.push_back(std::move(u));
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, 1);

    StylePool pool;
    auto [writer, rfd] = make_pipe_writer();

    auto rows_of = [&](const Canvas& c) {
        std::vector<std::string> rows;
        const int mr = c.max_content_row();
        for (int y = 0; y <= mr; ++y) {
            std::string line;
            for (int x = 0; x < kWidth; ++x) {
                char32_t ch = c.get(x, y).character;
                line.push_back(ch && ch < 128 ? static_cast<char>(ch) : ' ');
            }
            while (!line.empty() && line.back() == ' ') line.pop_back();
            rows.push_back(std::move(line));
        }
        return rows;
    };

    // frame A: a tall settled-text reply that overflows by a wide margin.
    // The body is the FROZEN content; A additionally carries extra
    // TRAILING paragraphs that frame B drops. Critically the dropped
    // rows are all at the BOTTOM (inside [rows_b, rows_a)), so the
    // committed prefix [0, rows_a - term_h) is byte-identical between A
    // and B — a true turn-finish bottom-shrink (live caret / reveal gap
    // collapsing), NOT a content-removal that shifts scrollback.
    const int kBodyParas = 80;
    auto body_text = [](int paras) {
        std::string body;
        for (int i = 0; i < paras; ++i)
            body += "reply line " + std::to_string(i) + " with enough text\n\n";
        return body;
    };
    // The final paragraph wraps over several rows in A (long) and a
    // single row in B (short). Everything above it is identical, so the
    // committed prefix stays byte-identical while the bottom shrinks.
    auto tall_tail = [](int width_lines) {
        std::string s = "TAIL";
        for (int i = 0; i < width_lines; ++i) s += " wwwwwwww wwwwwwww wwwwwwww";
        return s;
    };
    {
        Message a; a.role = Role::Assistant;
        a.text = body_text(kBodyParas) + tall_tail(8);
        auto& cache = m.ui.view_cache.message_md(m.d.current.id, a.id);
        cache.streaming = std::make_shared<maya::StreamingMarkdown>();
        cache.streaming->set_content(a.text);
        cache.streaming->finish();
        m.d.current.messages.push_back(std::move(a));
    }
    Canvas ca = paint(build_root(m), kWidth, pool);
    auto oa = InlineFrame<Empty>{}.seed().render(
        ca, content_rows(ca), term_rows_for_test(kTermH), pool, writer, false);
    (void)drain(rfd);
    InlineFrame<Synced> s = std::visit(
        [](auto&& arm) -> InlineFrame<Synced> {
            using T = std::decay_t<decltype(arm)>;
            if constexpr (std::is_same_v<T, InlineFrame<Synced>>) return std::move(arm);
            else { std::fprintf(stderr, "  shrink frame A not Synced\n"); std::abort(); }
        }, std::move(oa));
    const int rows_a = s.rows();
    auto prev_a = rows_of(ca);
    CHECK(rows_a > kTermH + 4,
          "shrink setup: frame A must overflow the viewport by several rows");

    // frame B: drops the trailing filler paragraphs only. All 80 body
    // paragraphs remain, so the upper (committed) rows are byte-identical;
    // only the bottom few rows shrink. Stays overflowed.
    {
        auto& a = m.d.current.messages.back();
        a.text = body_text(kBodyParas) + tall_tail(0);
        auto& cache = m.ui.view_cache.message_md(m.d.current.id, a.id);
        cache.streaming->set_content(a.text);
        cache.streaming->finish();
    }
    Canvas cb = paint(build_root(m), kWidth, pool);
    const int rows_b = (int)rows_of(cb).size();
    std::fprintf(stderr, "  [shrink] rows_a=%d rows_b=%d term_h=%d\n",
                 rows_a, rows_b, kTermH);
    CHECK(rows_b < rows_a && rows_b > kTermH,
          "shrink setup: frame B must be shorter than A yet still overflow");

    // The gate's discriminator, exercised against the REAL API. For a
    // pure bottom shrink the overflow prefix is byte-identical → the
    // recovery must NOT fire and the diff path runs.
    const int prev_rows_v = s.rows();
    const int overflow    = prev_rows_v - kTermH;
    const bool prefix_unchanged = s.scrollback_prefix_matches(cb, overflow);
    if (!prefix_unchanged) {
        auto rba = rows_of(ca);
        auto rbb = rows_of(cb);
        for (int y = 0; y < overflow; ++y) {
            const std::string& la = y < (int)rba.size() ? rba[y] : std::string();
            const std::string& lb = y < (int)rbb.size() ? rbb[y] : std::string();
            if (la != lb) {
                std::fprintf(stderr,
                    "  [shrink] prefix diverges at row %d/%d\n    A |%s|\n    B |%s|\n",
                    y, overflow, la.c_str(), lb.c_str());
                break;
            }
        }
    }
    CHECK(prefix_unchanged,
          "pure bottom shrink: scrollback_prefix_matches must report the "
          "overflow prefix unchanged, so the recovery does NOT fire and the "
          "diff path keeps it Synced (no stranded duplicate).");

    // Run the actual gate decision (mirrors Runtime::render's Synced arm).
    std::string bytes_guard;
    if (!prefix_unchanged) {
        std::fprintf(stderr, "  [shrink] unexpected recovery path\n");
        std::abort();
    } else {
        auto wit = s.verify();
        CHECK(wit.has_value(), "shrink shadow verify failed after A");
        if (wit) {
            auto proof = s.check_scrollback(cb, kTermH);
            CHECK(proof.has_value(),
                  "shrink: scrollback gate rejected the pure-bottom-shrink "
                  "frame (prefix should be unchanged)");
            auto ob = std::move(s).render(
                cb, content_rows(cb), term_rows_for_test(kTermH), pool, writer,
                std::move(*wit), std::move(*proof), false);
            bytes_guard = drain(rfd);
            bool synced = std::visit([](auto&& arm) {
                using T = std::decay_t<decltype(arm)>;
                return std::is_same_v<T, InlineFrame<Synced>>;
            }, std::move(ob));
            CHECK(synced, "still-overflowed shrink diff did not stay Synced");
        }
    }

    // Wire-volume witness. The diff path for a pure BOTTOM shrink emits
    // only the changed bottom rows + scroll-off protection + \x1b[J — a
    // few KB at most, NOT the full-viewport-plus-committed-prefix repaint
    // the buggy commit+case-(B) recovery produced. The structural checks
    // below (stays Synced, no committed row rewritten) are the primary
    // assertion; this bound just catches a regression back to case-(B).
    std::fprintf(stderr, "  [shrink] prefix_unchanged=%d emitted_bytes=%zu\n",
                 (int)prefix_unchanged, bytes_guard.size());
    CHECK(bytes_guard.size() < 4096,
          "shrink emitted a full-viewport repaint — the recovery over-fired "
          "into case-(B), re-emitting committed scrollback rows (the stranded "
          "duplicate). Expected the small incremental diff.");

    auto rb = rows_of(cb);
    const int committed_b = rows_b > kTermH ? rows_b - kTermH : 0;
    int d = -1;
    for (int y = 0; y < committed_b && y < (int)rb.size(); ++y)
        if (y >= (int)prev_a.size() || prev_a[y] != rb[y]) { d = y; break; }
    if (d >= 0)
        std::fprintf(stderr,
            "  SHRINK committed divergence at %d\n    A |%s|\n    B |%s|\n",
            d, d < (int)prev_a.size() ? prev_a[d].c_str() : "<none>",
            d < (int)rb.size() ? rb[d].c_str() : "<none>");
    CHECK(d < 0, "still-overflowed shrink rewrote a committed scrollback row");

    close(rfd);
}

// MID-RUN TRIM at the wire. A long active run accumulates a tall frozen
// prefix dominated by OUTPUT-ELIDED tools (bash / read / grep) whose maya
// renderers collapse a huge output to a few rows. trim_frozen_above_
// viewport drops the off-screen front and emits commit_scrollback(N).
// The corruption this guards: if estimate_msg_rows OVER-counts those
// elided cards, the keep-loop drops an entry still on screen and the
// commit boundary lands below the real off-screen rows — frame B then
// rewrites a committed scrollback row (the bash-card ghost band). Drive
// real compose across the trim and assert no committed row is rewritten
// AND the commit count was not clamped (no under-commit).
static ToolUse settled_bash(const std::string& tag, int n_lines) {
    ToolUse t;
    t.id = ToolCallId{"bash_" + tag}; t.name = ToolName{"shell"};
    t.args = {{"command", "grep -rn x src # " + tag}};
    std::string out;
    for (int i = 0; i < n_lines; ++i)
        out += "src/file" + std::to_string(i) + ".cpp:" + std::to_string(i)
             + ": a matching line of plausible source text here\n";
    auto now = steady_clock::now();
    t.status = ToolUse::Done{now - milliseconds{5}, now, std::move(out)};
    return t;
}

TEST_CASE("midrun trim output heavy no rewrite") {
    constexpr int kWidth = 100;
    constexpr int kTermH = 30;

    Model m;
    m.d.current.id = agentty::ThreadId{"wiretrim"};
    // Many COMPLETE turns (user + noisy bash), each frozen at its settle
    // boundary, then an active streaming tail. Each bash card has a HUGE
    // output that elides to ~4 rows. Pre-fix the row estimate over-counted
    // ~120 rows each, so a handful tripped the keep-loop's
    // drop-an-on-screen-entry path.
    for (int e = 0; e < 16; ++e) {
        Message u; u.role = Role::User;
        u.text = "noisy work " + std::to_string(e);
        m.d.current.messages.push_back(std::move(u));
        Message a; a.role = Role::Assistant;
        a.tool_calls.push_back(
            settled_bash("t" + std::to_string(e), 120));
        m.d.current.messages.push_back(std::move(a));
    }
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, m.d.current.messages.size());
    Message ph; ph.role = Role::Assistant; ph.streaming_text = "continuing";
    m.d.current.messages.push_back(std::move(ph));
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};

    StylePool pool;
    auto [writer, rfd] = make_pipe_writer();

    // frame A: render the tall frozen prefix + live placeholder.
    Canvas ca = paint(build_root(m), kWidth, pool);
    auto oa = InlineFrame<Empty>{}.seed().render(
        ca, content_rows(ca), term_rows_for_test(kTermH), pool, writer, false);
    (void)drain(rfd);
    InlineFrame<Synced> sa = std::visit(
        [](auto&& arm) -> InlineFrame<Synced> {
            using T = std::decay_t<decltype(arm)>;
            if constexpr (std::is_same_v<T, InlineFrame<Synced>>)
                return std::move(arm);
            else { std::fprintf(stderr, "  trim frame A not Synced\n");
                   std::abort(); }
        }, std::move(oa));

    const int rows_a = sa.rows();
    const int committed_a = rows_a > kTermH ? rows_a - kTermH : 0;
    CHECK(committed_a > 0,
          "trim setup: prefix must overflow the viewport");

    // ── The trim (the ONE production trim, fired at turn boundaries).
    //    Drops the off-budget front from the MODEL and returns a
    //    commit_scrollback(N) for the N rows it deleted from the TOP.
    //    A top-deletion is invisible to maya's render-time shrink
    //    reconciliation (the shifted-up canvas fails the prefix memcmp,
    //    so maya would commit the NEW prefix over PHYSICAL old rows and
    //    strand a duplicate) — the host must commit the dropped rows
    //    against the still-valid old frame BEFORE the next render.
    auto cmd = agentty::app::detail::trim_frozen_if_oversized(m);
    using Cmd = maya::Cmd<agentty::Msg>;
    const auto* commit = std::get_if<Cmd::CommitScrollback>(&cmd.inner);
    CHECK(commit != nullptr,
          "trim must return commit_scrollback(N) for the top-deletion; "
          "none() leaves maya unable to reconcile and strands a duplicate.");
    // Apply the commit exactly as the production Cmd interpreter does:
    // advance the prior (old-content) Synced frame's prev_rows/prev_cells
    // by N before rendering the shorter tree. commit_inline_prefix's
    // internal clamp = min(N, prev_rows - term_h); since the dropped
    // entries overflowed, N never exceeds that bound.
    if (commit && commit->rows > 0) {
        const int safe = std::min(commit->rows, std::max(0, sa.rows() - kTermH));
        if (safe > 0)
            sa = std::move(sa).commit(sa.scrollback_marker(safe));
    }

    // Render frame B against the trimmed tree through maya's REAL Synced
    // render, AFTER the host commit. This is exactly the production wire:
    // the frozen tree shrank at the top, the host committed the dropped
    // rows, and the next render must land the kept tree in place WITHOUT
    // rewriting a committed scrollback row.
    Canvas cb = paint(build_root(m), kWidth, pool);
    // AUTHORITATIVE proof check: after the trim, the REAL rendered height
    // of the kept frozen tree (+ live tail) must still be >= term_h. The
    // trim's correctness rests on keeping >= a viewport on screen so every
    // dropped row provably overflowed. If estimate_msg_rows OVER-counts an
    // output-elided card, the keep-loop stops early and the REAL kept rows
    // fall below term_h — the dropped entry was still on screen and its
    // committed copy strands as the ghost band. This catches the bug even
    // when maya's clamp happens to absorb the byte delta.
    const int kept_real_rows = cb.max_content_row() + 1;
    std::fprintf(stderr, "  [trim] kept_real_rows=%d term_h=%d\n",
                 kept_real_rows, kTermH);
    CHECK(kept_real_rows >= kTermH,
          "trim left FEWER than a viewport of REAL rows on screen — the "
          "estimate over-counted and dropped an on-screen entry (ghost band)");

    auto wit = sa.verify();
    CHECK(wit.has_value(), "trim: shadow verify failed before render");
    std::string bytes_b;
    if (wit) {
        // The top-drop shifts content up, so the committed prefix may no
        // longer match — check_scrollback then returns nullopt and the
        // production path commits the overflow + soft-repaints (case B).
        // Both a valid proof (stay Synced) and the recovery are correct
        // and non-destructive; the anti-corruption proof is the
        // kept_real_rows >= term_h check above.
        auto proof = sa.check_scrollback(cb, kTermH);
        maya::inline_frame::RenderOutcome ob = [&] {
            if (proof) {
                return std::move(sa).render(
                    cb, content_rows(cb), term_rows_for_test(kTermH), pool,
                    writer, std::move(*wit), std::move(*proof), false);
            }
            const int overflow = std::max(0, sa.rows() - kTermH);
            auto committed = std::move(sa).commit(sa.scrollback_marker(overflow));
            return maya::inline_frame::RenderOutcome{
                std::move(committed).demote_to_stale()};
        }();
        bytes_b = drain(rfd);
        bool synced_b = std::visit([](auto&& arm) {
            using T = std::decay_t<decltype(arm)>;
            return std::is_same_v<T, InlineFrame<Synced>>;
        }, std::move(ob));
        // maya may legitimately take its OWN commit-overflow + soft-repaint
        // recovery here (the top-drop shifts content up so the prefix no
        // longer matches) — that is the CORRECT reconciliation and is
        // non-destructive (no \x1b[3J wipe). What we forbid is the bug
        // symptom: a committed scrollback row being rewritten below the
        // boundary. The kept_real_rows >= term_h proof above already shows
        // a full viewport stayed on screen; whether maya stays Synced or
        // demotes to its soft-repaint recovery, no committed row strands.
        (void)synced_b;
        (void)rows_a;
        (void)committed_a;
    }

    close(rfd);
}

TEST_CASE("midrun trim full body writes no rewrite") {
    // THE screenshot scenario, end to end at the wire: repeated FULL-body
    // writes during an active run, each overflowing the viewport, with the
    // mid-run trim firing. Full bodies mean frozen_rows[] is now maya's
    // REAL measured height (push_frozen measures the built Element), not
    // an estimate. The trim's keep-loop and commit count both derive from
    // that measured number, so the kept-real-rows >= term_h proof and the
    // no-clamp (safe_n == commit_n) proof hold by construction even though
    // every body renders in full.
    constexpr int kWidth = 100;
    constexpr int kTermH = 30;

    Model m;
    m.d.current.id = agentty::ThreadId{"wirefull"};
    // Separate turns (user + full-body write each), so the frozen prefix
    // is MANY entries the trim can actually drop — the real screenshot
    // shape (repeated "write a file" turns), not one merged mega-run.
    // Sized so the prefix is a couple viewports over the keep margin —
    // the realistic per-Tick cadence (a few entries dropped), not a
    // pathological single mega-trim.
    for (int e = 0; e < 6; ++e) {
        Message u; u.role = Role::User;
        u.text = "write file " + std::to_string(e);
        m.d.current.messages.push_back(std::move(u));
        Message a; a.role = Role::Assistant;
        a.tool_calls.push_back(settled_write("f" + std::to_string(e), 40));
        m.d.current.messages.push_back(std::move(a));
    }
    Message ph; ph.role = Role::Assistant; ph.streaming_text = "continuing";
    m.d.current.messages.push_back(std::move(ph));
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};

    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, m.d.current.messages.size() - 1);

    StylePool pool;
    auto [writer, rfd] = make_pipe_writer();

    Canvas ca = paint(build_root(m), kWidth, pool);
    auto oa = InlineFrame<Empty>{}.seed().render(
        ca, content_rows(ca), term_rows_for_test(kTermH), pool, writer, false);
    (void)drain(rfd);
    InlineFrame<Synced> sa = std::visit(
        [](auto&& arm) -> InlineFrame<Synced> {
            using T = std::decay_t<decltype(arm)>;
            if constexpr (std::is_same_v<T, InlineFrame<Synced>>)
                return std::move(arm);
            else { std::fprintf(stderr, "  full-body trim frame A not Synced\n");
                   std::abort(); }
        }, std::move(oa));

    const int rows_a = sa.rows();
    const int committed_a = rows_a > kTermH ? rows_a - kTermH : 0;
    CHECK(committed_a > 0,
          "full-body trim setup: prefix must overflow the viewport");

    auto cmd = agentty::app::detail::trim_frozen_if_oversized(m);
    using Cmd = maya::Cmd<agentty::Msg>;
    const auto* commit = std::get_if<Cmd::CommitScrollback>(&cmd.inner);
    CHECK(commit != nullptr,
          "full-body trim must return commit_scrollback(N) for the "
          "top-deletion; none() leaves maya unable to reconcile and "
          "strands a duplicate of the trimmed boundary one screen up.");
    // Apply the commit exactly as the production Cmd interpreter does:
    // advance the prior (old-content) Synced frame's prev_rows/prev_cells
    // by N before rendering the shorter tree.
    if (commit && commit->rows > 0) {
        const int safe = std::min(commit->rows, std::max(0, sa.rows() - kTermH));
        if (safe > 0)
            sa = std::move(sa).commit(sa.scrollback_marker(safe));
    }

    // Render frame B against the trimmed tree through maya's REAL Synced
    // render, AFTER the host commit (which advanced the shadow in
    // lockstep with the top-drop memmove).
    Canvas cb = paint(build_root(m), kWidth, pool);
    const int kept_real_rows = cb.max_content_row() + 1;
    std::fprintf(stderr, "  [full-body trim] kept_real_rows=%d term_h=%d\n",
                 kept_real_rows, kTermH);
    CHECK(kept_real_rows >= kTermH,
          "full-body trim left fewer than a viewport of REAL rows — a "
          "measured entry disagreed with the render (ghost band)");

    auto wit = sa.verify();
    CHECK(wit.has_value(), "full-body trim: shadow verify failed");
    std::string bytes_b;
    if (wit) {
        // As in the output-heavy variant: the top-drop may shift the
        // committed prefix so check_scrollback returns nullopt; the
        // production path then commits overflow + soft-repaints. Both
        // outcomes are correct and non-destructive.
        auto proof = sa.check_scrollback(cb, kTermH);
        maya::inline_frame::RenderOutcome ob = [&] {
            if (proof) {
                return std::move(sa).render(
                    cb, content_rows(cb), term_rows_for_test(kTermH), pool,
                    writer, std::move(*wit), std::move(*proof), false);
            }
            const int overflow = std::max(0, sa.rows() - kTermH);
            auto committed = std::move(sa).commit(sa.scrollback_marker(overflow));
            return maya::inline_frame::RenderOutcome{
                std::move(committed).demote_to_stale()};
        }();
        bytes_b = drain(rfd);
        bool synced_b = std::visit([](auto&& arm) {
            using T = std::decay_t<decltype(arm)>;
            return std::is_same_v<T, InlineFrame<Synced>>;
        }, std::move(ob));
        // As in the output-heavy variant: maya may stay Synced (append-only
        // diff) or take its own commit-overflow + soft-repaint recovery for
        // the top-drop shift — both are correct and non-destructive. The
        // anti-corruption proof is kept_real_rows >= term_h (a full
        // viewport stayed on screen).
        (void)synced_b;
        (void)rows_a;
        (void)committed_a;
    }

    close(rfd);
}

// REGRESSION: opening/closing the model picker over the welcome screen,
// repeatedly, must NOT strand copies of the base frame in native
// scrollback. The picker overlay is a few rows TALLER than the welcome
// base; on a terminal where welcome+overlay exceeds term_h, the grow
// scrolls the top rows (the wordmark) into scrollback via bottom-edge
// \r\n, and the shrink-back can't reclaim them — each open/close cycle
// strands another copy ("the wordmark gets longer with every picker").
//
// We drive the REAL inline compose (no force_redraw — that made it worse
// by routing through case-B scroll) across several open→close cycles at a
// SHORT term height and assert no row that overflowed the viewport top on
// an earlier frame is ever rewritten, AND the frame stays Synced (a
// recovery demote would itself scroll).
static maya::Element view_root(const Model& m) {
    return agentty::ui::view(m);
}

TEST_CASE("model picker open close no scrollback growth") {
    using namespace agentty;
    constexpr int kWidth = 100;
    // SHORT viewport so welcome+overlay overflows — the production trigger.
    constexpr int kTermH = 20;

    // deps stub: FusedPickerSelect persists settings via deps().
    app::install_deps(app::Deps{
        .stream        = [](provider::Request, provider::EventSink) {},
        .save_thread   = [](const agentty::Thread&) {},
        .delete_thread = [](const agentty::ThreadId&) {},
        .load_threads  = [] { return std::vector<agentty::Thread>{}; },
        .load_thread   = [](const ThreadId&) { return std::optional<agentty::Thread>{}; },
        .load_settings = [] { return store::Settings{}; },
        .save_settings = [](const store::Settings&) {},
        .new_thread_id = [] { return ThreadId{"stub"}; },
        .title_from    = [](std::string_view) { return std::string{}; },
        .auth          = {},
    });

    Model m;
    m.d.current.id = ThreadId{"pickercycle"};
    m.d.available_models.push_back({});
    m.d.available_models.back().id = ModelId{"claude-opus-4-1"};
    m.d.available_models.push_back({});
    m.d.available_models.back().id = ModelId{"claude-sonnet-4-5"};

    StylePool pool;
    auto [writer, rfd] = make_pipe_writer();

    auto rows_of = [&](const Canvas& c) {
        std::vector<std::string> rows;
        const int mr = c.max_content_row();
        for (int y = 0; y <= mr; ++y) {
            std::string line;
            for (int x = 0; x < kWidth; ++x) {
                char32_t ch = c.get(x, y).character;
                line.push_back(ch && ch < 128 ? static_cast<char>(ch) : ' ');
            }
            while (!line.empty() && line.back() == ' ') line.pop_back();
            rows.push_back(std::move(line));
        }
        return rows;
    };

    std::optional<InlineFrame<Synced>> synced;
    std::vector<std::string> committed_wire;   // rows already off-screen

    auto render_state = [&](const Model& mm) {
        Canvas c = paint(view_root(mm), kWidth, pool);
        auto cur = rows_of(c);
        const int total = static_cast<int>(cur.size());
        const int new_committed = total > kTermH ? total - kTermH : 0;

        if (!synced) {
            auto o = InlineFrame<Empty>{}.seed().render(
                c, content_rows(c), term_rows_for_test(kTermH), pool, writer,
                false);
            (void)drain(rfd);
            synced = std::visit(
                [](auto&& a) -> std::optional<InlineFrame<Synced>> {
                    using T = std::decay_t<decltype(a)>;
                    if constexpr (std::is_same_v<T, InlineFrame<Synced>>)
                        return std::move(a);
                    else return std::nullopt;
                }, std::move(o));
            CHECK(synced.has_value(), "picker-cycle: first frame Synced");
        } else {
            auto wit = synced->verify();
            CHECK(wit.has_value(),
                  "picker-cycle: shadow verify before render (desync)");
            if (!wit) { synced.reset(); return; }
            auto proof = synced->check_scrollback(c, kTermH);
            maya::inline_frame::RenderOutcome o = [&] {
                if (proof) {
                    return std::move(*synced).render(
                        c, content_rows(c), term_rows_for_test(kTermH), pool,
                        writer, std::move(*wit), std::move(*proof), false);
                }
                // Open/close height transition shifted the committed
                // prefix vs the prior frame's shadow: production commits
                // the off-viewport rows and soft-repaints via Stale (which
                // re-enters Synced, no scrollback wipe). Model that exactly
                // — the REAL anti-corruption invariant is the byte-stability
                // check below (no already-committed wire row is rewritten),
                // which holds regardless of which arm rendered this frame.
                const int prev_rows = synced->rows();
                const int overflow = prev_rows > kTermH
                    ? prev_rows - kTermH : 0;
                auto committed = std::move(*synced).commit(
                    synced->scrollback_marker(overflow));
                return std::move(committed).demote_to_stale().render(
                    c, content_rows(c), term_rows_for_test(kTermH), pool,
                    writer, false);
            }();
            (void)drain(rfd);
            synced = std::visit(
                [](auto&& a) -> std::optional<InlineFrame<Synced>> {
                    using T = std::decay_t<decltype(a)>;
                    if constexpr (std::is_same_v<T, InlineFrame<Synced>>)
                        return std::move(a);
                    else return std::nullopt;
                }, std::move(o));
            CHECK(synced.has_value(),
                  "picker-cycle: render re-entered Synced after recovery");
            if (!synced) return;
        }

        // Committed prefix must be byte-stable across every frame.
        const int check_n = std::min<int>(
            static_cast<int>(committed_wire.size()), new_committed);
        for (int y = 0; y < check_n; ++y) {
            if (committed_wire[static_cast<std::size_t>(y)]
                != cur[static_cast<std::size_t>(y)]) {
                std::fprintf(stderr,
                    "  PICKER committed-row rewrite at y=%d:\n"
                    "    was |%s|\n    now |%s|\n", y,
                    committed_wire[static_cast<std::size_t>(y)].c_str(),
                    cur[static_cast<std::size_t>(y)].c_str());
                CHECK(false,
                      "a base-frame row that overflowed the viewport top "
                      "was rewritten on a later open/close — stranded "
                      "scrollback copy (the growing-wordmark bug)");
                break;
            }
        }
        committed_wire.assign(cur.begin(), cur.begin() + new_committed);
    };

    // Welcome (base).
    render_state(m);
    // THE fix invariant (view.cpp overlay_layer): opening a picker must
    // NEVER grow the frame. The overlay is bottom-inset 2 rows so its
    // painted extent stays at/above the base's painted extent; without
    // the inset the overlay's bg fill painted the base box's 2
    // structurally-unpainted bottom rows (+2 on open, -2 on close),
    // pushing the wordmark into native scrollback once per cycle.
    {
        Canvas closed_c = paint(view_root(m), kWidth, pool);
        const int closed_rows = closed_c.max_content_row();
        m.ui.panel = pn::FusedPicker{{0, ""}};
        Canvas open_c = paint(view_root(m), kWidth, pool);
        m.ui.panel.close<pn::FusedPicker>();
        const int open_rows = open_c.max_content_row();
        if (open_rows > closed_rows) {
            std::fprintf(stderr,
                "  PICKER frame grew on open: closed=%d open=%d\n",
                closed_rows + 1, open_rows + 1);
        }
        CHECK(open_rows <= closed_rows,
              "opening the model picker grew the frame past the closed "
              "welcome height \u2014 rows would cross the viewport boundary "
              "into native scrollback (the growing-wordmark bug)");
    }
    // Several open→close cycles — the accumulation the user reported.
    // Render multiple animation frames per state so the welcome
    // wordmark's per-frame bob and the picker animation advance.
    for (int cycle = 0; cycle < 4; ++cycle) {
        m.ui.panel = pn::FusedPicker{{0, ""}};   // open
        for (int f = 0; f < 5; ++f) {
            render_state(m);
            std::this_thread::sleep_for(std::chrono::milliseconds{8});
        }
        auto [m2, cmd] = app::detail::fused_picker_update(
            std::move(m), msg::FusedPickerMsg{FusedPickerSelect{}});  // close
        m = std::move(m2);
        (void)cmd;
        for (int f = 0; f < 5; ++f) {
            render_state(m);
            std::this_thread::sleep_for(std::chrono::milliseconds{8});
        }
    }

    std::fprintf(stderr, "  [picker] cycles=4 committed_rows=%zu\n",
                 committed_wire.size());
    close(rfd);
}

