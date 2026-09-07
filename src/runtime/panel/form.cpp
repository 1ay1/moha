// form.cpp — the shared form reducer.
//
// State and behaviour only: no glyphs, no colours, no layout. Everything here
// is a pure mutation on Form, so the whole interaction model is testable
// without a terminal, and maya::Panel remains the single owner of appearance.
//
// UTF-8 is handled without a text-shaping dependency: fields hold host names,
// model ids, file paths and API keys, and a user pasting a non-ASCII path must
// not be able to split a code point and corrupt the buffer. Cursor motion and
// deletion therefore step over whole code points by inspecting continuation
// bytes (0b10xxxxxx).

#include "agentty/runtime/panel/form.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace agentty::form {

namespace {

[[nodiscard]] bool is_continuation(unsigned char c) noexcept {
    return (c & 0xC0u) == 0x80u;
}

[[nodiscard]] std::size_t prev_boundary(const std::string& s, std::size_t pos) {
    if (pos == 0) return 0;
    std::size_t i = pos - 1;
    while (i > 0 && is_continuation(static_cast<unsigned char>(s[i]))) --i;
    return i;
}

[[nodiscard]] std::size_t next_boundary(const std::string& s, std::size_t pos) {
    if (pos >= s.size()) return s.size();
    std::size_t i = pos + 1;
    while (i < s.size() && is_continuation(static_cast<unsigned char>(s[i]))) ++i;
    return i;
}

// Code points in [0, byte_pos) — maya renders in characters, we store bytes.
[[nodiscard]] std::size_t chars_before(const std::string& s, std::size_t byte_pos) {
    const std::size_t end = std::min(byte_pos, s.size());
    std::size_t n = 0;
    for (std::size_t i = 0; i < end; ++i)
        if (!is_continuation(static_cast<unsigned char>(s[i]))) ++n;
    return n;
}

[[nodiscard]] std::size_t char_count(const std::string& s) {
    return chars_before(s, s.size());
}

void encode_utf8(std::string& out, char32_t ch) {
    if (ch < 0x80) {
        out.push_back(static_cast<char>(ch));
    } else if (ch < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (ch >> 6)));
        out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
    } else if (ch < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (ch >> 12)));
        out.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (ch >> 18)));
        out.push_back(static_cast<char>(0x80 | ((ch >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
    }
}

// Shared bodies for the (value, cursor) pair Text/Secret/Path all carry.
void insert_at(std::string& value, std::size_t& cursor, char32_t ch) {
    if (cursor > value.size()) cursor = value.size();
    std::string enc;
    encode_utf8(enc, ch);
    value.insert(cursor, enc);
    cursor += enc.size();
}

void backspace_at(std::string& value, std::size_t& cursor) {
    if (cursor > value.size()) cursor = value.size();
    if (cursor == 0) return;
    const std::size_t start = prev_boundary(value, cursor);
    value.erase(start, cursor - start);
    cursor = start;
}

void delete_forward_at(std::string& value, std::size_t& cursor) {
    if (cursor >= value.size()) return;
    const std::size_t end = next_boundary(value, cursor);
    value.erase(cursor, end - cursor);
}

void move_at(const std::string& value, std::size_t& cursor, int delta) {
    if (cursor > value.size()) cursor = value.size();
    while (delta < 0 && cursor > 0)            { cursor = prev_boundary(value, cursor); ++delta; }
    while (delta > 0 && cursor < value.size()) { cursor = next_boundary(value, cursor); --delta; }
}

// Digits-only editing with clamping, expressed as decimal arithmetic rather
// than string surgery: the field can then never HOLD a value outside its
// range — validation by construction instead of validate-later.
void number_insert(field::Number& n, char32_t ch) {
    if (ch < U'0' || ch > U'9') return;
    const std::int64_t digit = static_cast<std::int64_t>(ch - U'0');
    if (n.max > 0 && n.value > (n.max - digit) / 10) { n.value = n.max; return; }
    const std::int64_t next = n.value * 10 + digit;
    n.value = std::clamp(next, n.min, n.max);
}

[[nodiscard]] std::string lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

// Keep the highlight inside the list, and the scroll window near it.
//
// This is a HINT. The real window is only knowable at paint time (it depends on
// the panel's real height), so maya::Panel re-derives it and guarantees the
// highlight is on screen. Duplicating the exact arithmetic here would be two
// owners for one measurement — the shape that made scrollback trims drift.
// All the reducer owes is a sane starting point.
void reclamp(focus::Choosing& d, int count) {
    if (count <= 0) { d.highlighted = 0; d.scroll = 0; return; }
    d.highlighted = std::clamp(d.highlighted, 0, count - 1);
    const int view = kDropdownViewport;
    if (d.highlighted < d.scroll)         d.scroll = d.highlighted;
    if (d.highlighted >= d.scroll + view) d.scroll = d.highlighted - view + 1;
    d.scroll = std::clamp(d.scroll, 0, std::max(0, count - view));
}

} // namespace

// ── Lookup ───────────────────────────────────────────────────────────────

const Field* Form::find(std::string_view id) const noexcept {
    for (const auto& f : fields) if (f.id == id) return &f;
    return nullptr;
}
Field* Form::find(std::string_view id) noexcept {
    for (auto& f : fields) if (f.id == id) return &f;
    return nullptr;
}

// ── Projection helpers ───────────────────────────────────────────────────

Options visible_options(const field::Choice& c) {
    Options out;
    for (int i = 0; i < c.count(); ++i) {
        const auto k = static_cast<std::size_t>(i);
        out.labels.push_back(c.labels[k]);
        out.hints.push_back(k < c.hints.size() ? c.hints[k] : std::string{});
    }
    return out;
}

std::size_t secret_filled(const field::Secret& s) noexcept {
    return char_count(s.value);
}

std::size_t caret_chars(const FieldValue& v, bool editing) noexcept {
    if (!editing) return std::string::npos;
    return std::visit([](const auto& f) -> std::size_t {
        using T = std::decay_t<decltype(f)>;
        if constexpr (std::is_same_v<T, field::Text>
                   || std::is_same_v<T, field::Secret>
                   || std::is_same_v<T, field::Path>)
            return chars_before(f.value, f.cursor);
        else
            return std::string::npos;
    }, v);
}

// ── Editing ──────────────────────────────────────────────────────────────

void insert(FieldValue& v, char32_t ch) {
    std::visit([&](auto& f) {
        using T = std::decay_t<decltype(f)>;
        if constexpr (std::is_same_v<T, field::Text>
                   || std::is_same_v<T, field::Secret>
                   || std::is_same_v<T, field::Path>)
            insert_at(f.value, f.cursor, ch);
        else if constexpr (std::is_same_v<T, field::Number>)
            number_insert(f, ch);
    }, v);
}

void backspace(FieldValue& v) {
    std::visit([&](auto& f) {
        using T = std::decay_t<decltype(f)>;
        if constexpr (std::is_same_v<T, field::Text>
                   || std::is_same_v<T, field::Secret>
                   || std::is_same_v<T, field::Path>)
            backspace_at(f.value, f.cursor);
        else if constexpr (std::is_same_v<T, field::Number>)
            f.value = std::clamp<std::int64_t>(f.value / 10, f.min, f.max);
    }, v);
}

void delete_forward(FieldValue& v) {
    std::visit([&](auto& f) {
        using T = std::decay_t<decltype(f)>;
        if constexpr (std::is_same_v<T, field::Text>
                   || std::is_same_v<T, field::Secret>
                   || std::is_same_v<T, field::Path>)
            delete_forward_at(f.value, f.cursor);
    }, v);
}

void move_cursor(FieldValue& v, int delta) {
    std::visit([&](auto& f) {
        using T = std::decay_t<decltype(f)>;
        if constexpr (std::is_same_v<T, field::Text>
                   || std::is_same_v<T, field::Secret>
                   || std::is_same_v<T, field::Path>)
            move_at(f.value, f.cursor, delta);
    }, v);
}

void cursor_home(FieldValue& v) {
    std::visit([&](auto& f) {
        using T = std::decay_t<decltype(f)>;
        if constexpr (std::is_same_v<T, field::Text>
                   || std::is_same_v<T, field::Secret>
                   || std::is_same_v<T, field::Path>)
            f.cursor = 0;
    }, v);
}

void cursor_end(FieldValue& v) {
    std::visit([&](auto& f) {
        using T = std::decay_t<decltype(f)>;
        if constexpr (std::is_same_v<T, field::Text>
                   || std::is_same_v<T, field::Secret>
                   || std::is_same_v<T, field::Path>)
            f.cursor = f.value.size();
    }, v);
}

void paste(FieldValue& v, std::string_view text) {
    // Strip newlines/tabs: these are single-line fields, and a pasted newline
    // used to submit the surrounding form by accident in the hand-rolled
    // inputs this replaces.
    std::string clean;
    clean.reserve(text.size());
    for (char c : text)
        if (c != '\n' && c != '\r' && c != '\t') clean.push_back(c);

    std::visit([&](auto& f) {
        using T = std::decay_t<decltype(f)>;
        if constexpr (std::is_same_v<T, field::Text>
                   || std::is_same_v<T, field::Secret>
                   || std::is_same_v<T, field::Path>) {
            if (f.cursor > f.value.size()) f.cursor = f.value.size();
            f.value.insert(f.cursor, clean);
            f.cursor += clean.size();
        } else if constexpr (std::is_same_v<T, field::Number>) {
            for (char c : clean)
                if (c >= '0' && c <= '9') number_insert(f, static_cast<char32_t>(c));
        }
    }, v);
}

void clear(FieldValue& v) {
    std::visit([&](auto& f) {
        using T = std::decay_t<decltype(f)>;
        if constexpr (std::is_same_v<T, field::Text>
                   || std::is_same_v<T, field::Secret>
                   || std::is_same_v<T, field::Path>) {
            f.value.clear();
            f.cursor = 0;
        } else if constexpr (std::is_same_v<T, field::Number>) {
            f.value = f.min;
        } else if constexpr (std::is_same_v<T, field::Slider>) {
            f.value = f.min;
        }
    }, v);
}

void adjust(FieldValue& v, int dir) {
    std::visit([&](auto& f) {
        using T = std::decay_t<decltype(f)>;
        if constexpr (std::is_same_v<T, field::Toggle>) {
            f.on = !f.on;
        } else if constexpr (std::is_same_v<T, field::Choice>) {
            if (f.count() > 0) f.index = f.normalized(f.index + dir);
        } else if constexpr (std::is_same_v<T, field::Number>) {
            f.value = std::clamp<std::int64_t>(f.value + dir, f.min, f.max);
        } else if constexpr (std::is_same_v<T, field::Slider>) {
            // Snap to the step grid so repeated ±steps can't accumulate
            // floating-point drift into values like 0.6500000000000001.
            const double stepped = f.value + dir * f.step;
            const double snapped = (f.step > 0.0)
                ? std::round(stepped / f.step) * f.step
                : stepped;
            f.value = std::clamp(snapped, f.min, f.max);
        }
    }, v);
}

// ── Form-level operations ────────────────────────────────────────────────

void move(Form& f, int delta) {
    if (f.choosing()) return;          // the dropdown owns the highlight
    if (f.fields.empty()) return;
    const int n = static_cast<int>(f.fields.size());
    const int step = delta >= 0 ? 1 : -1;

    // Headers are labels, not settings: stopping on one is a dead keypress.
    // Skip them, but bound the walk by the row count so an all-header form
    // (or one where every row is a header) terminates rather than spinning.
    int moved = 0;
    int guard = 0;
    while (moved < std::abs(delta) && guard < n) {
        f.cursor = ((f.cursor + step) % n + n) % n;
        ++guard;
        if (!f.fields[static_cast<std::size_t>(f.cursor)].is_header()) ++moved;
    }
}

Activated activate(Form& f) {
    Field* row = f.focused();
    if (!row || row->locked) return Activated::Nothing;

    if (row->is_action()) return Activated::FiredAction;

    // A Pick's candidates live in another overlay — the pane opens it and
    // writes the result back. The form deliberately knows nothing about them.
    if (row->is_pick()) return Activated::HandOff;

    if (row->is_choice()) {
        // Open the dropdown WITHOUT touching the field: the highlight starts
        // on the current selection and the value changes only on commit, so
        // Esc is a true cancel rather than an undo.
        const auto& c = std::get<field::Choice>(row->value);
        focus::Choosing d;
        d.highlighted = c.normalized(c.index);
        reclamp(d, c.count());
        f.focus = d;
        return Activated::OpenedDropdown;
    }

    if (row->is_text_like()) {
        f.focus = focus::Editing{};
        return Activated::StartedEditing;
    }

    adjust(row->value, +1);            // Toggle and anything else adjustable
    f.dirty = true;
    return Activated::Changed;
}

bool escape(Form& f) {
    if (f.choosing()) { f.focus = focus::Browsing{}; return false; }   // cancel
    if (f.editing())  { f.focus = focus::Browsing{}; return false; }   // leave field
    return true;                                                       // close form
}

void dropdown_move(Form& f, int delta) {
    auto* d = f.dropdown();
    const Field* row = f.focused();
    if (!d || !row || !row->is_choice()) return;
    const int n = std::get<field::Choice>(row->value).count();
    if (n <= 0) return;
    d->highlighted = ((d->highlighted + delta) % n + n) % n;
    reclamp(*d, n);
}

bool dropdown_commit(Form& f) {
    auto* d = f.dropdown();
    if (!d) return false;
    Field* row = f.focused();
    if (!row || !row->is_choice()) { f.focus = focus::Browsing{}; return false; }

    auto& c = std::get<field::Choice>(row->value);
    bool changed = false;
    if (c.count() > 0) {
        const int pick = std::clamp(d->highlighted, 0, c.count() - 1);
        changed = (c.normalized(c.index) != pick);
        c.index = pick;
    }
    f.focus = focus::Browsing{};
    if (changed) f.dirty = true;
    return changed;
}

void reset_field(Form& f, const FieldValue& dflt) {
    Field* row = f.focused();
    if (!row || row->locked) return;
    row->value = dflt;
    row->error.clear();
    f.dirty = true;
}

// ── Builder ──────────────────────────────────────────────────────────────

namespace {
void push(Form& form, std::string id, std::string label, std::string help, FieldValue v) {
    Field f;
    f.id    = std::move(id);
    f.label = std::move(label);
    f.help  = std::move(help);
    f.value = std::move(v);
    form.fields.push_back(std::move(f));
}
} // namespace

Builder& Builder::toggle(std::string id, std::string label, bool on, std::string help) {
    push(form_, std::move(id), std::move(label), std::move(help), field::Toggle{on});
    return *this;
}

Builder& Builder::choice(std::string id, std::string label,
                         std::vector<std::string> labels,
                         std::vector<std::string> ids,
                         std::string_view selected,
                         std::string help,
                         std::vector<std::string> hints) {
    field::Choice c;
    c.labels = std::move(labels);
    c.ids    = std::move(ids);
    c.hints  = std::move(hints);
    if (!selected.empty()) c.select_id(selected);
    push(form_, std::move(id), std::move(label), std::move(help), std::move(c));
    return *this;
}

Builder& Builder::number(std::string id, std::string label, std::int64_t value,
                         std::int64_t min, std::int64_t max, std::string help) {
    push(form_, std::move(id), std::move(label), std::move(help),
         field::Number{std::clamp(value, min, max), min, max});
    return *this;
}

Builder& Builder::slider(std::string id, std::string label, double value,
                         double min, double max, double step, std::string help) {
    field::Slider s;
    s.value = std::clamp(value, min, max);
    s.min = min; s.max = max; s.step = step;
    push(form_, std::move(id), std::move(label), std::move(help), s);
    return *this;
}

Builder& Builder::text(std::string id, std::string label, std::string value,
                       std::string help) {
    const auto n = value.size();
    push(form_, std::move(id), std::move(label), std::move(help),
         field::Text{std::move(value), n});
    return *this;
}

Builder& Builder::secret(std::string id, std::string label, std::string value,
                         std::string help) {
    const auto n = value.size();
    push(form_, std::move(id), std::move(label), std::move(help),
         field::Secret{std::move(value), n});
    return *this;
}

Builder& Builder::path(std::string id, std::string label, std::string value,
                       std::string help, bool want_dir) {
    const auto n = value.size();
    push(form_, std::move(id), std::move(label), std::move(help),
         field::Path{std::move(value), n, want_dir});
    return *this;
}

Builder& Builder::action(std::string id, std::string label, std::string help,
                         std::string hint) {
    field::Action a;
    a.hint = std::move(hint);
    push(form_, std::move(id), std::move(label), std::move(help), std::move(a));
    return *this;
}

Builder& Builder::header(std::string label) {
    push(form_, std::string{"__header."} + label, std::move(label), {},
         field::Header{});
    return *this;
}

Builder& Builder::pick(std::string id, std::string label, std::string current,
                       std::string help) {
    field::Pick p;
    p.label = std::move(current);
    push(form_, std::move(id), std::move(label), std::move(help), std::move(p));
    return *this;
}

Builder& Builder::lock(std::string reason) {
    if (!form_.fields.empty()) {
        form_.fields.back().locked = true;
        form_.fields.back().locked_reason = std::move(reason);
    }
    return *this;
}

Builder& Builder::origin(std::string where) {
    if (!form_.fields.empty()) form_.fields.back().origin = std::move(where);
    return *this;
}

} // namespace agentty::form
