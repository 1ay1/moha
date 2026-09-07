// visual_hash_coverage_test — proves the render gate can't silently
// drop a visible change.
//
// THE BUG CLASS THIS GUARDS
// =========================
// maya's run loop skips view()+render() whenever
// `Program::visual_hash(model)` matches the previous frame's value
// (app.hpp). That gate is what lets a 30 fps Tick fire cheaply: a tick
// that changed nothing visible produces the same hash and costs zero
// layout/paint. The hazard is the inverse — if a model field DOES
// affect the rendered frame but is NOT mixed into visual_hash, a
// mutation to it produces an identical hash, the gate fires, and the
// change never paints until some UNRELATED hashed axis happens to flip.
// The failure is SILENT: no crash, no log, just a region of the UI that
// stops updating until the next keystroke / spinner tick. This is
// exactly the production "picker arrow keys register once per 4-5
// presses" regression (CHANGELOG): picker selection index wasn't in the
// hash, so cursor moves were gated away.
//
// THE CONTRACT
// ============
// Every field that affects the rendered view MUST advance visual_hash
// when it changes. Conversely, fields that DON'T affect pixels (the
// tick clock, token counters, cancel handles) MUST NOT advance it —
// otherwise the gate is defeated and every tick repaints, reintroducing
// the per-frame layout cost the gate exists to avoid.
//
// HOW THIS TEST ENFORCES IT
// =========================
// Two declarative tables:
//   • kVisualAxes    — {name, mutate}. For each: snapshot the hash,
//                      apply a visually-meaningful mutation, assert the
//                      hash CHANGED. A new view-affecting field that the
//                      author forgot to mix into visual_hash fails here
//                      the moment they add it to this table — and the
//                      table is the natural place to add it, so the
//                      omission is caught at the same edit.
//   • kInvariantAxes — {name, mutate}. For each: assert the hash did
//                      NOT change. Guards against over-hashing (mixing
//                      last_tick would make every tick repaint).
//
// The tables ARE the spec. When you add a model field that the view
// reads, add a row to kVisualAxes; the test then requires the matching
// `mix()` line in program.hpp. When you add ephemeral state the view
// ignores, add a row to kInvariantAxes.

#include <chrono>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include <maya/core/anim_clock.hpp>

#include "agtest.hpp"

#include "agentty/runtime/app/program.hpp"
#include "agentty/runtime/smart_form.hpp"
#include "agentty/runtime/model.hpp"

namespace ov = agentty::ui::overlay;
namespace smart = agentty::smart;

using agentty::Model;
using agentty::app::AgenttyApp;

