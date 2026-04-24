#pragma once
// moha::ui::login — the in-app authentication modal's state machine.
//
// Same shape as the other picker variants in `runtime/picker.hpp`: a
// closed sum type so the validity of each state's data is enforced
// by the type system rather than by hand-maintained invariants.
//
// Five states cover the full flow:
//
//   Closed         — modal not shown.
//   Picking        — choose OAuth (1) or paste API key (2).
//   OAuthCode      — browser opened; user is pasting the callback code.
//                    Carries the PKCE verifier + state needed to
//                    exchange the code on submit.
//   OAuthExchanging — code submitted; HTTP POST to /oauth/token in flight.
//   ApiKeyInput    — user is typing an `sk-ant-...` key.
//   Failed         — error toast; press any key to return to Picking.

#include <string>
#include <variant>

#include "moha/auth/auth.hpp"

namespace moha::ui::login {

struct Closed {};

struct Picking {};

struct OAuthCode {
    moha::auth::PkceVerifier verifier;
    moha::auth::OAuthState   state;
    std::string              authorize_url;   // shown to the user as a fallback
    std::string              code_input;
    int                      cursor = 0;
};

struct OAuthExchanging {};

struct ApiKeyInput {
    std::string key_input;
    int         cursor = 0;
};

struct Failed {
    std::string message;
};

using State = std::variant<Closed, Picking, OAuthCode, OAuthExchanging,
                           ApiKeyInput, Failed>;

[[nodiscard]] inline bool is_open(const State& s) noexcept {
    return !std::holds_alternative<Closed>(s);
}

[[nodiscard]] inline bool is_input_state(const State& s) noexcept {
    // States that consume free-text key input (vs the Picking choice keys).
    return std::holds_alternative<OAuthCode>(s)
        || std::holds_alternative<ApiKeyInput>(s);
}

} // namespace moha::ui::login
