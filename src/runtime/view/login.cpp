// In-app login modal — same overlay shape as the other pickers, but
// state-driven by the closed `ui::login::State` variant. Each
// alternative renders its own panel content; the chrome (border,
// title, key hints) is shared so the layout doesn't shift between
// transitions.

#include "moha/runtime/view/login.hpp"

#include <string>
#include <variant>
#include <vector>

#include "moha/runtime/login.hpp"
#include "moha/runtime/view/palette.hpp"

namespace moha::ui {

using namespace maya;
using namespace maya::dsl;
// `login::` resolves to `moha::ui::login::` from this scope without an
// alias — and MSVC rejects an alias whose name shadows the existing
// nested namespace, so don't write one.

namespace {

// Shared key-hint footer. Each sub-state passes the keys that are
// meaningful in its context so the user always sees a complete map.
Element key_hints(std::initializer_list<std::pair<std::string, std::string>> hints) {
    std::vector<Element> cells;
    bool first = true;
    for (const auto& [k, v] : hints) {
        if (!first) cells.push_back(text("  ", fg_dim(muted)));
        first = false;
        cells.push_back(text(k, fg_of(fg)));
        cells.push_back(text(" ", fg_dim(muted)));
        cells.push_back(text(v, fg_dim(muted)));
    }
    return h(std::move(cells)).build();
}

// Render an inline single-line text input with a block cursor. Mirrors
// the composer's input style. `secret` masks each byte as a bullet so
// keys/codes don't sit in plaintext on screen.
Element input_row(std::string_view value, int cursor, bool secret) {
    std::string display;
    if (secret) {
        // Bytes, not codepoints — a full UTF-8 mask is overkill since
        // OAuth codes and API keys are ASCII.
        display.assign(value.size(), '*');
    } else {
        display.assign(value);
    }
    auto prefix = text("\xE2\x80\xBA ", fg_bold(accent));
    if (cursor < 0) cursor = 0;
    if (cursor > static_cast<int>(display.size())) cursor = static_cast<int>(display.size());
    std::string before = display.substr(0, cursor);
    std::string at_cursor = (cursor < static_cast<int>(display.size()))
        ? std::string{display[cursor]} : std::string{" "};
    std::string after = (cursor + 1 < static_cast<int>(display.size()))
        ? display.substr(cursor + 1) : std::string{};
    return h(
        prefix,
        text(before, fg_of(fg)),
        text(at_cursor, Style{}.with_bold().with_inverse()),
        text(after, fg_of(fg))
    ).build();
}

Element panel_picking(bool failed, std::string_view fail_msg) {
    std::vector<Element> rows;
    rows.push_back(text("Authenticate with Claude", fg_bold(fg)));
    rows.push_back(text(""));
    if (failed) {
        rows.push_back(text(std::string{"\xE2\x9A\xA0 "} + std::string{fail_msg},
                            fg_of(danger)));
        rows.push_back(text(""));
    }
    rows.push_back(h(text("  1) ", fg_bold(highlight)),
                     text("OAuth via claude.ai (Pro/Max)", fg_of(fg))).build());
    rows.push_back(h(text("  2) ", fg_bold(highlight)),
                     text("Paste an Anthropic API key (sk-ant-…)", fg_of(fg))).build());
    rows.push_back(text(""));
    rows.push_back(key_hints({{"1/2", "choose"}, {"Esc", "close"}}));
    return v(std::move(rows)).build();
}

Element panel_oauth_code(const login::OAuthCode& s) {
    std::vector<Element> rows;
    rows.push_back(text("OAuth via claude.ai", fg_bold(fg)));
    rows.push_back(text(""));
    rows.push_back(text("Browser opened. After authorizing, paste the code shown",
                        fg_dim(muted)));
    rows.push_back(text("on the callback page below.", fg_dim(muted)));
    rows.push_back(text(""));
    rows.push_back(text("If the browser didn't open, visit:", fg_dim(muted)));
    rows.push_back(text(s.authorize_url, fg_italic(muted)));
    rows.push_back(text(""));
    rows.push_back(text("Code:", fg_dim(muted)));
    rows.push_back(input_row(s.code_input, s.cursor, /*secret=*/true));
    rows.push_back(text(""));
    rows.push_back(key_hints({{"Enter", "submit"}, {"Esc", "cancel"}}));
    return v(std::move(rows)).build();
}

Element panel_oauth_exchanging() {
    std::vector<Element> rows;
    rows.push_back(text("Exchanging authorization code…", fg_bold(fg)));
    rows.push_back(text(""));
    rows.push_back(text("Talking to platform.claude.com — this should take a second.",
                        fg_dim(muted)));
    rows.push_back(text(""));
    rows.push_back(key_hints({{"Esc", "cancel"}}));
    return v(std::move(rows)).build();
}

Element panel_api_key(const login::ApiKeyInput& s) {
    std::vector<Element> rows;
    rows.push_back(text("Anthropic API key", fg_bold(fg)));
    rows.push_back(text(""));
    rows.push_back(text("Paste an sk-ant-… key. It will be saved to ~/.config/moha.",
                        fg_dim(muted)));
    rows.push_back(text(""));
    rows.push_back(text("Key:", fg_dim(muted)));
    rows.push_back(input_row(s.key_input, s.cursor, /*secret=*/true));
    rows.push_back(text(""));
    rows.push_back(key_hints({{"Enter", "submit"}, {"Esc", "cancel"}}));
    return v(std::move(rows)).build();
}

} // namespace

Element login_modal(const Model& m) {
    if (!login::is_open(m.ui.login)) return nothing();

    Element body = std::visit([](const auto& s) -> Element {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::same_as<T, login::Closed>) {
            return nothing();
        } else if constexpr (std::same_as<T, login::Picking>) {
            return panel_picking(false, "");
        } else if constexpr (std::same_as<T, login::OAuthCode>) {
            return panel_oauth_code(s);
        } else if constexpr (std::same_as<T, login::OAuthExchanging>) {
            return panel_oauth_exchanging();
        } else if constexpr (std::same_as<T, login::ApiKeyInput>) {
            return panel_api_key(s);
        } else if constexpr (std::same_as<T, login::Failed>) {
            return panel_picking(true, s.message);
        }
    }, m.ui.login);

    auto content = (v(std::move(body)) | padding(1, 2) | width(70));
    return (v(content.build())
            | border(BorderStyle::Round) | bcolor(accent)
            | btext(" Sign in to moha ", BorderTextPos::Top, BorderTextAlign::Center)
            ).build();
}

} // namespace moha::ui