namespace {

int g_checks = 0;


std::uint64_t hash_of(const Model& m) { return AgenttyApp::visual_hash(m); }

// A baseline model that is "settled and idle" — no active stream, empty
// composer, no modal open, a couple of messages so frozen/live axes are
// meaningful. Each axis test starts from a fresh copy of this so axes
// don't interfere.
Model baseline() {
    Model m;
    // Two settled messages so messages.size() / render_key axes have
    // something to move against.
    agentty::Message u;
    u.role = agentty::Role::User;
    u.text = "hello";
    m.d.current.messages.push_back(u);
    agentty::Message a;
    a.role = agentty::Role::Assistant;
    a.text = "hi there";
    m.d.current.messages.push_back(a);
    m.s.phase = agentty::phase::Idle{};
    return m;
}

// ── A mutation that changes one axis in a visually-meaningful way. ──
struct Axis {
    const char*                 name;
    std::function<void(Model&)> mutate;
};

// Fields that AFFECT the rendered frame. Each mutation must move the
// hash. Mirror the axes program.hpp::visual_hash mixes; a gap between
// these tables and that function is the bug this test exists to catch.
const std::vector<Axis>& visual_axes() {
    static const std::vector<Axis> axes = {
        {"messages.size (append a turn)", [](Model& m) {
            agentty::Message x; x.role = agentty::Role::User; x.text = "new";
            m.d.current.messages.push_back(x);
        }},
        {"live tail message text (render_key)", [](Model& m) {
            m.d.current.messages.back().text += " more";
        }},
        {"live tail pending_stream (render_key)", [](Model& m) {
            // A delta that lands only in the Tick pacer's buffer must
            // still advance the hash, or the render gate skips the frame
            // and the live tail's reveal stops re-arming — the stream
            // freezes until an unrelated axis flips.
            m.d.current.messages.back().pending_stream += "buffered";
        }},
        {"profile cycle", [](Model& m) {
            m.d.profile = (m.d.profile == agentty::Profile::Write)
                        ? agentty::Profile::Ask : agentty::Profile::Write;
        }},
        {"reasoning effort tier", [](Model& m) {
            // The effort tier renders in the model badge (and picker line);
            // ←/→ must repaint. High → Low is a visible change.
            m.d.effort = (m.d.effort == agentty::Effort::High)
                       ? agentty::Effort::Low : agentty::Effort::High;
        }},
        {"model_id swap", [](Model& m) {
            m.d.model_id = agentty::ModelId{std::string{"claude-haiku-4-5"}};
        }},
        {"pending_permission appears", [](Model& m) {
            m.d.pending_permission = agentty::PendingPermission{};
        }},
        {"phase Idle -> Streaming", [](Model& m) {
            m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
        }},
        {"status banner text", [](Model& m) {
            m.s.status = "something happened";
        }},
        {"status expiry (100ms bucket)", [](Model& m) {
            m.s.status = "x";
            m.s.status_until = std::chrono::steady_clock::now()
                             + std::chrono::seconds(5);
        }},
        {"spinner frame (while active)", [](Model& m) {
            m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
            // The spinner is clock-driven: advance the shared animation
            // clock past a full frame-interval so frame_index() changes.
            maya::testing::advance_anim_clock_ms(1000);
        }},
        {"composer text", [](Model& m) {
            m.ui.composer.text = "typing";
        }},
        {"composer cursor", [](Model& m) {
            m.ui.composer.text = "abc";
            m.ui.composer.cursor = 2;
        }},
        {"composer attachment count", [](Model& m) {
            m.ui.composer.attachments.push_back(agentty::Attachment{});
        }},
        {"composer queued count", [](Model& m) {
            m.ui.composer.queued.push_back({"queued msg", {}});
        }},
        {"composer expanded toggle", [](Model& m) {
            m.ui.composer.expanded = true;
        }},
        {"frozen prefix grows", [](Model& m) {
            m.ui.frozen.seal(maya::Element{}, 1);
        }},
        {"frozen_turn advances", [](Model& m) {
            m.ui.frozen_turn = 7;
        }},
        {"model_picker opens", [](Model& m) {
            m.ui.overlay = ov::FusedPicker{{0, ""}};
        }},
        {"model_picker cursor move", [](Model& m) {
            m.ui.overlay = ov::FusedPicker{{3}};
        }},
        {"model_picker query", [](Model& m) {
            agentty::ui::pick::OpenAt o; o.index = 0; o.query = "free";
            m.ui.overlay = ov::FusedPicker{std::move(o)};
        }},
        {"provider_picker opens", [](Model& m) {
            m.ui.overlay = ov::ProviderPicker{{0}};
        }},
        {"provider_picker cursor move", [](Model& m) {
            m.ui.overlay = ov::ProviderPicker{{2}};
        }},
        {"thread_list opens", [](Model& m) {
            m.ui.overlay = ov::ThreadList{{0}};
        }},
        {"thread_list cursor move", [](Model& m) {
            m.ui.overlay = ov::ThreadList{{4}};
        }},
        {"thread_list delete confirm", [](Model& m) {
            auto o = agentty::ui::pick::OpenAt{2};
            o.confirm_remove = "abc123";
            m.ui.overlay = agentty::ui::overlay::ThreadList{std::move(o)};
        }},
        {"diff_review opens at cell", [](Model& m) {
            m.ui.overlay = ov::DiffReview{{0, 0}};
        }},
        {"diff_review hunk move", [](Model& m) {
            m.ui.overlay = ov::DiffReview{{1, 2}};
        }},
        {"command_palette opens", [](Model& m) {
            m.ui.overlay = ov::CommandPalette{{}};
        }},
        {"command_palette query", [](Model& m) {
            m.ui.overlay = ov::CommandPalette{{"git", 0}};
        }},
        {"command_palette index", [](Model& m) {
            m.ui.overlay = ov::CommandPalette{{"git", 5}};
        }},
        {"mention_palette opens", [](Model& m) {
            m.ui.overlay = agentty::ui::overlay::Mention{};
        }},
        {"mention_palette query", [](Model& m) {
            agentty::mention::Open o; o.query = "src"; o.index = 0;
            m.ui.overlay = ov::Mention{std::move(o)};
        }},
        {"mention_palette index", [](Model& m) {
            agentty::mention::Open o; o.query = "src"; o.index = 3;
            m.ui.overlay = ov::Mention{std::move(o)};
        }},
        {"symbol_palette opens", [](Model& m) {
            m.ui.overlay = agentty::ui::overlay::Symbol{};
        }},
        {"symbol_palette query", [](Model& m) {
            agentty::symbol_palette::Open o; o.query = "foo"; o.index = 0;
            m.ui.overlay = ov::Symbol{std::move(o)};
        }},
        {"symbol_palette index", [](Model& m) {
            agentty::symbol_palette::Open o; o.query = "foo"; o.index = 2;
            m.ui.overlay = ov::Symbol{std::move(o)};
        }},
        {"todo modal opens", [](Model& m) {
            m.ui.todo.open = agentty::ui::pick::OpenModal{};
        }},
        {"tool viewer opens", [](Model& m) {
            m.ui.overlay = ov::ToolViewer{{{}, 0, false}};
        }},
        {"tool viewer list cursor move", [](Model& m) {
            m.ui.overlay = ov::ToolViewer{{{}, 2, false}};
        }},
        {"tool viewer list -> body stage", [](Model& m) {
            m.ui.overlay = ov::ToolViewer{{{}, 0, true}};
        }},
        {"tool viewer body scroll", [](Model& m) {
            m.ui.overlay = ov::ToolViewer{{{}, 0, true}};
            m.ui.tool_viewer_scroll.y = 5;
        }},
        {"tool viewer live tail toggled", [](Model& m) {
            m.ui.overlay = ov::ToolViewer{{
                {agentty::tool_viewer::Entry{}}, 0, /*viewing=*/true}};
            m.ui.tool_viewer_tail = false;
        }},
        {"code block picker opens", [](Model& m) {
            m.ui.overlay = ov::CodeBlocks{{{}, 0}};
        }},
        {"code block picker cursor move", [](Model& m) {
            m.ui.overlay = ov::CodeBlocks{{{}, 3}};
        }},
        {"login modal opens", [](Model& m) {
            m.ui.login = agentty::ui::login::Picking{};
        }},
        {"login chatgpt waiting", [](Model& m) {
            m.ui.login = agentty::ui::login::ChatGptWaiting{};
        }},
        {"settings_list opens (Plugins)", [](Model& m) {
            agentty::settings::ListOpen o;
            o.concern = agentty::settings::Category::Plugins;
            m.ui.overlay = agentty::ui::overlay::SettingsList{o};
        }},
        {"settings_list cursor move", [](Model& m) {
            agentty::settings::ListOpen o;
            o.concern = agentty::settings::Category::Plugins;
            o.index = 3;
            m.ui.overlay = agentty::ui::overlay::SettingsList{o};
        }},
        {"settings_list add-mode input", [](Model& m) {
            agentty::settings::ListOpen o;
            o.concern = agentty::settings::Category::Plugins;
            o.input_active = true;
            o.input = "date -- /path/date_server";
            o.cursor = 4;
            m.ui.overlay = agentty::ui::overlay::SettingsList{o};
        }},
        {"plugins loading flag", [](Model& m) {
            m.ui.plugins_loading = true;
        }},
        {"plugins snapshot lands (server connected)", [](Model& m) {
            agentty::mcp::ServerState s;
            s.name = "date";
            s.connected = true;
            s.tools.push_back({"current_date", "", true, false});
            m.ui.plugins.servers.push_back(std::move(s));
        }},
        {"plugins server disabled toggles the hash", [](Model& m) {
            agentty::mcp::ServerState s;
            s.name = "date";
            s.disabled = true;   // distinct from plain disconnected
            s.tools.push_back({"current_date", "", true, false});
            m.ui.plugins.servers.push_back(std::move(s));
        }},
        {"smart_mode overlay opens", [](Model& m) {
            agentty::smart_form::Inputs in;
            in.enabled = true;
            m.ui.overlay = ov::SmartMode{{}, agentty::smart_form::build_form(in)};
        }},
        {"smart_mode cursor move", [](Model& m) {
            agentty::smart_form::Inputs in;
            in.enabled = true;
            auto f = agentty::smart_form::build_form(in);
            agentty::smart_form::focus_role(f, agentty::smart::ModelRole::Utility);
            m.ui.overlay = ov::SmartMode{{}, std::move(f)};
        }},
        {"rag picker opens", [](Model& m) {
            agentty::rag_settings::Open o;
            m.ui.overlay = agentty::ui::overlay::RagSettings{o};
        }},
        {"rag picker cursor move", [](Model& m) {
            agentty::rag_settings::Open o;
            o.cursor = agentty::store::RagMode::Off;
            m.ui.overlay = agentty::ui::overlay::RagSettings{o};
        }},
        {"fork picker opens", [](Model& m) {
            m.ui.overlay = ov::Fork{{agentty::fork_picker::Choice::RagPerTurn}};
        }},
        {"fork picker cursor move", [](Model& m) {
            m.ui.overlay = ov::Fork{{agentty::fork_picker::Choice::RagOff}};
        }},
    };
    return axes;
}

// Fields the view does NOT read. Mutating these MUST NOT move the hash,
// or the render gate is defeated (every tick repaints). These are the
// dual of the contract: visual_hash's whole value is in what it OMITS.
const std::vector<Axis>& invariant_axes() {
    static const std::vector<Axis> axes = {
        {"last_tick clock", [](Model& m) {
            m.s.last_tick = std::chrono::steady_clock::now()
                          + std::chrono::hours(1);
        }},
        {"token counters", [](Model& m) {
            m.s.tokens_in  = 12345;
            m.s.tokens_out = 67890;
        }},
        {"pending rehydrate trim", [](Model& m) {
            m.ui.pending_rehydrate_trim = true;
        }},
    };
    return axes;
}

}  // namespace (helpers)

