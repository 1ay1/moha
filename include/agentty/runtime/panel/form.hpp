#pragma once
// agentty::form — the form component layer.
//
// ONE typed form model + ONE reducer + ONE renderer, so every configuration
// surface in agentty (embeddings, Smart Mode, retrieval stages, future panes)
// is DATA rather than another hand-written picker. Before this, each pane
// re-implemented row navigation, single-line text editing, and its own idea of
// "am I typing or navigating?" — five divergent copies of the same 200 lines.
//
// ── The load-bearing type: FormFocus ─────────────────────────────────────
// A form has exactly THREE interaction modes, and precisely one owns the
// keyboard at any moment. Spelling that as `bool editing` + `bool dropdown_open`
// is two bools encoding three states, with the invariant ("never both") living
// only in comments — the same shape as the stall-watchdog bools and the
// ollama_probed/ollama_ready pair, both of which shipped bugs. So it is a sum
// type, and the VIEW reads the same value the key router does: what renders and
// what receives keys cannot disagree.
//
// ── Choice has two interactions, not two types ───────────────────────────
// A pick-one-of-N field is one value. Whether you cycle it inline with ←/→ or
// open a list with Enter is a matter of taste and option count, not a different
// kind of data — so `Choice` supports BOTH and there is no presentation flag to
// set wrong. Short enumerations feel like a toggle; long ones get a searchable
// floating list. Same field, same state, no configuration.
//
// ── Why Secret is its own alternative ────────────────────────────────────
// `Text` + a `masked` bool makes leaking a credential a matter of remembering
// to check a bool at every render site. As a distinct alternative the masking
// is a TYPE-LEVEL fact: display() has no branch that can emit a Secret's
// plaintext, so a renderer physically cannot leak it, and redaction keys on the
// type rather than on a field-name heuristic.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace agentty::form {

// ── Field kinds ──────────────────────────────────────────────────────────

namespace field {

// A boolean. Enter/Space/←/→ all flip it.
struct Toggle {
    bool on = false;
};

// Pick one of a SMALL, CLOSED set — an enum, and nothing else.
//
// ←/→ cycle in place; Enter opens a short floating list. There is no search
// box and no filtering, because the moment a list needs either it is not an
// enum any more and does not belong in a dropdown: it belongs in a full
// picker overlay (see `Pick` below). Keeping that boundary sharp is what
// stops this widget from slowly growing into a second, worse picker.
//
// Rule of thumb: if you cannot name every option in the header comment,
// use `Pick`.
//
// `labels` is what the user sees; `ids` is what the caller stores — kept
// parallel so a form never persists a display string, and a relabelled
// option doesn't silently change the saved value.
struct Choice {
    std::vector<std::string> labels;
    std::vector<std::string> ids;      // may be empty ⇒ ids == labels
    std::vector<std::string> hints;    // optional per-option dim blurb
    int                      index = 0;

