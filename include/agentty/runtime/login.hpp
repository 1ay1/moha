#pragma once
// agentty::ui::login — the in-app authentication modal's state machine.
//
// Same shape as the other picker variants in `runtime/panel/common.hpp`: a
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
//   CustomHostInput — user is typing a raw `host[:port]` for an
//                    OpenAI-compatible backend (llama.cpp, vLLM, a
//                    remote box). No auth; submit switches the provider
//                    to that endpoint directly.
//   Failed         — error toast; press any key to return to Picking.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "agentty/auth/auth.hpp"

namespace agentty::ui::login {

struct Closed {};

// Where Esc lands from a sub-modal — the ONE-LEVEL parent, modelled as a
// FIRST-CLASS FRAME that carries its OWN context, not a lossy tag.
//
// The login flow is a stack the user walks INTO (provider picker → account
// list → key input); Esc walks back OUT one step, rebuilding the parent with
// its FULL context. The old `Back` enum could only name a KIND of parent
// ("AccountList") — it lost WHICH provider's list, so Esc from Mistral's key
// input wrongly reopened the ACTIVE provider's accounts. By making each parent
// carry its data, "return to the wrong provider" becomes UNREPRESENTABLE:
// the frame that opened Mistral's key input holds `Accounts{"mistral"}`, so
// Esc provably returns there.
namespace origin {
    struct Nowhere {};                            // no parent — close the modal.
    struct ProviderPicker {};                     // reopen the provider picker.
    struct FusedPicker {};                         // reopen the fused model picker.
    struct Accounts { std::string provider; };     // reopen THIS provider's accounts.
    struct Method   { std::string provider; };     // the sign-in method menu (Picking).
    struct HostInput { std::string spec; };        // restore the typed custom host.
}
// A self-contained "where I came from". Every enterable sub-state stores one;
// LoginBack std::visits it — no side-channel params, no active-provider guess.
using Origin = std::variant<origin::Nowhere, origin::ProviderPicker,
                            origin::FusedPicker, origin::Accounts,
                            origin::Method, origin::HostInput>;

// True when Esc should read "back" (has a parent) rather than "cancel".
[[nodiscard]] inline bool has_parent(const Origin& o) noexcept {
    return !std::holds_alternative<origin::Nowhere>(o);
}

// Optional provider context keeps an "add another account" flow scoped to
// the provider it came from. Empty means the general first-run/sign-in menu.
struct Picking {
    std::string provider;
    Origin origin = origin::Nowhere{};
};

struct OAuthCode {
    agentty::auth::PkceVerifier verifier;
    agentty::auth::OAuthState   state;
    std::string              authorize_url;   // shown to the user as a fallback
    std::string              code_input;
    int                      cursor = 0;
};

struct OAuthExchanging {
    // True when this login came from the account manager's "+ Add
    // another account…" flow (origin::Accounts), i.e. the user
    // DELIBERATELY wants a separate slot even if the derived label
    // collides. A plain sign-in leaves this false and reuses the
    // derived-label slot on re-auth (no "OAuth login 2/3/…" churn).
    bool as_new_account = false;
};

// Native ChatGPT (Codex) login is in flight. Local sessions wait for the
// port-1455 browser callback. SSH sessions use device authorization and this
// same state is updated with the URL + one-time code as soon as OpenAI issues
// them. Esc closes the modal; the bounded worker eventually exits.
struct ChatGptWaiting {
    std::uint64_t                      attempt_id = 0;
    std::shared_ptr<std::atomic_bool> cancel;
    bool                               device_auth = false;
    std::string                        authorize_url;
    std::string                        user_code;
};

// A native OAuth **device-flow** login is in flight (GitHub Copilot, Kimi, or
// any future device-flow provider). The provider always issues a one-time code
// (device flow only), so the modal shows the verification URL + code as soon as
// the DeviceCodeReady message lands. `provider` is the canonical registry id
// ("copilot", "kimi") — it drives the panel title, the completion dispatch, and
// the account label. One state for every device-flow provider so the panel,
// key-handling, and copy shortcuts are written once. Esc closes the modal; the
// bounded worker eventually exits.
struct DeviceWaiting {
    std::string                        provider;      // "copilot" | "kimi" | …
    std::string                        provider_label; // "GitHub Copilot" | "Kimi"
    std::uint64_t                      attempt_id = 0;
    std::shared_ptr<std::atomic_bool> cancel;
    std::string                        authorize_url;  // BARE url shown in the panel (has a code field)
    std::string                        browser_url;     // pre-filled url auto-opened + copied by `u`
    std::string                        user_code;
};

struct ApiKeyInput {
    std::string key_input;
    int         cursor = 0;
    // Which backend this key is for. Empty = Anthropic (saved to
    // credentials.json). A provider id ("openai", "groq", …) routes the
    // submit to Settings.provider_keys + a live provider switch. Carries
    // the human label for the panel header so the view needs no registry
    // lookup.
    std::string provider;        // canonical id; empty = Anthropic
    std::string provider_label;  // display name for the panel title
    Origin origin = origin::Nowhere{};     // Esc target (one level up)
};

// Free-text entry of a raw OpenAI-compatible endpoint ("host" or
// "host:port"). Opened from the provider picker's "Custom host…" row.
// Submit routes through provider::parse_selection(raw) — the same path
// --provider host:port takes — so any llama.cpp / vLLM / remote server
// is reachable from the UI without touching the CLI.
struct CustomHostInput {
    std::string host_input;
    int         cursor = 0;
    Origin origin = origin::Nowhere{};     // Esc target (one level up)
};

// Async connect-probe of a just-entered custom host: the modal shows
// "probing host…" while a background worker dials the endpoint's model
// list (configured path → /v1/models → Ollama /api/tags). HostProbed
// resolves it: success commits the switch with the DETECTED dialect;
// failure returns to CustomHostInput with the typed spec restored and the
// reason shown — the user never commits to a dead endpoint blind.
// `attempt_id` guards against a stale probe result landing after the user
// Esc'd or resubmitted (same pattern as the OAuth attempt ids).
struct HostProbing {
    std::string   spec;          // canonical spec being probed
    std::uint64_t attempt_id = 0;
    Origin origin = origin::Nowhere{};     // where failure/Esc returns to
};

struct Failed {
    std::string message;
};

// One row in the account switcher.
struct AccountRow {
    std::string provider;    // canonical id
    std::string label;       // user-facing name
    bool        active = false;
};

// The in-app account switcher: lists every saved account for the ACTIVE
// provider so the user can switch who they're signed in as — or add a new
// one / remove one — without ever leaving agentty. Selecting a row that
// isn't the active one swaps that account's credential into the live store;
// last row is always "+ Add another account…". ChatGPT launches its native
// OAuth flow directly; Anthropic stays scoped to its API-key/OAuth choices.
struct AccountList {
    std::string             provider;       // provider these rows belong to
    std::string             provider_label; // display name for the header
    std::vector<AccountRow> rows;           // saved accounts (+ synthesized add row is index == rows.size())
    int                     cursor = 0;      // 0..rows.size() (last = add-new)
    std::string             confirm_remove; // label awaiting a second Del/d press
};

using State = std::variant<Closed, Picking, OAuthCode, OAuthExchanging,
                           ChatGptWaiting, DeviceWaiting, ApiKeyInput, CustomHostInput,
                           HostProbing, AccountList, Failed>;

[[nodiscard]] inline bool is_open(const State& s) noexcept {
    return !std::holds_alternative<Closed>(s);
}

[[nodiscard]] inline bool is_input_state(const State& s) noexcept {
    // States that consume free-text key input (vs the Picking choice keys).
    return std::holds_alternative<OAuthCode>(s)
        || std::holds_alternative<ApiKeyInput>(s)
        || std::holds_alternative<CustomHostInput>(s);
}

} // namespace agentty::ui::login