TEST_CASE("visual axes advance hash") {
    std::printf("visual_hash: each view axis advances the hash\n");
    for (const auto& ax : visual_axes()) {
        Model before = baseline();
        const std::uint64_t h0 = hash_of(before);
        Model after = baseline();
        ax.mutate(after);
        const std::uint64_t h1 = hash_of(after);
        check(h0 != h1,
              std::string("axis '") + ax.name +
              "' did NOT change visual_hash — the view reads it but "
              "program.hpp::visual_hash forgot to mix() it. Renders for "
              "this change will be gated away (silent dead region).");
    }
}

TEST_CASE("invariant axes preserve hash") {
    std::printf("visual_hash: non-visual axes do NOT advance the hash\n");
    for (const auto& ax : invariant_axes()) {
        Model before = baseline();
        const std::uint64_t h0 = hash_of(before);
        Model after = baseline();
        ax.mutate(after);
        const std::uint64_t h1 = hash_of(after);
        check(h0 == h1,
              std::string("axis '") + ax.name +
              "' CHANGED visual_hash but the view ignores it — every "
              "Tick will now defeat the render gate and repaint. Remove "
              "its mix() from program.hpp::visual_hash.");
    }
}

// Sanity: the baseline itself is stable across two calls (no time term
// leaking for a settled/idle model — regime (c): nothing animating).
TEST_CASE("idle settled is stable") {
    // Pin the animation clock. These cases assert that two hashes of an
    // UNCHANGED model agree, which is only meaningful if time cannot move
    // between the two calls. visual_hash mixes a time BUCKET whenever
    // anything animates — and a bare Model has no messages, so the welcome
    // bob counts as animating. Unfrozen, the two calls could straddle a
    // bucket edge and disagree: reliably so under `ctest -j12`, where load
    // widens the gap between them. The clock is frozen, not merely read
    // once, so the assertion tests the MODEL rather than the scheduler.
    maya::testing::freeze_anim_clock(1000000);
    std::printf("visual_hash: settled idle model is hash-stable across calls\n");
    Model m = baseline();
    const std::uint64_t a = hash_of(m);
    // No mutation, no sleep — an idle settled model (no active stream,
    // a non-empty thread so the welcome bob is off, empty queue) must
    // contribute no time term, so two reads match.
    const std::uint64_t b = hash_of(m);
    check(a == b,
          "idle settled model produced two different hashes — a time "
          "term is leaking into a regime (c) state (should be none).");
}