    [[nodiscard]] int count() const noexcept { return static_cast<int>(labels.size()); }
    [[nodiscard]] int normalized(int i) const noexcept {
        const int n = count();
        return n <= 0 ? 0 : ((i % n) + n) % n;
    }
    [[nodiscard]] std::string_view label() const noexcept {
        if (labels.empty()) return {};
        return labels[static_cast<std::size_t>(normalized(index))];
    }
    // The STORED value for the current selection.
    [[nodiscard]] std::string_view id() const noexcept {
        if (labels.empty()) return {};
        const auto i = static_cast<std::size_t>(normalized(index));
        return ids.empty() ? std::string_view{labels[i]} : std::string_view{ids[i]};
    }
    [[nodiscard]] std::string_view hint_at(int i) const noexcept {
        if (hints.empty()) return {};
        const auto k = static_cast<std::size_t>(normalized(i));
        return k < hints.size() ? std::string_view{hints[k]} : std::string_view{};
    }
    // Select by stored id; leaves the index alone when absent, so loading a
    // config written by a newer agentty doesn't silently reset the field.
    void select_id(std::string_view want) noexcept {
        const auto& keys = ids.empty() ? labels : ids;
        for (std::size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == want) { index = static_cast<int>(i); return; }
    }
};

// A bounded integer, edited digit-by-digit. Clamped on every mutation, so the
// field can never HOLD a value the validator would later reject.
struct Number {
    std::int64_t value = 0;
    std::int64_t min   = 0;
    std::int64_t max   = 65535;
};

// A bounded real, stepped with ←/→ and drawn as a bar. Distinct from Number
// because typing "0.65" one digit at a time is miserable, and because the
// knobs that need it (mmr_lambda, dedup_threshold, dense_weight,
// proactive_min_conf …) are ratios whose value is best judged visually.
struct Slider {
    double value = 0.0;
    double min   = 0.0;
    double max   = 1.0;
    double step  = 0.05;
    int    decimals = 2;
};

// Free single-line text.
struct Text {
    std::string value;
    std::size_t cursor = 0;
};

// Free text that must never be rendered, logged, or persisted in the clear.
struct Secret {
    std::string value;
    std::size_t cursor = 0;
};

// A filesystem path. Text plus an existence probe, so "model file not found"
// is visible while typing rather than as a failed probe later.
struct Path {
    std::string value;
    std::size_t cursor = 0;
    bool        want_dir = false;
};

// A row whose value comes from ANOTHER overlay.
//
// The counterpart to `Choice`: when the candidate set is large, dynamic, or
// needs searching (models, files, threads), Enter closes nothing and hands
// off to a real picker — exactly what Smart Mode already does when you set a
// role's model. The form only holds the CURRENT display label; the pane owns
// the hand-off and writes the result back.
//
// This is the escape hatch that keeps `Choice` honest: there is never a
// reason to grow the dropdown into a searchable list, because that case has
// its own row kind.
struct Pick {
    std::string label;         // current selection, for display
    std::string placeholder = "\xe2\x80\x94";
};

// A row that is a SECTION HEADER, not a setting. Grouping is a rendering
// concern the form otherwise has no way to express, and faking it with a
// locked Text row leaked a stray "—" placeholder into the value column.
struct Header {};

// A row that DOES something rather than holding a value (e.g. "Test
// connection"). A real alternative, not a Toggle that is never rendered as
// one: the reducer dispatches on the type, and `status` carries the result
// so the outcome renders where the action lives.
struct Action {
    std::string status;        // rendered right-aligned; empty ⇒ the hint
    std::string hint = "press Enter";
    enum class Tone : std::uint8_t { Neutral, Busy, Good, Bad };
    Tone tone = Tone::Neutral;
};

} // namespace field

using FieldValue = std::variant<field::Toggle, field::Choice, field::Number,
                                field::Slider, field::Text, field::Secret,
                                field::Path, field::Pick, field::Header,
                                field::Action>;

// Rows a dropdown shows before it scrolls. An enum list should fit well
// inside this; the window exists so a pathologically short terminal degrades
// gracefully, not as a browsing feature.
inline constexpr int kDropdownViewport = 8;

// ── A row ────────────────────────────────────────────────────────────────

struct Field {
    std::string id;        // stable key the reducer + caller address it by
    std::string label;
    std::string help;      // one-line description, dim
    FieldValue  value;

    // Rendered right of the value in a dim tone — used for provenance
    // ("env: AGENTTY_RAG_MMR_LAMBDA", "default") so a user can see WHERE a
    // value came from, the single most confusing thing about layered config.
    std::string origin;

    // A locked row is visible but not editable, and says why. An env var that
    // overrides settings.json must not silently swallow an edit.
    bool        locked = false;
    std::string locked_reason;

    // Per-row validity, rendered inline under the form. Empty ⇒ valid.
    std::string error;

