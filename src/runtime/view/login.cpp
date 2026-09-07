// In-app login modal.
//
// Five sub-states keyed off `ui::login::State`:
//
//   Picking          choose OAuth (1) or API key (2)
//   OAuthCode        browser opened; user pastes callback code.
//                    The authorize URL gets a dedicated bordered box
//                    + `[c] copy URL` / `[o] open in browser again`
//                    affordances so the URL is one keystroke from the
//                    user's clipboard — no terminal mouse-select needed.
//   OAuthExchanging  HTTP POST in flight.
//   ApiKeyInput      paste an sk-ant-… key.
//   Failed           error toast above the Picking panel.
//
// Sizing is responsive: the outer wrapper sets a `min_width` floor
// and the Overlay widget's default Stretch lets it fill the available
// width (minus 2-col edge padding). Every text node uses TextWrap::Wrap
// so long URLs / API key labels reflow rather than truncate. Mirrors
// the picker chrome (see view/pickers.cpp's wrap_picker).

#include "agentty/runtime/view/login.hpp"

#include <string>
#include <variant>
#include <vector>

#include "agentty/runtime/login.hpp"
#include "agentty/runtime/view/palette.hpp"

#include <maya/widget/panel.hpp>

namespace agentty::ui {

using namespace maya;
using namespace maya::dsl;
// `login::` resolves to `agentty::ui::login::` from this scope without an
// alias — and MSVC rejects an alias whose name shadows the existing
// nested namespace, so don't write one.

namespace {

// Shared key-hint footer. Each sub-state passes the keys that are
// meaningful in its context so the user always sees a complete map.
// Wrapped so long footers reflow on narrow terminals.
Element key_hints(std::initializer_list<std::pair<std::string, std::string>> hints) {
    // Flatten into one TextElement with styled runs so the painter can
    // wrap across rows on a narrow terminal. The previous hstack approach
    // overflowed without ever wrapping.
    std::string content;
    std::vector<StyledRun> runs;
    auto push_run = [&](std::string_view s, Style sty) {
        if (s.empty()) return;
        runs.push_back(StyledRun{
            .byte_offset = content.size(),
            .byte_length = s.size(),
            .style       = sty,
        });
        content.append(s);
    };
    const Style key_sty = Style{}.with_fg(fg).with_bold();
    const Style sep_sty = fg_dim(muted);
    const Style val_sty = fg_dim(muted);
    bool first = true;
    for (const auto& [k, v] : hints) {
        if (!first) push_run("   \xc2\xb7   ", sep_sty);   //   ·
        first = false;
        push_run(k, key_sty);
        push_run(" ", sep_sty);
        push_run(v, val_sty);
    }
    return Element{TextElement{
        .content = std::move(content),
        .style   = Style{}.with_fg(fg),
        .wrap    = TextWrap::Wrap,
        .runs    = std::move(runs),
    }};
}

// Wrap-mode body text — long URLs / explanation lines reflow rather
// than overflowing the modal.
Element body_text(std::string_view content, Style sty) {
    return Element{TextElement{
        .content = std::string{content},
        .style   = sty,
        .wrap    = TextWrap::Wrap,
    }};
}

// Bordered, wrap-mode "URL panel": the URL fills the modal width and
// reflows across rows on narrow terminals, so the user can read every
// character without horizontal scrolling. Background-styled so it
// reads as a dedicated artifact (the thing to copy) rather than as
// inline prose.
Element url_panel(std::string_view url) {
    auto url_text = Element{TextElement{
        .content = std::string{url},
        .style   = Style{}.with_fg(code_path),   // bright_cyan — "this is a path/identifier"
        .wrap    = TextWrap::Wrap,
    }};
    return (v(url_text)
            | padding(0, 1)
            | border(BorderStyle::Round)
            | bcolor(code_path)).build();
}

Element panel_picking(std::string_view provider,
                      bool failed, std::string_view fail_msg) {
    // "Does this provider offer a CHOICE of auth method" is a registry
    // capability (method_menu), not a name — the reducer's gate reads the
    // same flag, so the view and the behaviour cannot drift.
    const auto* prow = provider::preset_for(provider);
    const bool anthropic_only = prow && prow->method_menu;
    std::vector<Element> rows;
    rows.push_back(text(anthropic_only
                            ? "Add a " + std::string{prow->label} + " account"
                            : std::string{"Sign in to agentty"},
                        fg_bold(fg)));
    rows.push_back(body_text(
        anthropic_only
            ? "Choose how to authenticate this new Anthropic account."
            : "Bring your own model. Pick how you want to connect — you can "
              "change this any time from the provider picker.",
        fg_dim(muted)));
    rows.push_back(text(""));
    if (failed) {
        // Wrap-mode danger toast so a long error message stays inside
        // the modal instead of overflowing into the chrome.
        rows.push_back(body_text(
            std::string{"\xE2\x9A\xA0 "} + std::string{fail_msg},
            fg_of(danger)));
        rows.push_back(text(""));
    }
    // Numbered items are flush-left to share the column with the header
    // / description / hint rows. The title sits next to the number badge;
    // the subtitle continuation indents by exactly the badge width ("1) "
    // = 3 chars) so it visually hangs under the title, not under the number.
    //
    // API key is presented FIRST: it's the unambiguous, provider-neutral
    // path (any Anthropic/OpenAI-family key), whereas subscription OAuth is
    // a third-party-client path some users would rather avoid. Leading with
    // the key keeps the un-controversial option one keystroke away.
    rows.push_back(h(text("1) ", fg_bold(highlight)),
                     text("Paste an API key", fg_bold(fg))).build());
    rows.push_back(h(text("   ", fg_of(fg)),
                     body_text("Anthropic sk-ant-…, or any provider's key",
                               fg_dim(muted))).build());
    rows.push_back(text(""));
    rows.push_back(h(text("2) ", fg_bold(highlight)),
                     text("OAuth via claude.ai", fg_bold(fg))).build());
    rows.push_back(h(text("   ", fg_of(fg)),
                     body_text("use your Claude Pro / Max subscription",
                               fg_dim(muted))).build());
    rows.push_back(text(""));
    if (!anthropic_only) {
        rows.push_back(h(text("3) ", fg_bold(highlight)),
                         text("Sign in with ChatGPT", fg_bold(fg))).build());
        rows.push_back(h(text("   ", fg_of(fg)),
                         body_text("use your ChatGPT Plus / Pro (GPT-5 Codex)",
                                   fg_dim(muted))).build());
        rows.push_back(text(""));
        rows.push_back(h(text("4) ", fg_bold(highlight)),
                         text("Custom OpenAI-compatible host", fg_bold(fg)),
                         text("  \xe2\x80\x94 llama.cpp, vLLM, LM Studio, Ollama",
                                   fg_dim(muted))).build());
        rows.push_back(text(""));
        rows.push_back(h(text("5) ", fg_bold(highlight)),
                         text("Sign in with GitHub Copilot", fg_bold(fg))).build());
        rows.push_back(h(text("   ", fg_of(fg)),
                         body_text("use your GitHub Copilot subscription",
                                   fg_dim(muted))).build());
        rows.push_back(text(""));
        rows.push_back(h(text("6) ", fg_bold(highlight)),
                         text("Sign in with Kimi", fg_bold(fg))).build());
        rows.push_back(h(text("   ", fg_of(fg)),
                         body_text("use your Kimi plan (K2 models)",
                                   fg_dim(muted))).build());
        rows.push_back(text(""));
    }
    rows.push_back(key_hints({{anthropic_only ? "1/2" : "1\xe2\x80\x93" "6", "choose"},
                              {"Esc", "close"}}));
    return v(std::move(rows)).build();
}

// One-item Panel for a single-field input state. The field is a REAL
// panel item — Secret (count-only masking, the type-level can't-leak
// guarantee) or Text (caret windowing, so a long paste scrolls under the
// caret instead of walking off the edge). Prose rides as header lines;
// Enter SUBMITS here, so the derived editing hint is overridden.
Element input_panel(std::string title, std::vector<Element> prose,
                    maya::PanelControl control, std::string note,
                    std::string editing_note,
                    std::vector<Element> footer = {}) {
    maya::Panel::Config cfg;
    cfg.title     = std::move(title);
    cfg.accent    = accent;
    cfg.min_width = 48;
    cfg.selected  = 0;
    for (auto& p : prose) cfg.header.push_back(std::move(p));
    cfg.header.push_back(text(""));
    maya::Panel::Item it;
    it.leading = "\xe2\x80\xba";                    // › — the prompt marker
    it.leading_style = fg_bold(accent);
    it.control = std::move(control);
    cfg.items.push_back(std::move(it));
    cfg.footer       = std::move(footer);
    cfg.note         = std::move(note);
    cfg.editing_note = std::move(editing_note);
    return maya::Panel{std::move(cfg)}.build();
}

Element panel_oauth_code(const login::OAuthCode& s) {
    std::vector<Element> prose;
    prose.push_back(body_text(
        "  Step 1 \xe2\x80\x94 open this URL and authorize agentty:",
        fg_dim(muted)));
    prose.push_back(text(""));
    prose.push_back(url_panel(s.authorize_url));
    prose.push_back(text(""));
    prose.push_back(body_text(
        "  Step 2 \xe2\x80\x94 paste the callback code below:",
        fg_dim(muted)));
    // The code is a credential in transit: Secret, count-only.
    maya::panel::Secret sec;
    sec.filled      = s.code_input.size();
    sec.caret       = std::min<std::size_t>(
        static_cast<std::size_t>(std::max(0, s.cursor)), s.code_input.size());
    sec.placeholder = "paste the code from claude.ai";
    std::vector<Element> footer;
    footer.push_back(text(""));
    footer.push_back(key_hints({
        {"c",   "copy URL"},
        {"o",   "open browser"},
        {"^Y",  "copy"},
        {"^O",  "open"},
    }));
    return input_panel(" OAuth via claude.ai ", std::move(prose),
                       std::move(sec),
                       "\xe2\x86\xb5 submit code \xc2\xb7 esc cancel",
                       "\xe2\x86\xb5 submit code \xc2\xb7 esc cancel",
                       std::move(footer));
}

Element panel_oauth_exchanging() {
    std::vector<Element> rows;
    rows.push_back(text("Exchanging authorization code\xE2\x80\xA6",   // …
                        fg_bold(fg)));
    rows.push_back(text(""));
    rows.push_back(body_text(
        "Talking to platform.claude.com — this should take a second.",
        fg_dim(muted)));
    rows.push_back(text(""));
    rows.push_back(key_hints({{"Esc", "cancel"}}));
    return v(std::move(rows)).build();
}

Element panel_chatgpt_waiting(const login::ChatGptWaiting& s) {
    std::vector<Element> rows;
    if (s.device_auth) {
        rows.push_back(text(s.user_code.empty()
                                ? "Requesting ChatGPT device code\xE2\x80\xA6"
                                : "Sign in to ChatGPT",
                            fg_bold(fg)));
        rows.push_back(text(""));
        if (s.user_code.empty()) {
            rows.push_back(body_text(
                "This SSH session uses device authorization. Waiting for "
                "OpenAI to issue a one-time code\xE2\x80\xA6",
                fg_dim(muted)));
        } else {
            rows.push_back(body_text(
                "Open this link in a browser on any device:", fg_dim(muted)));
            rows.push_back(text(""));
            rows.push_back(url_panel(s.authorize_url));
            rows.push_back(text(""));
            rows.push_back(body_text("Then enter this one-time code:",
                                     fg_dim(muted)));
            rows.push_back(text(s.user_code, fg_bold(fg)));
            rows.push_back(text(""));
            rows.push_back(body_text(
                "The code expires in 15 minutes. Continue only because you "
                "started this login in agentty.", fg_dim(muted)));
        }
    } else {
        rows.push_back(text("Waiting for ChatGPT\xE2\x80\xA6", fg_bold(fg)));
        rows.push_back(text(""));
        rows.push_back(body_text(
            "A browser tab has opened at chatgpt.com \xE2\x80\x94 authorize agentty "
            "there. The sign-in completes automatically over a local callback "
            "(http://localhost:1455); you don't need to paste anything.",
            fg_dim(muted)));
        rows.push_back(text(""));
        if (!s.authorize_url.empty()) {
            rows.push_back(body_text(
                "Browser didn't open? Visit this URL manually:", fg_dim(muted)));
            rows.push_back(text(""));
            rows.push_back(url_panel(s.authorize_url));
            rows.push_back(text(""));
        }
    }
    rows.push_back(text(""));
    rows.push_back(key_hints({{"Esc", "cancel"}}));
    return v(std::move(rows)).build();
}

Element panel_device_waiting(const login::DeviceWaiting& s) {
    const std::string label = s.provider_label.empty() ? "your provider"
                                                       : s.provider_label;
    std::vector<Element> rows;
    rows.push_back(text(s.user_code.empty()
                            ? "Requesting device code\xE2\x80\xA6"
                            : "Sign in with " + label,
                        fg_bold(fg)));
    rows.push_back(text(""));
    if (s.user_code.empty()) {
        rows.push_back(body_text(
            "Waiting for " + label + " to issue a one-time code\xE2\x80\xA6",
            fg_dim(muted)));
    } else {
        rows.push_back(body_text(
            "Open this URL to approve the device (it already includes your",
            fg_dim(muted)));
        rows.push_back(body_text(
            "code). On SSH, press [u] to copy it, or scan/type it on any device:",
            fg_dim(muted)));
        rows.push_back(text(""));
        rows.push_back(url_panel(s.authorize_url));
        rows.push_back(text(""));
        rows.push_back(h(text("   if the page asks for a code, enter: ", fg_dim(muted)),
                         text(s.user_code, fg_bold(highlight))).build());
        rows.push_back(text(""));
        rows.push_back(body_text(
            "After signing in to " + label + ", approve the device. The code "
            "expires in ~15 minutes; no API key is needed.", fg_dim(muted)));
        rows.push_back(text(""));
        rows.push_back(key_hints({
            {"c",   "copy code"},
            {"u",   "copy URL"},
            {"o",   "open browser"},
            {"Esc", "cancel"},
        }));
        return v(std::move(rows)).build();
    }
    rows.push_back(text(""));
    rows.push_back(key_hints({{"Esc", "cancel"}}));
    return v(std::move(rows)).build();
}

Element panel_api_key(const login::ApiKeyInput& s) {
    const bool anthropic = s.provider.empty();
    std::vector<Element> prose;
    prose.push_back(body_text(
        anthropic
            ? "  Paste an sk-ant-\xe2\x80\xa6 key. It will be saved to "
              "~/.config/agentty/credentials.json (0600)."
            : "  Paste your " + s.provider_label + " API key to switch to "
              "it. It's saved to ~/.config/agentty settings so you won't "
              "be asked again.",
        fg_dim(muted)));
    // The KEY never reaches the widget — panel::Secret carries a count.
    maya::panel::Secret sec;
    sec.filled      = s.key_input.size();
    sec.caret       = std::min<std::size_t>(
        static_cast<std::size_t>(std::max(0, s.cursor)), s.key_input.size());
    sec.placeholder = anthropic ? "sk-ant-\xe2\x80\xa6" : "paste API key\xe2\x80\xa6";
    const char* back = login::has_parent(s.origin) ? "esc back" : "esc cancel";
    return input_panel(
        " " + (anthropic ? std::string{"Anthropic"} : s.provider_label) + " API key ",
        std::move(prose), std::move(sec),
        std::string{"\xe2\x86\xb5 submit \xc2\xb7 "} + back,
        std::string{"\xe2\x86\xb5 submit \xc2\xb7 "} + back);
}

// Connect probe in flight: name the host and what's being tried so the
// short wait reads as WORK, not a hang. Esc cancels back to the input.
Element panel_host_probing(const login::HostProbing& s) {
    std::vector<Element> rows;
    rows.push_back(text("Checking " + s.spec + "\xe2\x80\xa6", fg_bold(fg)));
    rows.push_back(text(""));
    rows.push_back(body_text(
        "Dialing the server's model list \xe2\x80\x94 the configured path, "
        "then /v1/models, then Ollama's /api/tags \xe2\x80\x94 to detect what "
        "it speaks and confirm it's alive before switching.",
        fg_dim(muted)));
    rows.push_back(text(""));
    rows.push_back(key_hints({{"Esc", "cancel"}}));
    return v(std::move(rows)).build();
}

Element panel_custom_host(const login::CustomHostInput& s) {
    std::vector<Element> prose;
    prose.push_back(body_text(
        "  Enter a host or host:port for any server that speaks the OpenAI "
        "chat API \xe2\x80\x94 llama.cpp, vLLM, LM Studio, a proxy, or a "
        "remote box. A non-443 port uses plain HTTP (the local-server "
        "convention); a bare host uses HTTPS on 443 and defaults to the "
        "/v1 API prefix. For a server on a different prefix, paste the "
        "full URL (https://host/path) and the prefix is honoured verbatim. "
        "Append #name to keep several accounts on the SAME endpoint "
        "(e.g. \xe2\x80\xa6/v1#work and \xe2\x80\xa6/v1#personal \xe2\x80\x94 "
        "each keeps its own API key and model).",
        fg_dim(muted)));
    // Text, not Secret — a host is not a credential, and the caret
    // windowing means a long pasted URL scrolls under the caret.
    maya::panel::Text t;
    t.value       = s.host_input;
    t.caret       = std::min<std::size_t>(
        static_cast<std::size_t>(std::max(0, s.cursor)), s.host_input.size());
    t.placeholder = "localhost:8080";
    const char* back = login::has_parent(s.origin) ? "esc back" : "esc cancel";
    std::vector<Element> footer;
    footer.push_back(text(""));
    footer.push_back(body_text(
        "  Examples:  localhost:8080  \xc2\xb7  https://ollama.com/v1#work  "
        "\xc2\xb7  inference.example.com  \xc2\xb7  https://inference.example.com/api",
        fg_dim(muted)));
    return input_panel(
        " Custom OpenAI-compatible host ", std::move(prose), std::move(t),
        std::string{"\xe2\x86\xb5 connect \xc2\xb7 "} + back,
        std::string{"\xe2\x86\xb5 connect \xc2\xb7 "} + back,
        std::move(footer));
}

// The account switcher, as a real maya::Panel — the same widget every
// picker uses, so the account list gets the panel's cursor bar, badge
// column, viewport scrolling and mode chrome instead of a hand-rolled
// inverse-video list (which the panel merge removed everywhere else).
//
// Item mapping:
//   badge   ✓ on the ACTIVE account (accent) — ⚠ while delete is armed
//   leading label ("#work", "personal…")
//   trailing"press d again to confirm" while armed — SECONDARY, so the
//           label survives narrow terminals
//   + Add another account… rides as the last item, dim until selected
Element panel_account_list(const login::AccountList& s,
                           maya::ScrollState* scroll) {
    maya::Panel::Config cfg;
    cfg.title     = " " + s.provider_label + " accounts ";
    cfg.accent    = accent;
    cfg.min_width = 48;
    // Scroll like every list panel: many accounts on one provider must
    // window inside the modal, not grow it past the terminal edge.
    cfg.scroll     = scroll;
    cfg.viewport_h = 14;
    const int n = static_cast<int>(s.rows.size());
    cfg.selected  = std::min(s.cursor, n);   // last item = add-new

    if (s.rows.empty()) {
        cfg.header.push_back(body_text(
            "  No saved accounts. Add one to sign in without leaving agentty.",
            fg_dim(muted)));
        cfg.header.push_back(text(""));
    }

    for (const auto& r : s.rows) {
        maya::Panel::Item it;
        const bool confirming = (s.confirm_remove == r.label);
        it.leading = r.label;
        it.active  = r.active;
        if (confirming) {
            it.badge           = "\xe2\x9a\xa0";           // ⚠
            it.badge_style     = fg_of(danger);
            it.leading_style   = fg_bold(danger);
            it.trailing        = "press d again to confirm";
            it.trailing_style  = fg_of(danger);
        } else if (r.active) {
            it.badge         = "\xe2\x9c\x93";             // ✓
            it.badge_style   = fg_of(accent);
            it.leading_style = fg_bold(accent);
            it.trailing       = "active";
            it.trailing_style = fg_dim(muted);
        }
        it.trailing_secondary = true;
        cfg.items.push_back(std::move(it));
    }
    {
        maya::Panel::Item add;
        add.leading       = "+ Add another account\xe2\x80\xa6";
        add.leading_style = fg_dim(muted);
        cfg.items.push_back(std::move(add));
    }

    cfg.note = s.confirm_remove.empty()
                   ? "\xe2\x86\x91\xe2\x86\x93 move \xc2\xb7 \xe2\x86\xb5 select \xc2\xb7 d remove \xc2\xb7 esc close"
                   : "d confirm remove \xc2\xb7 any other key cancels";
    return maya::Panel{std::move(cfg)}.build();
}

} // namespace

Element login_modal(const Model& m) {
    if (!login::is_open(m.ui.login)) return nothing();

    Element body = std::visit([&m](const auto& s) -> Element {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::same_as<T, login::Closed>) {
            return nothing();
        } else if constexpr (std::same_as<T, login::Picking>) {
            return panel_picking(s.provider, false, "");
        } else if constexpr (std::same_as<T, login::OAuthCode>) {
            return panel_oauth_code(s);
        } else if constexpr (std::same_as<T, login::OAuthExchanging>) {
            return panel_oauth_exchanging();
        } else if constexpr (std::same_as<T, login::ChatGptWaiting>) {
            return panel_chatgpt_waiting(s);
        } else if constexpr (std::same_as<T, login::DeviceWaiting>) {
            return panel_device_waiting(s);
        } else if constexpr (std::same_as<T, login::ApiKeyInput>) {
            return panel_api_key(s);
        } else if constexpr (std::same_as<T, login::CustomHostInput>) {
            return panel_custom_host(s);
        } else if constexpr (std::same_as<T, login::HostProbing>) {
            return panel_host_probing(s);
        } else if constexpr (std::same_as<T, login::AccountList>) {
            return panel_account_list(s, &m.ui.account_list_scroll);
        } else if constexpr (std::same_as<T, login::Failed>) {
            return panel_picking("", true, s.message);
        }
    }, m.ui.login);