// ── Spinner gates: subscription, advance and hash must agree ────────────
//
// A time-based animation in this app passes through THREE independent
// gates, and all three must name the same condition:
//
//   1. subscribe.cpp   — is a Tick even delivered?
//   2. update/meta.cpp — does the spinner advance on that Tick?
//   3. program.hpp     — does the new frame change the visual hash
//                        (i.e. does anything repaint)?
//
// Disagreement is SILENT: a frame that advances without being hashed
// animates invisibly; one hashed without advancing burns renders on an
// unchanged glyph. The fused picker's "loading …" spinner needs all
// three while `models_loading` is set — a frozen glyph reads as a hang,
// which is the exact opposite of what it exists to say.
TEST_CASE("visual hash: spinner advances the hash while models load") {
    // Frozen so the ONLY time motion is the explicit advance below.
    maya::testing::freeze_anim_clock(2000000);
    Model m;
    REQUIRE(!m.s.active());          // idle: the streaming gate is off
    m.s.models_loading = true;

    const auto h0 = agentty::app::AgenttyApp::visual_hash(m);
    // The spinner derives its frame from the shared clock; move it past a
    // full frame-interval so frame_index() changes.
    maya::testing::advance_anim_clock_ms(1000);
    const auto h1 = agentty::app::AgenttyApp::visual_hash(m);
    CHECK(h0 != h1);
    maya::testing::unfreeze_anim_clock();
}