    [[nodiscard]] bool is_text_like() const noexcept {
        return std::holds_alternative<field::Text>(value)
            || std::holds_alternative<field::Secret>(value)
            || std::holds_alternative<field::Path>(value)
            || std::holds_alternative<field::Number>(value);
    }
    [[nodiscard]] bool is_choice() const noexcept {
        return std::holds_alternative<field::Choice>(value);
    }
    [[nodiscard]] bool is_pick() const noexcept {
        return std::holds_alternative<field::Pick>(value);
    }
    [[nodiscard]] bool is_action() const noexcept {
        return std::holds_alternative<field::Action>(value);
    }
    [[nodiscard]] bool is_header() const noexcept {
        return std::holds_alternative<field::Header>(value);
    }
    // A Pick hands off to another overlay, an Action runs something, and a
    // Header is not a setting at all; none of the three is edited in place.
    [[nodiscard]] bool editable() const noexcept {
        return !locked && !is_action() && !is_pick() && !is_header();
    }
};

// ── Focus: the three interaction modes ───────────────────────────────────

namespace focus {

// Cursor moves between rows; ←/→ adjust in place.
struct Browsing {};

// A text-like row owns the keyboard: printable keys EDIT rather than navigate.
struct Editing {};

// A floating option list is open over the form. It owns the keyboard and the
// highlight; the underlying field is untouched until the user commits, so Esc
// is a true cancel.
//
// There is no query here: a dropdown only ever holds an enum, and an enum you
// need to search is a `Pick` row that belongs in a real picker instead.
struct Choosing {
    int highlighted = 0;
    int scroll      = 0;
};

} // namespace focus

using FormFocus = std::variant<focus::Browsing, focus::Editing, focus::Choosing>;

// ── The form ─────────────────────────────────────────────────────────────

struct Form {
    std::string        title;
    std::string        subtitle;      // e.g. the effective config summary
    std::vector<Field> fields;
    int                cursor = 0;
    FormFocus          focus{focus::Browsing{}};

    // Set when a value changed since the last commit — lets the view say
    // "unsaved" instead of the user guessing. form_config APPENDS the
    // shared "unsaved · ^S save" marker to the footer whenever this is
    // set — panes never spell that themselves.
    bool dirty = false;

    // Footer note. The SHARED key grammar (↑↓ · Enter · ^S · Esc) is
    // rendered by form_config for EVERY form pane — no pane repeats it.
    // This field carries only what is PANE-SPECIFIC: Rag's "a advanced",
    // a validation summary, the armed-remove warning. When set to a
    // TRANSIENT WARNING (armed remove), the pane's reducer owns clearing
    // it — the projection renders it INSTEAD of the grammar so it is
    // unmissable.
    std::string note;
    // When true, `note` REPLACES the shared grammar line instead of
    // preceding it — for arm/confirm warnings that must be the only
    // thing the footer says.
    bool note_replaces_grammar = false;

    [[nodiscard]] const Field* focused() const noexcept {
        if (cursor < 0 || cursor >= static_cast<int>(fields.size())) return nullptr;
        return &fields[static_cast<std::size_t>(cursor)];
    }
    [[nodiscard]] Field* focused() noexcept {
        if (cursor < 0 || cursor >= static_cast<int>(fields.size())) return nullptr;
        return &fields[static_cast<std::size_t>(cursor)];
    }
    [[nodiscard]] const Field* find(std::string_view id) const noexcept;
    [[nodiscard]] Field*       find(std::string_view id) noexcept;

    [[nodiscard]] bool editing()  const noexcept {
        return std::holds_alternative<focus::Editing>(focus);
    }
    [[nodiscard]] bool choosing() const noexcept {
        return std::holds_alternative<focus::Choosing>(focus);
    }
    [[nodiscard]] const focus::Choosing* dropdown() const noexcept {
        return std::get_if<focus::Choosing>(&focus);
    }
    [[nodiscard]] focus::Choosing* dropdown() noexcept {
        return std::get_if<focus::Choosing>(&focus);
    }
};

