// moha::app::update reducer tests.
//
// The reducer is pure modulo the Deps vtable (type-erased provider + store
// callables). We install no-op stubs so any dispatch into cmd factories
// stays graceful, then exercise a curated set of pure state transitions —
// enough to trip every hand-written arm of the composer / picker / modal /
// profile / tool / stream handlers at least once.

#include <cstdio>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "moha/runtime/app/deps.hpp"
#include "moha/runtime/app/update.hpp"
#include "moha/runtime/model.hpp"
#include "moha/runtime/msg.hpp"
#include "moha/runtime/picker.hpp"
#include "moha/runtime/command_palette.hpp"

namespace {

// ── minimal test harness ────────────────────────────────────────────────

struct Test {
    std::string_view            name;
    void                      (*run)(int&);
};

std::vector<Test>& registry() {
    static std::vector<Test> r;
    return r;
}

struct Registrar {
    Registrar(std::string_view n, void (*f)(int&)) {
        registry().push_back({n, f});
    }
};

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) {                                                        \
            ++fails;                                                          \
            std::fprintf(stderr, "  FAIL %s:%u  %s\n",                        \
                         std::source_location::current().file_name(),         \
                         std::source_location::current().line(),              \
                         #expr);                                              \
        }                                                                     \
    } while (0)

#define TEST(NAME)                                                            \
    static void NAME(int& fails);                                             \
    static const Registrar reg_##NAME{#NAME, &NAME};                          \
    static void NAME([[maybe_unused]] int& fails)

// ── shared fixtures ─────────────────────────────────────────────────────

void install_stub_deps() {
    moha::app::install_deps(moha::app::Deps{
        .stream         = [](moha::provider::Request, moha::provider::EventSink) {},
        .save_thread    = [](const moha::Thread&) {},
        .load_threads   = [] { return std::vector<moha::Thread>{}; },
        .load_settings  = [] { return moha::store::Settings{}; },
        .save_settings  = [](const moha::store::Settings&) {},
        .new_thread_id  = [] { return moha::ThreadId{"t-test"}; },
        .title_from     = [](std::string_view s) { return std::string{s}; },
        .auth_header    = {},
        .auth_style     = moha::auth::Style::ApiKey,
    });
}

moha::Model blank_model() {
    moha::Model m{};
    m.d.current.id = moha::ThreadId{"t-test"};
    return m;
}

// ── composer ────────────────────────────────────────────────────────────

TEST(composer_char_input_appends_and_advances_cursor) {
    auto m = blank_model();
    auto [m1, _] = moha::app::update(std::move(m), moha::ComposerCharInput{U'h'});
    auto [m2, __] = moha::app::update(std::move(m1), moha::ComposerCharInput{U'i'});
    CHECK(m2.ui.composer.text == "hi");
    CHECK(m2.ui.composer.cursor == 2);
}

TEST(composer_backspace_removes_prev_grapheme) {
    auto m = blank_model();
    m.ui.composer.text   = "abc";
    m.ui.composer.cursor = 3;
    auto [m1, _] = moha::app::update(std::move(m), moha::ComposerBackspace{});
    CHECK(m1.ui.composer.text == "ab");
    CHECK(m1.ui.composer.cursor == 2);
}

TEST(composer_backspace_on_empty_is_noop) {
    auto m = blank_model();
    auto [m1, _] = moha::app::update(std::move(m), moha::ComposerBackspace{});
    CHECK(m1.ui.composer.text.empty());
    CHECK(m1.ui.composer.cursor == 0);
}

TEST(composer_newline_inserts_and_expands) {
    auto m = blank_model();
    m.ui.composer.text   = "a";
    m.ui.composer.cursor = 1;
    auto [m1, _] = moha::app::update(std::move(m), moha::ComposerNewline{});
    CHECK(m1.ui.composer.text == "a\n");
    CHECK(m1.ui.composer.cursor == 2);
    CHECK(m1.ui.composer.expanded);
}