    // Panel-BUILT states (the inputs, the account list) draw their own
    // frame, title and note line — wrapping them again would nest two
    // borders. Only the hand-built prose states (picking, the waiting
    // spinners, failure) still get the generic "Sign in" frame.
    const bool self_framed =
        std::holds_alternative<login::OAuthCode>(m.ui.login)
        || std::holds_alternative<login::ApiKeyInput>(m.ui.login)
        || std::holds_alternative<login::CustomHostInput>(m.ui.login)
        || std::holds_alternative<login::AccountList>(m.ui.login);
    if (self_framed) return body;

    // Responsive sizing: `min_width` floors the modal at a readable
    // width on tiny terminals; the Overlay's default Stretch lets it
    // grow to fill all available columns on wider terminals (minus the
    // Overlay's 2-col edge padding). No max cap — every text node uses
    // TextWrap::Wrap, so URLs and prose reflow naturally to whatever
    // width the terminal gives us. Capping at 96 cols left ~50 cols of
    // empty terminal on a typical 150-col window.
    return vstack()
        .padding(1, 2)
        .min_width(Dimension::fixed(48))
        .border(BorderStyle::Round)
        .border_color(accent)
        .border_text(" Sign in to agentty ",
                     BorderTextPos::Top, BorderTextAlign::Center)
        (std::move(body));
}

} // namespace agentty::ui