// ── Builder ──────────────────────────────────────────────────────────────
// The ergonomic surface. A whole pane is ~10 lines:
//
//   auto f = form::Builder{"Embeddings"}
//       .choice("backend", "Backend", labels, ids, current_id)
//       .text  ("host",    "Host",    cfg.host,  "hostname or IP")
//       .number("port",    "Port",    cfg.port,  1, 65535)
//       .secret("api_key", "API key", cfg.key,   "stored in the OS keystore")
//       .slider("weight",  "Dense weight", 0.65)
//       .action("test",    "Test connection")
//       .build();
//
// Chaining returns *this by value-reference so a caller never has to name an
// intermediate; `build()` moves the result out.
class Builder {
public:
    explicit Builder(std::string title) { form_.title = std::move(title); }

    Builder& subtitle(std::string s) { form_.subtitle = std::move(s); return *this; }
    // The footer line under the rows: a validation summary, a save hint, or a
    // key affordance. Same field the panes already set directly — exposed on
    // the builder so a form that is built in one pass can say its piece
    // without the caller reaching into the result.
    Builder& note(std::string s) { form_.note = std::move(s); return *this; }

    Builder& toggle(std::string id, std::string label, bool on, std::string help = {});

    // `ids` may be empty ⇒ the labels ARE the ids. `selected` picks by id.
    Builder& choice(std::string id, std::string label,
                    std::vector<std::string> labels,
                    std::vector<std::string> ids = {},
                    std::string_view selected = {},
                    std::string help = {},
                    std::vector<std::string> hints = {});

    Builder& number(std::string id, std::string label, std::int64_t value,
                    std::int64_t min, std::int64_t max, std::string help = {});

    Builder& slider(std::string id, std::string label, double value,
                    double min = 0.0, double max = 1.0, double step = 0.05,
                    std::string help = {});

    Builder& text(std::string id, std::string label, std::string value,
                  std::string help = {});

    Builder& secret(std::string id, std::string label, std::string value,
                    std::string help = {});

    Builder& path(std::string id, std::string label, std::string value,
                  std::string help = {}, bool want_dir = false);

    Builder& action(std::string id, std::string label, std::string help = {},
                    std::string hint = "press Enter");

    // A section header. Skipped by cursor movement — landing on a label that
    // does nothing is a dead keypress.
    Builder& header(std::string label);

    // A row backed by another overlay: Enter hands off to a real picker
    // instead of opening a dropdown. Use this whenever the candidate set is
    // large, dynamic, or needs searching.
    Builder& pick(std::string id, std::string label, std::string current,
                  std::string help = {});

    // Mark the row just added as locked (e.g. pinned by an env var).
    Builder& lock(std::string reason);
    // Provenance for the row just added ("default", "settings.json", "env: X").
    Builder& origin(std::string where);

    [[nodiscard]] Form build() { return std::move(form_); }

private:
    Form form_;
};

// ── Rendering lives in maya, not here ────────────────────────────────────
// This header is STATE + REDUCER only. It knows what a form contains and how
// keys mutate it; it owns no glyph, no column, no colour, no caret drawing.
// The view layer projects a Form onto maya::Form::Config (a mechanical
// field-by-field map) and maya owns every pixel — the same contract Picker
// and the timeline follow. `display()` and friends do NOT live here for that
// reason: a "just this one string" helper in the host is how chrome leaks
// across the boundary.

// ── The option list a dropdown shows ─────────────────────────────────────
// A Choice's options verbatim — no filtering, because a dropdown only holds
// an enum. The view hands this to maya::Form::Menu and the reducer resolves a
// commit against it, so what is displayed and what Enter selects cannot
// diverge.
struct Options {
    std::vector<std::string> labels;
    std::vector<std::string> hints;
};
[[nodiscard]] Options visible_options(const field::Choice& c);

// Character count of a Secret, for maya::form::Secret{filled} — the widget is
// never given the plaintext at all.
[[nodiscard]] std::size_t secret_filled(const field::Secret& s) noexcept;

// Caret as a CHARACTER index (maya renders in characters, the model stores
// bytes). npos when the field is not being edited.
[[nodiscard]] std::size_t caret_chars(const FieldValue& v, bool editing) noexcept;

