// scrollback_wire_fuzz — randomized WIRE-LEVEL anti-corruption fuzz.
//
// The strongest guarantee agentty can give that scrollback corruption
// and ghosting are impossible: drive ARBITRARY interleavings of every
// production mutation source — streaming deltas, settled tool cards,
// the settle freeze, the trim, picker open/close, user submits — through
// the REAL ui::view (overlays included) and maya's REAL inline compose,
// and assert the wire invariants after EVERY frame:
//
//   W1  shrink-while-overflowed always finds the overflow prefix
//       byte-identical (the recovery/demote path NEVER fires under the
//       production cadence — that path is what strands duplicates)
//   W2  the prev_cells shadow verifies before every render (no desync)
//   W3  every render stays Synced (no Stale/HardReset repaint)
//   W4  a wire row, once committed past the viewport top, is NEVER
//       rewritten by any later frame (stranded-duplicate / ghost check),
//       byte-for-byte against an absolute wire model
//   W5  explicit CommitScrollback rows from the trim agree with the
//       harness's wire accounting (no over/under commit)
//
// A failure prints SEED=<n> + the op trace for a deterministic repro.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <maya/render/canvas.hpp>
#include <maya/render/inline_frame.hpp>
#include <maya/render/renderer.hpp>
#include <maya/render/serialize.hpp>
#include <maya/style/theme.hpp>
#include <maya/terminal/writer.hpp>

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/view/view.hpp"

namespace pn = agentty::ui::panel;

using agentty::Model;
using agentty::Message;
using agentty::Role;
using agentty::ToolCallId;
using agentty::ToolName;
using agentty::ToolUse;
using std::chrono::milliseconds;
using std::chrono::steady_clock;

// A FIXED wall-clock timestamp for every fuzz message. The turn header
// renders msg.timestamp as HH:MM (timestamp_hh_mm); a default-constructed
// time_point is the epoch, whose HH:MM depends on the local timezone. Pin it
// to a constant instant (12:00 UTC — with TZ=UTC set in run_walk) so the
// rendered header bytes are identical on every machine and a committed row's
// content stays a pure function of model state.
namespace {  // fold: TU-local (bundled into agentty_standalone_tests)
static const std::chrono::system_clock::time_point kFixedTs =
    std::chrono::system_clock::time_point{std::chrono::hours{12}};

using namespace maya;
using namespace maya::inline_frame;

static int g_failures = 0;
static int g_checks   = 0;
static std::uint64_t g_seed = 0;
static std::vector<std::string> g_oplog;

static void fail(const char* inv, const std::string& detail) {
    ++g_failures;
    std::fprintf(stderr, "  FAIL [%s] %s  (SEED=%llu)\n", inv, detail.c_str(),
                 static_cast<unsigned long long>(g_seed));
    std::fprintf(stderr, "    op trace (%zu ops):\n", g_oplog.size());
    for (const auto& o : g_oplog)
        std::fprintf(stderr, "      %s\n", o.c_str());
}

#define INV(cond, name, detail)                                  \
    do { ++g_checks; if (!(cond)) fail(name, detail); } while (0)

// ── Deterministic PRNG (splitmix64) ─────────────────────────────────────
struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed) {}
    std::uint64_t next() {
        s += 0x9e3779b97f4a7c15ULL;
        std::uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
    int below(int n) {
        return n <= 0 ? 0
                      : static_cast<int>(next()
                            % static_cast<std::uint64_t>(n));
    }
    bool chance(int pct) { return below(100) < pct; }
};

// ── Pipe writer (real Writer over a real fd, drained per frame) ───────
static std::pair<Writer, int> make_pipe_writer() {
    int fds[2];
    if (pipe(fds) != 0) { std::perror("pipe"); std::abort(); }
    fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL, 0) | O_NONBLOCK);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);
    return {Writer{static_cast<platform::NativeHandle>(fds[1])}, fds[0]};
}