TEST_CASE("visual hash: an idle picker with no load does NOT animate") {
    maya::testing::freeze_anim_clock(2000000);
    // The complement: without models_loading (and not streaming) the
    // spinner must stay out of the hash, or an idle agentty repaints
    // forever for a glyph nobody is looking at. STRONGER than the old
    // advance()-based check: with a clock-driven spinner we assert the
    // hash is time-independent outright — moving the clock (which moves
    // the spinner's frame) must not move the hash.
    Model m;
    REQUIRE(!m.s.active());
    m.s.models_loading = false;

    const auto h0 = agentty::app::AgenttyApp::visual_hash(m);
    // Parity-neutral advance: a multiple of the 530 ms blink period so the
    // caret parity bucket (now/265 & 1) is unchanged, while the spinner
    // (80 ms/frame) moves through ~13 frames. Isolates the axis under test.
    maya::testing::advance_anim_clock_ms(1060);
    CHECK(agentty::app::AgenttyApp::visual_hash(m) == h0);
    maya::testing::unfreeze_anim_clock();
}

TEST_CASE("visual hash: spinner animates while a PICKER catalog loads") {
    // Frozen: this asserts the hash CHANGES for one spinner step, which a
    // live clock could satisfy by accident (a bucket flip) instead of by
    // the spinner frame the case is actually about.
    maya::testing::freeze_anim_clock(2000000);
    // The picker fans out to every authed provider and tracks each fetch on
    // ProviderCatalog::state — it never sets Session::models_loading (that
    // covers the ACTIVE provider only: provider switch / startup). Gating
    // the spinner on the wrong flag left it frozen in the one surface it
    // exists for, which reads as a hang.
    Model m;
    REQUIRE(!m.s.active());
    REQUIRE(!m.s.models_loading);
    agentty::ProviderCatalog c;
    c.provider_id = "openai";
    c.state = agentty::ProviderCatalog::State::Loading;
    m.d.provider_catalogs.push_back(std::move(c));
    m.ui.overlay = agentty::ui::overlay::FusedPicker{};
    REQUIRE(m.loading_spinner_visible());

    const auto h0 = agentty::app::AgenttyApp::visual_hash(m);
    maya::testing::advance_anim_clock_ms(1000);   // move the spinner's frame
    CHECK(agentty::app::AgenttyApp::visual_hash(m) != h0);
    maya::testing::unfreeze_anim_clock();
}

TEST_CASE("visual hash: a READY catalog does not animate") {
    maya::testing::freeze_anim_clock(2000000);
    Model m;
    agentty::ProviderCatalog c;
    c.provider_id = "openai";
    c.state = agentty::ProviderCatalog::State::Ready;
    m.d.provider_catalogs.push_back(std::move(c));
    m.ui.overlay = agentty::ui::overlay::FusedPicker{};
    REQUIRE(!m.loading_spinner_visible());

    const auto h0 = agentty::app::AgenttyApp::visual_hash(m);
    // Parity-neutral advance (multiple of the 530 ms blink period) — see
    // the idle-picker case above; moves the spinner ~13 frames while the
    // caret parity bucket stays put.
    maya::testing::advance_anim_clock_ms(1060);
    CHECK(agentty::app::AgenttyApp::visual_hash(m) == h0);
    maya::testing::unfreeze_anim_clock();
}

