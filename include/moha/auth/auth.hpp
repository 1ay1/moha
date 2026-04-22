#pragma once
// moha::auth — credential domain.  `Credentials`, `Method`, `Style` are
// pure values; the adapter that loads them from disk lives in
// `moha/io/auth_store.hpp`, and the provider-specific OAuth wiring
// (client_id, token URLs, PKCE exchange) lives in
// `moha/provider/anthropic/oauth.hpp`.
//
// `resolve()` is the one orchestration point — it picks the best-available
// credential (CLI key > env > OAuth-from-disk) and refreshes expired OAuth
// tokens in place.  It has to live somewhere that can pull the adapter
// layers together, so it's declared here and defined in `src/io/auth.cpp`.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "moha/domain/id.hpp"

namespace moha::auth {

enum class Method { None, ApiKey, OAuth };
enum class Style  { ApiKey, Bearer };

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

struct Credentials {
    Method method = Method::None;
    std::string access_token;
    std::string refresh_token;
    int64_t expires_at_ms = 0; // 0 = no expiration info (api_key)

    [[nodiscard]] bool is_valid()   const noexcept { return !access_token.empty(); }
    [[nodiscard]] bool is_expired() const;                // only meaningful for OAuth
    [[nodiscard]] std::string header_value() const;       // "Bearer <t>" or raw key
    [[nodiscard]] Style style() const noexcept {
        return method == Method::OAuth ? Style::Bearer : Style::ApiKey;
    }
};

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
struct TokenResponse {
    bool ok = false;
    std::string error;
    std::string access_token;
    std::string refresh_token;
    int64_t expires_in_s = 0;
};
[[nodiscard]] TokenResponse exchange_code(const OAuthCode& code,
                                          const PkceVerifier& verifier,
                                          const OAuthState& state);
[[nodiscard]] TokenResponse refresh_access_token(const RefreshToken& refresh_token);

// Resolve credentials following the documented priority order.
// `cli_api_key` (from `-k`) takes top priority if non-empty. Auto-refresh
// expired OAuth tokens when refresh_token is available.
[[nodiscard]] Credentials resolve(const std::string& cli_api_key);

// ── Interactive CLI flows (blocking, stdout/stdin — NOT in TUI) ──────────
int cmd_login();
int cmd_logout();
int cmd_status();

} // namespace moha::auth