TEST(composer_toggle_expand_flips) {
    auto m = blank_model();
    CHECK(!m.ui.composer.expanded);
    auto [m1, _] = moha::app::update(std::move(m), moha::ComposerToggleExpand{});
    CHECK(m1.ui.composer.expanded);
    auto [m2, __] = moha::app::update(std::move(m1), moha::ComposerToggleExpand{});
    CHECK(!m2.ui.composer.expanded);
}

TEST(composer_cursor_home_and_end) {
    auto m = blank_model();
    m.ui.composer.text   = "hello";
    m.ui.composer.cursor = 2;
    auto [m1, _] = moha::app::update(std::move(m), moha::ComposerCursorHome{});
    CHECK(m1.ui.composer.cursor == 0);
    auto [m2, __] = moha::app::update(std::move(m1), moha::ComposerCursorEnd{});
    CHECK(m2.ui.composer.cursor == 5);
}

TEST(composer_paste_with_newline_expands) {
    auto m = blank_model();
    auto [m1, _] = moha::app::update(std::move(m),
                                     moha::ComposerPaste{"line1\nline2"});
    CHECK(m1.ui.composer.text == "line1\nline2");
    CHECK(m1.ui.composer.cursor == 11);
    CHECK(m1.ui.composer.expanded);
}

// ── model picker ────────────────────────────────────────────────────────

TEST(model_picker_open_then_close) {
    auto m = blank_model();
    auto [m1, _] = moha::app::update(std::move(m), moha::OpenModelPicker{});
    CHECK(moha::ui::pick::opened(m1.ui.model_picker) != nullptr);
    auto [m2, __] = moha::app::update(std::move(m1), moha::CloseModelPicker{});
    CHECK(moha::ui::pick::opened(m2.ui.model_picker) == nullptr);
}

TEST(model_picker_move_adjusts_index) {
    auto m = blank_model();
    m.d.available_models.push_back(
        {moha::ModelId{"claude-opus-4-5"},   "Opus 4.5",   "anthropic", 200000, false});
    m.d.available_models.push_back(
        {moha::ModelId{"claude-sonnet-4-6"}, "Sonnet 4.6", "anthropic", 200000, false});
    // Open the picker first so moves are processed.
    m.ui.model_picker = moha::ui::pick::OpenAt{0};
    auto [m1, _] = moha::app::update(std::move(m), moha::ModelPickerMove{+1});
    auto* o1 = moha::ui::pick::opened(m1.ui.model_picker);
    CHECK(o1 != nullptr);
    if (o1) CHECK(o1->index == 1);
    auto [m2, __] = moha::app::update(std::move(m1), moha::ModelPickerMove{+1});
    // Clamps at the last entry; exact clamp style is implementation-defined,
    // but the index must stay within [0, size).
    int idx2 = moha::ui::pick::index_or(m2.ui.model_picker, -1);
    CHECK(idx2 >= 0);
    CHECK(idx2 < static_cast<int>(m2.d.available_models.size()));
}

// ── profile ─────────────────────────────────────────────────────────────

TEST(cycle_profile_rotates) {
    auto m = blank_model();
    CHECK(m.d.profile == moha::Profile::Write);
    auto [m1, _] = moha::app::update(std::move(m), moha::CycleProfile{});
    CHECK(m1.d.profile != moha::Profile::Write);
    // Three cycles should land back on Write regardless of enum order.
    auto [m2, __]  = moha::app::update(std::move(m1), moha::CycleProfile{});
    auto [m3, ___] = moha::app::update(std::move(m2), moha::CycleProfile{});
    CHECK(m3.d.profile == moha::Profile::Write);
}

// ── scroll / expand ─────────────────────────────────────────────────────

TEST(scroll_thread_adjusts_offset) {
    auto m = blank_model();
    auto [m1, _] = moha::app::update(std::move(m), moha::ScrollThread{3});
    CHECK(m1.ui.thread_scroll == 3);
    auto [m2, __] = moha::app::update(std::move(m1), moha::ScrollThread{-1});
    CHECK(m2.ui.thread_scroll == 2);
}