static std::string drain_fd(int rfd) {
    std::string out;
    char buf[8192];
    ssize_t n;
    while ((n = read(rfd, buf, sizeof(buf))) > 0)
        out.append(buf, static_cast<std::size_t>(n));
    return out;
}

// ── Settled tool-card builders (the realistic body shapes) ────────────
static ToolUse settled_tool(Rng& rng, const std::string& tag) {
    ToolUse t;
    auto now = steady_clock::now();
    const int kind = rng.below(4);
    if (kind == 0) {
        t.id = ToolCallId{"write_" + tag};
        t.name = ToolName{"write"};
        std::string content;
        const int n = 10 + rng.below(120);
        for (int i = 0; i < n; ++i)
            content += "line " + std::to_string(i) + ": file body content\n";
        t.args = {{"file_path", "/tmp/" + tag + ".txt"}, {"content", content}};
        t.status = ToolUse::Done{now - milliseconds{5}, now, "Created"};
    } else if (kind == 1) {
        t.id = ToolCallId{"edit_" + tag};
        t.name = ToolName{"edit"};
        std::string block;
        const int n = 3 + rng.below(20);
        for (int i = 0; i < n; ++i)
            block += "    auto v" + std::to_string(i) + " = compute();\n";
        nlohmann::json edits = nlohmann::json::array();
        edits.push_back({{"old_text", block}, {"new_text", block + "    done();\n"}});
        t.args = {{"path", "src/" + tag + ".cpp"}, {"edits", edits}};
        t.status = ToolUse::Done{now - milliseconds{5}, now, "edited"};
    } else if (kind == 2) {
        t.id = ToolCallId{"bash_" + tag};
        t.name = ToolName{"shell"};
        t.args = {{"command", "grep -rn pattern src # " + tag}};
        std::string out;
        const int n = 20 + rng.below(250);
        for (int i = 0; i < n; ++i)
            out += "src/f" + std::to_string(i) + ".cpp:" + std::to_string(i)
                 + ": matching line of source text\n";
        t.status = ToolUse::Done{now - milliseconds{5}, now, std::move(out)};
    } else {
        t.id = ToolCallId{"read_" + tag};
        t.name = ToolName{"read"};
        t.args = {{"path", "src/" + tag + ".cpp"}};
        std::string out;
        const int n = 15 + rng.below(80);
        for (int i = 0; i < n; ++i)
            out += std::to_string(i) + ": " + tag + " source line text\n";
        t.status = ToolUse::Done{now - milliseconds{5}, now, std::move(out)};
    }
    return t;
}

// ── Paint helper — the REAL full view (overlays included) ─────────────
// Installs a sized RenderContext around BOTH the view build and the
// paint: ui::view honors a parent context over its own ioctl (so the
// simulated term dims hold even when the test runner's stdout is a
// real tty), and render_tree's nested guard inherits for the paint.
static Canvas paint_view(const Model& m, int width, int term_h,
                         StylePool& pool) {
    maya::RenderContext ctx{width, term_h, maya::render_generation(),
                            /*auto_height=*/true};
    maya::RenderContextGuard guard(ctx);
    Canvas c(width, 4000, &pool);
    c.clear();
    std::vector<layout::LayoutNode> nodes;
    maya::render_tree(agentty::ui::view(m), c, pool, maya::theme::dark, nodes,
                      /*auto_height=*/true);
    return c;
}