// ── Editing (pure mutations) ─────────────────────────────────────────────
// Each is a no-op on a kind it doesn't apply to, so call sites never switch
// on the alternative. All of them clamp; none can leave a field invalid.

void insert(FieldValue& v, char32_t ch);
void backspace(FieldValue& v);
void delete_forward(FieldValue& v);
void move_cursor(FieldValue& v, int delta);
void cursor_home(FieldValue& v);
void cursor_end(FieldValue& v);
void paste(FieldValue& v, std::string_view text);

// ── Whole-form paste (SSOT) ─────────────────────────────────
// The one correct way to route a bracketed paste into a form pane. Guards
// against the stale-snapshot race by re-checking the TRUE mode here (the
// input router targets pastes off a FormFocus snapshot that can lag; see
// form_keys.cpp's editing-intent guards for the same rule): only while
// actually editing an editable, unlocked row does the text land. Returns
// true when it did (caller sets pane-specific dirty semantics if any).
// Every form pane's Paste arm MUST be exactly `form::paste_into(o->form,
// e.text)` — a hand-rolled guard is the drift this function exists to end
// (three panes had three copies of it before this).
bool paste_into(Form& f, std::string_view text);

// ── Typed field readers (SSOT) ───────────────────────────────
// Read a field's value by id without spelling the variant access at every
// call site. text_of covers ALL text-like alternatives (Text/Secret/Path)
// so a pane can't silently read "" from a Secret it thought was a Text —
// the exact bug class rag_form's local helper was written to avoid, now
// hoisted so smart_form/plugin_form/rag_form share one definition.
// Missing id or wrong kind ⇒ the neutral value; panes that must
// distinguish "absent" from "empty" use Form::find directly.
[[nodiscard]] std::string text_of  (const Form& f, std::string_view id);
[[nodiscard]] bool        toggle_of(const Form& f, std::string_view id);
// The selected id of a Choice row ("" when absent/not a choice).
[[nodiscard]] std::string choice_of(const Form& f, std::string_view id);
void clear(FieldValue& v);

// ←/→ on a row: flips a Toggle, cycles a Choice, steps a Slider/Number.
void adjust(FieldValue& v, int dir);

// ── Form-level operations ────────────────────────────────────────────────
// The shared reducer body. A per-pane reducer maps its messages onto these
// and then reads the values back out; it never re-implements navigation.

// Move the row cursor (wraps). No-op while a dropdown is open — that mode
// owns the highlight.
void move(Form& f, int delta);
// Home/End: jump to the first/last non-header field WITHOUT wrapping.
void move_edge(Form& f, bool last);
// PgUp/PgDn: header-skipping stride that CLAMPS at the edges (no wrap).
void move_page(Form& f, int delta);

// Enter on the focused row: engage a text field, open a Choice's dropdown,
// flip a Toggle, hand a Pick off to its picker, or report that an Action
// fired.
enum class Activated : std::uint8_t { Nothing, StartedEditing, OpenedDropdown,
                                      Changed, FiredAction, HandOff };
[[nodiscard]] Activated activate(Form& f);

// Esc: leave the innermost mode. Returns true when the form itself should
// close (i.e. we were already Browsing).
[[nodiscard]] bool escape(Form& f);

// Dropdown-only operations. Safe to call in any mode (no-ops elsewhere).
void dropdown_move(Form& f, int delta);
// Commit the highlighted option into the field and return to Browsing.
[[nodiscard]] bool dropdown_commit(Form& f);

// Reset the focused row to `dflt` and mark the form dirty.
void reset_field(Form& f, const FieldValue& dflt);

// ── Layout ───────────────────────────────────────────────────────────────
// Deliberately absent. Where the dropdown floats, how it flips near the
// bottom edge, and how wide it is are LAYOUT decisions — maya::Form resolves
// them from the row set it is already given. A host-side geometry pipeline
// (measure rows here, pass coordinates back in) is exactly the shape that let
// scrollback accounting drift for so long: two owners for one measurement.

} // namespace agentty::form