TEST(toggle_tool_expanded_flips_match) {
    auto m = blank_model();
    moha::Message a;
    a.role = moha::Role::Assistant;
    moha::ToolUse tc;
    tc.id       = moha::ToolCallId{"t-1"};
    tc.name     = moha::ToolName{"read"};
    tc.expanded = false;
    a.tool_calls.push_back(std::move(tc));
    m.d.current.messages.push_back(std::move(a));

    auto [m1, _] = moha::app::update(std::move(m),
                                     moha::ToggleToolExpanded{moha::ToolCallId{"t-1"}});
    CHECK(m1.d.current.messages.back().tool_calls.front().expanded);
}

// ── stream events ───────────────────────────────────────────────────────

TEST(stream_started_sets_active) {
    // The phase transitions to Streaming in cmd_factory (the submit path)
    // *before* the SSE loop fires StreamStarted. StreamStarted resets the
    // watchdog/rate fields; active() is already true when it arrives.
    // Test: pre-set Streaming phase, fire StreamStarted, verify active() holds
    // and the per-stream accumulators are wiped (started is updated).
    auto m = blank_model();
    m.s.phase = moha::phase::Streaming{};
    auto t_before = std::chrono::steady_clock::now();
    auto [m1, _] = moha::app::update(std::move(m), moha::StreamStarted{});
    CHECK(m1.s.active());
    CHECK(!m1.s.is_idle());
    CHECK(m1.s.started >= t_before);
}

TEST(stream_text_delta_appends_to_assistant_streaming_text) {
    auto m = blank_model();
    moha::Message a;
    a.role            = moha::Role::Assistant;
    a.streaming_text  = "Hel";
    m.d.current.messages.push_back(std::move(a));
    auto [m1, _] = moha::app::update(std::move(m), moha::StreamTextDelta{"lo"});
    CHECK(m1.d.current.messages.back().streaming_text == "Hello");
}

TEST(stream_error_transitions_to_idle) {
    auto m = blank_model();
    m.s.phase = moha::phase::Streaming{};
    auto [m1, _] = moha::app::update(std::move(m),
                                     moha::StreamError{"boom"});
    CHECK(!m1.s.active());
    CHECK(m1.s.is_idle());
}

// ── tick + no-op ────────────────────────────────────────────────────────

TEST(tick_on_fresh_model_is_harmless) {
    auto m = blank_model();
    auto [m1, _] = moha::app::update(std::move(m), moha::Tick{});
    CHECK(m1.s.is_idle());
}

TEST(noop_is_identity) {
    auto m     = blank_model();
    auto cur   = m.ui.composer.text;
    auto [m1, _] = moha::app::update(std::move(m), moha::NoOp{});
    CHECK(m1.ui.composer.text == cur);
}

// ── palette ─────────────────────────────────────────────────────────────

TEST(command_palette_open_close) {
    auto m = blank_model();
    auto [m1, _] = moha::app::update(std::move(m), moha::OpenCommandPalette{});
    CHECK(moha::is_open(m1.ui.command_palette));
    auto [m2, __] = moha::app::update(std::move(m1), moha::CloseCommandPalette{});
    CHECK(!moha::is_open(m2.ui.command_palette));
}

TEST(todo_modal_open_close) {
    auto m = blank_model();
    auto [m1, _] = moha::app::update(std::move(m), moha::OpenTodoModal{});
    CHECK(std::holds_alternative<moha::ui::pick::OpenModal>(m1.ui.todo.open));
    auto [m2, __] = moha::app::update(std::move(m1), moha::CloseTodoModal{});
    CHECK(std::holds_alternative<moha::ui::pick::Closed>(m2.ui.todo.open));
}

} // namespace

int main() {
    install_stub_deps();
    int total = 0, failed_tests = 0;
    for (auto& t : registry()) {
        int fails = 0;
        t.run(fails);
        ++total;
        if (fails > 0) {
            ++failed_tests;
            std::printf("[FAIL] %.*s  (%d assertion%s)\n",
                        static_cast<int>(t.name.size()), t.name.data(),
                        fails, fails == 1 ? "" : "s");
        } else {
            std::printf("[ ok ] %.*s\n",
                        static_cast<int>(t.name.size()), t.name.data());
        }
    }
    std::printf("\n%d test%s, %d failed.\n",
                total, total == 1 ? "" : "s", failed_tests);
    return failed_tests == 0 ? 0 : 1;
}