// ── A widget-paced visual gets NO time term from the host ────────────
//
// The design rule, enforced here rather than remembered: a visual is EITHER
// host-paced (we arm a timer and bucket the hash at that period) OR
// widget-paced (it calls request_animation_frame(_after) and maya's run loop
// renders it on its own schedule). Never both.
//
// Both is what broke: agentty bucketed the RAF-driven welcome screen at its
// streaming-tick period (80 ms over ssh) while welcome_screen.hpp was asking
// for 110 ms. Two independent clocks for one visual, so renders landed
// part-way through animation steps — 42 hash values per 30 requested frames,
// and an idle welcome screen that visibly flickered instead of bobbing.
//
// maya::animation_pending() reports "a widget already owns the next frame".
// While that holds, visual_hash must contribute no time term at all: maya's
// RAF override renders those frames regardless of this hash, so any bucket
// we add is purely a second, out-of-phase clock.
TEST_CASE("visual hash: a widget-paced frame gets no host time bucket") {
    Model m;
    m.d.current.messages.clear();          // welcome screen: RAF-driven
    REQUIRE(!m.s.active());

    // Simulate a widget having asked for the next frame, exactly as
    // welcome_screen.hpp's mount does during build().
    maya::request_animation_frame_after(110);
    REQUIRE(maya::animation_pending());

    // With a frame pending, the hash must be time-INDEPENDENT: sweep a span
    // far wider than any plausible bucket and demand a single value.
    std::set<std::uint64_t> distinct;
    for (std::int64_t t = 0; t < 3300; t += 7) {
        maya::testing::freeze_anim_clock(1000000 + t);
        distinct.insert(agentty::app::AgenttyApp::visual_hash(m));
    }
    CHECK(distinct.size() == 1);

    // And the converse: with NO widget-paced frame pending, a host-paced
    // animation still gets its bucket — the deferral must not disable the
    // hash's own animations. An active turn is host-paced (Tick-driven).
    maya::consume_animation_request_for_test();
    REQUIRE(!maya::animation_pending());
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
    REQUIRE(m.s.active());
    std::set<std::uint64_t> active_hashes;
    for (std::int64_t t = 0; t < 3300; t += 7) {
        maya::testing::freeze_anim_clock(1000000 + t);
        active_hashes.insert(agentty::app::AgenttyApp::visual_hash(m));
    }
    CHECK(active_hashes.size() > 1);

    maya::testing::freeze_anim_clock(1000000);
}

// ── An idle WELCOME screen must be hash-stable ───────────────────────
//
// The welcome screen is the one state with no messages, so it exercises a
// different set of hash terms than baseline() (which has two). It is also
// the state a user stares at longest before typing, and the one where a
// repaint they didn't ask for is most visible.
//
// With the animation clock frozen, visual_hash must be a PURE function of
// the model: two calls on an untouched Model must agree, and must keep
// agreeing. A difference here means the hash is reading something outside
// the model — which the run loop then sees as "the screen changed", so it
// repaints, and the caret is redrawn under the user for no reason.
TEST_CASE("visual hash: an idle welcome screen is hash-stable") {
    maya::testing::freeze_anim_clock(1000000);

    Model m;                          // no messages: the welcome screen
    REQUIRE(!m.s.active());
    REQUIRE(m.d.current.messages.empty());

    const auto h0 = agentty::app::AgenttyApp::visual_hash(m);
    for (int i = 0; i < 16; ++i) {
        CHECK(agentty::app::AgenttyApp::visual_hash(m) == h0);
    }

    // Advancing the clock must not move the hash WHEN THE TERMINAL OWNS THE
    // CARET. That is the case this exists to pin: with the hardware caret
    // maya paints no caret cell and schedules no frames, so there is no
    // visible step for a time bucket to track, and mixing one makes the loop
    // repaint against the terminal's own blink.
    //
    // With the PAINTED caret the opposite is true — maya blinks it and asks
    // for the frames — so a parity term is correct and the hash SHOULD move.
    // Asserting time-independence unconditionally would be asserting a bug.
    if (agentty::ui::composer_uses_hardware_caret(m)) {
        maya::request_animation_frame_after(110);
        for (std::int64_t t = 0; t < 2000; t += 37) {
            maya::testing::freeze_anim_clock(1000000 + t);
            CHECK(agentty::app::AgenttyApp::visual_hash(m) == h0);
        }
    }
    maya::consume_animation_request_for_test();
    maya::testing::freeze_anim_clock(1000000);
}

