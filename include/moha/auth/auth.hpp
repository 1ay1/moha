#pragma once
// moha::auth — credential domain. `Credentials` is a sum type
// (None | ApiKey | OAuth); each alternative owns the data that's
// only meaningful in that state. Token-flow operations return
// `std::expected<OAuthToken, OAuthError>` — no parallel `bool ok`
// flag, no string error fields.
//
// The adapter that loads credentials from disk lives in
// `src/io/auth.cpp`, and the provider-specific OAuth wiring
// (client_id, token URLs, PKCE exchange) lives in
// `moha/provider/anthropic/oauth.hpp`.
//
// `resolve()` is the one orchestration point — it picks the
// best-available credential (CLI key > env > OAuth-from-disk) and
// refreshes expired OAuth tokens in place. It has to live somewhere
// that can pull the adapter layers together, so it's declared here
// and defined in `src/io/auth.cpp`.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "moha/domain/id.hpp"

namespace moha::auth {

// Wire-format auth header style. Bearer for OAuth, raw key for API.
// Picked by callers (e.g. `provider::anthropic::transport`) to pick
// the correct header name (`x-api-key` vs `authorization`).
enum class Style : std::uint8_t { ApiKey, Bearer };

// Strong newtypes over the raw secret strings shuttled through the OAuth
// dance. All four are "some opaque hex" at runtime and the function signature
// `exchange_code(code, verifier, state)` is exactly the kind of place a
// caller can swap two arguments and the compiler used to wave it through.
struct OAuthCodeTag    {};
struct PkceVerifierTag {};
struct OAuthStateTag   {};
struct RefreshTokenTag {};
using OAuthCode    = Id<OAuthCodeTag>;
using PkceVerifier = Id<PkceVerifierTag>;
using OAuthState   = Id<OAuthStateTag>;
using RefreshToken = Id<RefreshTokenTag>;

// ── Credentials: sum type, not enum-with-fields ──────────────────────────
// Each alternative owns exactly the data valid in that state. Code that
// reads credentials uses `std::visit` (or the helpers below) instead of
// switching on a Method enum and remembering which fields are populated.
namespace cred {
struct None {};
struct ApiKey {
    std::string key;
};
struct OAuth {
    std::string access_token;
    std::string refresh_token;
    // Unix epoch milliseconds. 0 means "no expiration info"; refresh
    // is skipped when the upstream didn't include `expires_in`.
    std::int64_t expires_at_ms = 0;
};
} // namespace cred

using Credentials = std::variant<cred::None, cred::ApiKey, cred::OAuth>;

// Free helpers — the variant is the truth, these are derived views.
[[nodiscard]] bool        is_valid(const Credentials& c) noexcept;
[[nodiscard]] bool        is_expired(const Credentials& c) noexcept;
[[nodiscard]] std::string header_value(const Credentials& c);
[[nodiscard]] Style       style(const Credentials& c) noexcept;

// Disk serialization tag. "oauth" / "api_key" / "none" — used by
// load_credentials/save_credentials and by `cmd_status`.
[[nodiscard]] std::string_view persist_tag(const Credentials& c) noexcept;

// ── Paths ────────────────────────────────────────────────────────────────
[[nodiscard]] std::filesystem::path config_dir();              // ~/.config/moha on Unix
[[nodiscard]] std::filesystem::path credentials_path();

// ── Prewarm ──────────────────────────────────────────────────────────────
// Open a TCP+TLS+h2 connection to api.anthropic.com on a detached background
// thread, parking the result in the http client's pool. By the time the user
// hits Enter, the first real request reuses that connection and skips the
// ~150–300 ms of cold handshake. Idempotent (no-op after first call).
void prewarm_anthropic();

// ── Disk I/O ─────────────────────────────────────────────────────────────
[[nodiscard]] std::optional<Credentials> load_credentials();
bool save_credentials(const Credentials& c);     // writes with 0600 perms where supported
bool clear_credentials();

// ── PKCE helpers (exposed for tests) ─────────────────────────────────────
[[nodiscard]] std::string random_urlsafe(std::size_t n);
[[nodiscard]] std::string base64url_no_pad(const unsigned char* data, std::size_t len);
[[nodiscard]] std::string sha256_hex(const std::string& s);
[[nodiscard]] std::string code_challenge_s256(const std::string& verifier);

// ── Token operations ─────────────────────────────────────────────────────
struct OAuthToken {
    std::string  access_token;
    std::string  refresh_token;
    std::int64_t expires_in_s = 0;   // server-reported lifetime, 0 = no info
};

enum class OAuthErrorKind : std::uint8_t {
    Network,        // HTTP transport failure — no response from server
    BadResponse,    // server replied but body wasn't valid JSON
    ApiError,       // server returned an OAuth error_description
    MissingToken,   // 200 OK but no access_token field
};

struct OAuthError {
    OAuthErrorKind kind = OAuthErrorKind::ApiError;
    std::string    detail;
    [[nodiscard]] std::string render() const;
};

using TokenResult = std::expected<OAuthToken, OAuthError>;

[[nodiscard]] TokenResult exchange_code(const OAuthCode& code,
                                        const PkceVerifier& verifier,
                                        const OAuthState& state);
[[nodiscard]] TokenResult refresh_access_token(const RefreshToken& refresh_token);

// Build the claude.ai authorize URL the user must visit to grant moha
// access. Pure: same inputs, same URL — no side effects, safe to call
// from the reducer. The in-app login modal calls this once when it
// transitions Picking → OAuthCode so the URL can be both shown to the
// user (fallback if the browser launch fails) and handed to
// `open_browser` via a Cmd::task.
[[nodiscard]] std::string oauth_authorize_url(const PkceVerifier& verifier,
                                              const OAuthState&   state);

// Fire-and-forget: open the user's default browser pointing at `url`.
// Uses `xdg-open` / `open` / `ShellExecute` depending on platform.
// Always returns immediately — the system command is backgrounded with
// `&`, so this is safe to call from the UI thread, but it is also fine
// (and cleaner) to wrap in a Cmd::task so a wedged opener doesn't
// stall the reducer tick.
void open_browser(const std::string& url);

// Resolve credentials following the documented priority order.
// `cli_api_key` (from `-k`) takes top priority if non-empty. Auto-refresh
// expired OAuth tokens when refresh_token is available.
[[nodiscard]] Credentials resolve(const std::string& cli_api_key);

// ── Interactive CLI flows (blocking, stdout/stdin — NOT in TUI) ──────────
int cmd_login();
int cmd_logout();
int cmd_status();

} // namespace moha::auth