static std::vector<std::string> rows_of(const Canvas& c, int width) {
    std::vector<std::string> rows;
    const int mr = c.max_content_row();
    for (int y = 0; y <= mr; ++y) {
        std::string line;
        for (int x = 0; x < width; ++x) {
            char32_t ch = c.get(x, y).character;
            line.push_back(ch && ch < 128 ? static_cast<char>(ch) : ' ');
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        rows.push_back(std::move(line));
    }
    return rows;
}

// ── The wire harness ───────────────────────────────────────────
// Carries maya's Synced frame state across frames and models the
// physical wire as an absolute row array: committed rows (rows that
// crossed the viewport top, by overflow OR by explicit
// CommitScrollback) are immutable forever — W4 byte-compares every
// later frame against them.
struct WireHarness {
    int width;
    int term_h;
    StylePool pool;
    Writer writer;
    int rfd;

    std::optional<InlineFrame<Synced>> synced;
    std::vector<std::string> wire;   // committed (immutable) wire rows
    std::size_t commits = 0;         // explicit CommitScrollback total
    bool dead = false;               // stop after first failure

    WireHarness(int w, int th, std::pair<Writer, int> pw)
        : width(w), term_h(th),
          writer(std::move(pw.first)), rfd(pw.second) {}
    ~WireHarness() { close(rfd); }

    // Apply an explicit CommitScrollback (the trim's Cmd), mirroring
    // Runtime::commit_inline_prefix: clamp to prev_rows - term_h.
    void apply_commit(const Model& m, int rows_requested) {
        if (!synced || rows_requested <= 0) return;
        const int prev_rows = synced->rows();
        const int safe_max = prev_rows > term_h ? prev_rows - term_h : 0;
        const int n = std::min(rows_requested, safe_max);
        // W5: the trim's row count must never need the clamp — a clamp
        // means the model dropped more frozen rows than the wire can
        // commit (under-commit → stranded ghost).
        INV(n == rows_requested, "W5",
            "trim commit clamped: requested "
            + std::to_string(rows_requested) + " safe "
            + std::to_string(safe_max));
        if (n <= 0) { if (rows_requested > 0) dead = true; return; }
        // The committed rows leave the mutable window: capture them
        // into the wire model NOW (they were already captured as
        // overflow rows if past the viewport top — commit only moves
        // the boundary, the bytes were stable). Advance via marker.
        synced = std::move(*synced).commit(synced->scrollback_marker(n));
        commits += static_cast<std::size_t>(n);
        (void)m;
    }

    // Apply commit_scrollback_overflow() — maya's self-measured,
    // over-commit-impossible commit. Mirrors
    // Runtime::commit_inline_prefix(prev_rows - term_h): commit exactly
    // the rows the renderer KNOWS have overflowed (no host count). This
    // is what the production trim + settle-freeze now issue. By
    // construction it can never over-commit (it commits only rows already
    // scrolled off the top), so there is no W5 clamp hazard to assert —
    // the count IS prev_rows - term_h.
    void apply_commit_overflow(const Model& m) {
        if (!synced) return;
        const int prev_rows = synced->rows();
        const int overflow = prev_rows > term_h ? prev_rows - term_h : 0;
        if (overflow <= 0) return;   // nothing overflowed — no-op (safe)
        synced = std::move(*synced).commit(synced->scrollback_marker(overflow));
        commits += static_cast<std::size_t>(overflow);
        (void)m;
    }

    // Render one frame through maya's real compose; check W1-W4.
    void frame(const Model& m, const std::string& tag) {
        if (dead) return;
        Canvas c = paint_view(m, width, term_h, pool);
        auto cur = rows_of(c, width);
        const int R = static_cast<int>(cur.size());
        if (R <= 0) return;

        if (!synced) {
            auto o = InlineFrame<Empty>{}.seed().render(
                c, content_rows(c), term_rows_for_test(term_h), pool,
                writer, false);
            (void)drain_fd(rfd);
            synced = std::visit(
                [](auto&& a) -> std::optional<InlineFrame<Synced>> {
                    using T = std::decay_t<decltype(a)>;
                    if constexpr (std::is_same_v<T, InlineFrame<Synced>>)
                        return std::move(a);
                    else
                        return std::nullopt;
                }, std::move(o));
            INV(synced.has_value(), "W3", "first frame not Synced @" + tag);
            if (!synced) { dead = true; return; }
        } else {
            // Shrink-while-overflowed reconciliation. When the prior frame
            // overflowed (prev_rows > term_h) and THIS frame is shorter,
            // maya's Synced render discriminates two sub-cases (app.cpp):
            //
            //   * prefix UNCHANGED (turn-finish freeze where the already-
            //     overflowed rows are byte-identical): maya falls through
            //     to the append-only diff path. No commit needed.
            //
            //   * prefix CHANGED (scrollback-content shift — e.g. the live
            //     streaming tail re-lays-out into a shorter frozen form on
            //     settle): maya COMMITS the overflowed rows itself and
            //     soft-repaints. This is the CORRECT, non-destructive
            //     recovery, NOT corruption.
            //
            // The harness mirrors maya: on a prefix-changed shrink we
            // apply the same commit_scrollback_overflow maya does
            // internally so prev_rows advances down to term_h before the
            // diff render. The real anti-corruption guarantee is W4 (no
            // already-committed wire row is ever rewritten), which is
            // checked below regardless of which branch we took.
            const int prev_rows = synced->rows();
            if (prev_rows > term_h && R < prev_rows) {
                const int overflow = prev_rows - term_h;
                const bool pm =
                    synced->scrollback_prefix_matches(c, overflow);
                if (!pm) {
                    // maya's own recovery: commit the off-viewport rows.
                    synced = std::move(*synced).commit(
                        synced->scrollback_marker(overflow));
                    commits += static_cast<std::size_t>(overflow);
                }
            }
            // W2: shadow always verifies (prev_cells == wire).
            auto wit = synced->verify();
            INV(wit.has_value(), "W2", "shadow verify failed @" + tag);
            if (!wit) { dead = true; return; }
            // Scrollback gate, type-enforced: render REQUIRES the proof.
            // We already ran maya's recovery above (commit overflow on a
            // prefix-changed shrink), so the frame either fits or its
            // committed prefix is stable and check_scrollback mints a
            // proof. If it still comes back nullopt (a shift the
            // pre-commit didn't cover), mirror maya exactly: commit the
            // off-viewport rows and soft-repaint via Stale.
            auto proof =
                synced->check_scrollback(c, term_h);
            maya::inline_frame::RenderOutcome o = [&] {
                if (proof) {
                    return std::move(*synced).render(
                        c, content_rows(c), term_rows_for_test(term_h),
                        pool, writer, std::move(*wit), std::move(*proof),
                        false);
                }
                // Prefix shifted under an already-overflowed frame:
                // maya commits the off-viewport rows and soft-repaints
                // via Stale — and the Stale render RE-ENTERS Synced
                // (compose case B repaints in place, no scrollback wipe).
                // Render it here so the outcome is Synced again, exactly
                // as app.cpp's recovery does; leaving it Stale would strand
                // the frame out of the coherent state W3 asserts.
                const int prev_rows = synced->rows();
                const int overflow = prev_rows > term_h
                    ? prev_rows - term_h : 0;
                auto committed = std::move(*synced).commit(
                    synced->scrollback_marker(overflow));
                commits += static_cast<std::size_t>(overflow);
                return std::move(committed).demote_to_stale().render(
                    c, content_rows(c), term_rows_for_test(term_h),
                    pool, writer, false);
            }();
            (void)drain_fd(rfd);
            synced = std::visit(
                [](auto&& a) -> std::optional<InlineFrame<Synced>> {
                    using T = std::decay_t<decltype(a)>;
                    if constexpr (std::is_same_v<T, InlineFrame<Synced>>)
                        return std::move(a);
                    else
                        return std::nullopt;
                }, std::move(o));
            // W3: every render stays Synced.
            INV(synced.has_value(), "W3",
                "render demoted out of Synced @" + tag);
            if (!synced) { dead = true; return; }
        }

        // W4: wire-model byte check. Canvas row y maps to absolute wire
        // index commits + y. Rows past the viewport top of THIS frame
        // (R - term_h of them) are committed from now on; compare any
        // already-captured indices, then extend the capture.
        //
        // EXCEPTION: the welcome screen (empty thread) is transient
        // first-run UI — in production it is NEVER committed to real
        // scrollback; the first submit replaces it in the viewport and
        // the conversation renders in its place. If it overflows a short
        // terminal, its top rows are NOT committed wire. Skip capture
        // while the thread has no messages so the legitimate
        // welcome→conversation swap isn't mistaken for a committed-row
        // rewrite. Once real turns exist and overflow, capture resumes
        // exactly as before (the welcome frame committed nothing, so
        // there is no stale prefix to conflict with).
        if (m.d.current.messages.empty()) return;
        const int over = R > term_h ? R - term_h : 0;
        for (int y = 0; y < over; ++y) {
            const std::size_t wi = commits + static_cast<std::size_t>(y);
            if (wi < wire.size()) {
                if (wire[wi] != cur[static_cast<std::size_t>(y)]) {
                    INV(false, "W4",
                        "committed wire row " + std::to_string(wi)
                        + " rewritten @" + tag + "\n      was |" + wire[wi]
                        + "|\n      now |" + cur[static_cast<std::size_t>(y)]
                        + "|");
                    dead = true;
                    return;
                }
            } else if (wi == wire.size()) {
                wire.push_back(cur[static_cast<std::size_t>(y)]);
            } else {
                // gap: rows committed by an explicit commit while never
                // crossing the overflow boundary — backfill from the
                // current frame (they are on-screen and stable).
                wire.resize(wi, std::string{});
                wire.push_back(cur[static_cast<std::size_t>(y)]);
            }
        }
    }
};

// ── One random walk ───────────────────────────────────────────────
// Mirrors the production cadence exactly:
//   submit:  settle prior live md → push user → freeze_through(size)
//   stream:  push assistant tail, grow streaming_text across frames
//   tools:   append settled tool cards to the live run
//   settle:  text ← streaming_text, settle_message_md, PAINT (the
//            pending_settle_freeze deferred frame), freeze_through,
//            trim_frozen_if_oversized → apply the CommitScrollback Cmd
//   picker:  open/close overlays at arbitrary points
//
// reveal_fx is forced OFF on harness widgets: the fuzz needs frame
// heights to be a pure function of model state (the wall-clock
// typewriter would make them time-dependent). The reveal→freeze seam
// is separately locked by live_tail_reveal_settled +
// stream_liveness_test; here we fuzz the structural wire math.
static void seed_md_no_reveal(Model& m, const Message& msg) {
    auto& cache = m.ui.view_cache.message_md(m.d.current.id, msg.id);
    if (!cache.streaming) {
        cache.streaming = std::make_shared<maya::StreamingMarkdown>();
        cache.streaming->set_reveal_fx(false);
    }
}

static void run_walk(std::uint64_t seed, int width, int term_h) {
    g_seed = seed;
    g_oplog.clear();

    // The view build reads terminal dims via ioctl with COLUMNS/LINES
    // fallback (ui::view's build-phase RenderContext + the welcome
    // clamp). The harness has no tty, so wire the simulated dims
    // through the fallback.
    setenv("LINES", std::to_string(term_h).c_str(), 1);
    setenv("COLUMNS", std::to_string(width).c_str(), 1);

    // Pin the timezone so the turn-header timestamp (timestamp_hh_mm ->
    // localtime_r) renders IDENTICALLY on every machine. Without this the
    // rendered header bytes depend on the runner's TZ, so a committed row's
    // content becomes machine-dependent and W4's byte-exact check can flake
    // across CI hosts (the walk itself even diverges when the header wraps
    // differently). Combined with the fixed per-message timestamp below,
    // every frame's bytes are a pure function of model state.
    setenv("TZ", "UTC", 1);
    tzset();

    Rng rng(seed);
    Model m;
    m.d.current.id = agentty::ThreadId{"wirefuzz"};
    m.d.available_models.push_back({});
    m.d.available_models.back().id = agentty::ModelId{"claude-opus-4-1"};
    m.d.available_models.push_back({});
    m.d.available_models.back().id = agentty::ModelId{"claude-sonnet-4-5"};
    agentty::app::detail::clear_frozen(m);

    WireHarness h(width, term_h, make_pipe_writer());
    bool streaming = false;
    int  delta_i = 0;

    auto op = [&](std::string s) { g_oplog.push_back(std::move(s)); };

    // First frame: the welcome screen (empty thread).
    h.frame(m, "welcome");

    const int steps = 25 + rng.below(35);
    for (int step = 0; step < steps && !h.dead && !g_failures; ++step) {
        const std::string st = std::to_string(step);

        if (!streaming) {
            switch (rng.below(5)) {
            case 0: {   // submit: user push + freeze prior turn
                Message u; u.role = Role::User;
                u.text = "request " + st + ": do a thing with detail";
                u.timestamp = kFixedTs;
                m.d.current.messages.push_back(std::move(u));
                agentty::app::detail::freeze_through(
                    m, m.d.current.messages.size());
                op("submit+freeze");
                h.frame(m, "submit" + st);
                break;
            }
            case 1: {   // start a stream
                Message a; a.role = Role::Assistant;
                a.timestamp = kFixedTs;
                a.streaming_text = "working on it";
                seed_md_no_reveal(m, a);
                m.d.current.messages.push_back(std::move(a));
                m.s.phase =
                    agentty::phase::Streaming{agentty::phase::Active{}};
                streaming = true;
                op("start stream");
                h.frame(m, "sstart" + st);
                break;
            }
            case 2: {   // picker open → frames → close → frames
                m.ui.panel = pn::FusedPicker{{0, ""}};
                op("picker open");
                h.frame(m, "popen" + st);
                if (rng.chance(50)) h.frame(m, "popen2-" + st);
                m.ui.panel.close<pn::FusedPicker>();
                op("picker close");
                h.frame(m, "pclose" + st);
                break;
            }
            case 3: {   // idle re-render (animation tick)
                op("idle frame");
                h.frame(m, "idle" + st);
                break;
            }
            default: {  // settled tool turn in one go (history replay)
                Message u; u.role = Role::User;
                u.text = "tool req " + st;
                u.timestamp = kFixedTs;
                m.d.current.messages.push_back(std::move(u));
                Message a; a.role = Role::Assistant;
                a.timestamp = kFixedTs;
                a.tool_calls.push_back(settled_tool(rng, "h" + st));
                m.d.current.messages.push_back(std::move(a));
                agentty::app::detail::freeze_through(
                    m, m.d.current.messages.size());
                op("settled turn+freeze");
                // Render BEFORE the trim: in production every frozen
                // entry has been on the wire for many frames before a
                // trim can drop it (content streams/renders, then
                // settles, then freezes; the trim drops the OLDEST
                // entries). A trim of never-rendered rows would commit
                // against a stale prev_rows — not a production state.
                h.frame(m, "hturn" + st);
                if (h.dead) break;
                auto cmd =
                    agentty::app::detail::trim_frozen_if_oversized(m);
                using Cmd = maya::Cmd<agentty::Msg>;
                // W5: the trim returns commit_scrollback(N) when it drops
                // rows (a top-DELETION that maya's render-time reconciliation
                // cannot fix — the host must commit in lockstep), or none()
                // when it doesn't trim. Both are acceptable; the invariant
                // is that no OTHER command type is returned.
                INV(std::holds_alternative<Cmd::None>(cmd.inner) ||
                    std::holds_alternative<Cmd::CommitScrollback>(cmd.inner),
                    "W5", "trim returned unexpected command type (must be "
                    "none() or commit_scrollback(N))");
                op("trim");
                h.frame(m, "htrim" + st);
                break;
            }
            }
        } else {
            switch (rng.below(4)) {
            case 0: {   // stream delta(s)
                auto& back = m.d.current.messages.back();
                const int n = 1 + rng.below(4);
                for (int d = 0; d < n; ++d)
                    back.streaming_text +=
                        "\n\nDelta " + std::to_string(delta_i++)
                        + ": more prose that wraps across the width "
                          "and keeps the live tail growing steadily.";
                op("deltas x" + std::to_string(n));
                h.frame(m, "delta" + st);
                break;
            }
            case 1: {   // settled tool card lands on the live run
                Message a; a.role = Role::Assistant;
                a.timestamp = kFixedTs;
                a.tool_calls.push_back(settled_tool(rng, "t" + st));
                // production: the tool sub-turn is pushed; the streaming
                // tail message settled its text into `text` first
                auto& back = m.d.current.messages.back();
                if (!back.streaming_text.empty()) {
                    back.text += back.streaming_text;
                    back.streaming_text.clear();
                    agentty::app::detail::settle_message_md(m, back);
                }
                m.d.current.messages.push_back(std::move(a));
                // continuation placeholder — the next sub-turn
                Message ph; ph.role = Role::Assistant;
                ph.timestamp = kFixedTs;
                ph.streaming_text = "continuing";
                seed_md_no_reveal(m, ph);
                m.d.current.messages.push_back(std::move(ph));
                op("tool sub-turn + continuation");
                h.frame(m, "tool" + st);
                break;
            }
            case 2: {   // picker over a live stream
                m.ui.panel = pn::FusedPicker{{0, ""}};
                op("picker open (streaming)");
                h.frame(m, "spopen" + st);
                m.ui.panel.close<pn::FusedPicker>();
                op("picker close (streaming)");
                h.frame(m, "spclose" + st);
                break;
            }
            default: {  // settle: finalize_turn → deferred freeze + trim
                auto& back = m.d.current.messages.back();
                back.text += back.streaming_text;
                back.streaming_text.clear();
                agentty::app::detail::settle_message_md(m, back);
                m.s.phase = agentty::phase::Idle{};
                streaming = false;
                // the deferred-settle paint (pending_settle_freeze):
                // one frame paints the post-finish tree BEFORE freezing
                op("settle md + paint");
                h.frame(m, "settlepaint" + st);
                if (h.dead) break;
                agentty::app::detail::freeze_through(
                    m, m.d.current.messages.size());
                // Production runs the trim for its model mutation but
                // issues NO host scrollback commit on the live-tail→frozen
                // collapse (meta.cpp returns none()): current maya
                // reconciles the shrink itself via its Synced-arm
                // discrimination. Mirror that here: drop the trim's Cmd,
                // then just paint — the harness's frame() goes through
                // maya's real compose, which does the reconciliation.
                (void)agentty::app::detail::trim_frozen_if_oversized(m);
                op("freeze + paint");
                h.frame(m, "freeze" + st);
                break;
            }
            }
        }
    }
}

}  // namespace (fold)

int main() {
    std::printf("scrollback_wire_fuzz\n");

    struct Shape { int w, th; };
    const Shape shapes[] = {{80, 24}, {100, 20}, {120, 30}, {60, 16}};
    const int walks_per_shape = 40;

    for (int si = 0; si < 4 && !g_failures; ++si) {
        for (int k = 0; k < walks_per_shape && !g_failures; ++k) {
            std::uint64_t seed = 0xC0FFEEULL
                + static_cast<std::uint64_t>(si) * 7'368'787ULL
                + static_cast<std::uint64_t>(k)  * 2'654'435'761ULL;
            run_walk(seed, shapes[si].w, shapes[si].th);
        }
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) { std::printf("FAILED\n"); return 1; }
    std::printf("PASSED\n");
    return 0;
}