// ── The three-gate agreement, pinned structurally ───────────────────────
//
// animation_demand / reveal_needs_frames / reveal_draining (subscribe.hpp)
// are the single definitions of "something is moving". This case pins the
// agreement those predicates exist to guarantee:
//
//   ARMED ⇒ HASHED: any state where the Tick subscription arms the timer
//   for REVEAL reasons must be a state whose visual_hash carries a time
//   term — otherwise the loop wakes and the hash gate discards every
//   wake: armed frames, frozen pixels (the mid-glide freeze). The
//   converse (hashed ⇒ armed) is deliberately NOT asserted: widget-paced
//   and caret-parity terms are maya's own frame sources, armed outside
//   the Tick.
//
// Falsified: killing the drain bucket in visual_hash fails the three
// drain-shaped cases below. Two honest limits, verified by trying:
//   • reveal_needs_frames' REASONING term is cadence-only today —
//     fine_anim_live (m.s.active()) sits below it in the chain and mixes
//     a coarser bucket for the same states, so dropping the term degrades
//     16ms→33/100ms smoothness on sync terminals but cannot freeze the
//     hash. Defense-in-depth, deliberate; this test cannot (and should
//     not) fail on cadence.
//   • The drain states are the load-bearing ones: in that window BOTH
//     m.s.active() and fine_anim_live are false, so the drain bucket is
//     the ONLY time term — exactly why killing it fails here.
TEST_CASE("visual hash: every reveal-armed state carries a time term") {
    struct Case {
        const char* name;
        std::function<void(Model&)> setup;
    };
    const Case cases[] = {
        {"streaming answer bytes", [](Model& m) {
            m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
            agentty::Message a; a.role = agentty::Role::Assistant;
            a.streaming_text = "partial answer";
            m.d.current.messages.push_back(std::move(a));
        }},
        {"pure reasoning phase", [](Model& m) {
            m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
            m.d.show_reasoning = true;
            agentty::Message a; a.role = agentty::Role::Assistant;
            a.thinking = "chain of thought so far";
            m.d.current.messages.push_back(std::move(a));
        }},
        {"undrained bytes after active dropped", [](Model& m) {
            agentty::Message a; a.role = agentty::Role::Assistant;
            a.pending_stream = "tail not yet drained";
            m.d.current.messages.push_back(std::move(a));
        }},
        {"settle-freeze pending", [](Model& m) {
            agentty::Message a; a.role = agentty::Role::Assistant;
            a.text = "done";
            m.d.current.messages.push_back(std::move(a));
            m.ui.pending_settle_freeze = true;
        }},
        {"settle cooldown", [](Model& m) {
            agentty::Message a; a.role = agentty::Role::Assistant;
            a.text = "done";
            m.d.current.messages.push_back(std::move(a));
            m.ui.settle_cooldown_ticks = 2;
        }},
    };

    for (const auto& c : cases) {
        Model m;
        c.setup(m);
        // Only assert on states the subscription actually arms for reveal
        // reasons — the agreement under test.
        if (!agentty::app::animation_demand(m)) continue;

        maya::testing::freeze_anim_clock(3000000);
        const auto h0 = agentty::app::AgenttyApp::visual_hash(m);
        // One reveal bucket is 16-33ms; 300ms crosses several regardless
        // of terminal sync mode. If no time term is mixed, the hash is
        // frozen and the armed wakes render nothing.
        maya::testing::freeze_anim_clock(3000000 + 300);
        const auto h1 = agentty::app::AgenttyApp::visual_hash(m);
        maya::testing::unfreeze_anim_clock();

        INFO(c.name);
        CHECK_MESSAGE(h0 != h1,
            c.name << ": the Tick subscription arms for this state but "
            "visual_hash mixes no time term — armed frames would be gated "
            "away and the animation freezes until a keypress");
    }
}
