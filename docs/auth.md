# Authentication

moha supports two authentication methods for the Claude API: **OAuth** (Claude.ai login) and **API key**. OAuth reuses Claude Code's OAuth client, so users with a Pro/Max subscription can authenticate without separate API billing.

## Quick Start

```bash
moha login          # Interactive login (OAuth or API key)
moha status         # Show current auth status
moha logout         # Remove saved credentials
moha -k sk-ant-...  # One-off API-key override for this session
```

CLI flags:

| Flag | Effect |
|---|---|
| `-k`, `--key KEY` | API-key override for this session |
| `-m`, `--model ID` | Model id (e.g. `claude-opus-4-5`) |
| `-h`, `--help` | Print usage |

Subcommand dispatch lives in `src/main.cpp:1400-1435`.

## Authentication Methods

### OAuth (Claude.ai Login)

Uses OAuth 2.0 Authorization Code with PKCE (S256). The token is sent as
`Authorization: Bearer <token>` and enables Pro/Max subscription billing.

**OAuth client config** (`include/moha/auth.hpp:26-34`, `OAuthConfig`):

| Field | Value |
|---|---|
| Client ID | `9d1c250a-e61b-44d9-88ed-5944d1962f5e` |
| Authorize URL | `https://claude.ai/oauth/authorize` |
| Token URL | `https://platform.claude.com/v1/oauth/token` |
| Callback URL | `https://platform.claude.com/oauth/code/callback` |
| Scopes | `user:profile user:inference user:sessions:claude_code user:mcp_servers user:file_upload` |

This is the same OAuth client as Claude Code, which enables token reuse via the
`CLAUDE_CODE_OAUTH_TOKEN` environment variable.

### API Key

Standard Anthropic API keys (`sk-ant-...`). Sent as `x-api-key` header. Uses API billing.

## Credential Resolution

On startup, `auth::resolve(cli_api_key)` (`src/auth.cpp:337-378`) walks this
priority order:

1. **`-k` / `--key`** — CLI flag, API key for a single session
2. **`ANTHROPIC_API_KEY`** env var — API key
3. **`CLAUDE_CODE_OAUTH_TOKEN`** env var — OAuth token (reuse Claude Code's auth)
4. **Credentials file** — `~/.config/moha/credentials.json`
   (or `$XDG_CONFIG_HOME/moha/credentials.json`)

If a stored OAuth token is expired and a refresh token is present, `resolve()`
refreshes inline before returning. If the refresh fails (or there is no refresh
token), it returns an empty `Credentials` and `main()` aborts with a "not
authenticated" message.

## Credentials File

**Path:** `~/.config/moha/credentials.json` (mode `0600`, set via
`restrict_perms()` → `chmod` on POSIX; best-effort on Windows).

```json
{
  "method": "oauth",
  "access_token": "<token>",
  "refresh_token": "<refresh_token>",
  "expires_at": 1712345678000
}
```

- `method` — `"oauth"` or `"api_key"`
- `expires_at` — Unix milliseconds; `0` means no expiration info (API keys never expire)

`config_dir()` honours `XDG_CONFIG_HOME`, then falls back to `$HOME/.config`,
then `$USERPROFILE/.config` on Windows. The directory is created on first read
(`src/auth.cpp:57-73`).

## TLS / Certificate Trust

`auth::apply_tls_options(curl)` is called on every libcurl handle moha creates
(`src/auth.cpp:75-88`). It honours:

| Env var | Effect |
|---|---|
| `CURL_CA_BUNDLE` | `CURLOPT_CAINFO = $CURL_CA_BUNDLE` |
| `SSL_CERT_FILE` | Same, used only if `CURL_CA_BUNDLE` is unset |
| `MOHA_INSECURE=1` | Disables `SSL_VERIFYPEER` and `SSL_VERIFYHOST` (debug only) |

Both `CURL_CA_BUNDLE` and `SSL_CERT_FILE` are commonly set by Nix, certain
corporate VPN proxies, and tools like `mitmproxy`. `MOHA_INSECURE` should never
be set against a real Anthropic endpoint.

## OAuth PKCE Flow

The interactive login (`moha login` → option 1) implements Authorization Code
with PKCE (`src/auth.cpp:409-478`, `cmd_login()`):

```
┌─ moha ────────────────────────────────────────────────────┐
│                                                           │
│  1. verifier  = random_urlsafe(128)                       │
│  2. challenge = base64url_no_pad(SHA-256(verifier))       │
│  3. state     = random_urlsafe(32)                        │
│  4. Build authorize URL with PKCE params + code=true      │
│  5. Open browser (open / xdg-open / ShellExecuteA)        │
│                                                           │
└───────────────────────────┬───────────────────────────────┘
                            │
                            ▼
┌─ Browser ─────────────────────────────────────────────────┐
│                                                           │
│  User logs in at claude.ai/oauth/authorize                │
│  Redirected to platform.claude.com/oauth/code/callback    │
│  Page displays "<code>#<state>" for the user to copy      │
│                                                           │
└───────────────────────────┬───────────────────────────────┘
                            │ user pastes "<code>#<state>"
                            ▼
┌─ moha ────────────────────────────────────────────────────┐
│                                                           │
│  6. exchange_code() splits on '#', keeps the code half    │
│     (src/auth.cpp:299-319)                                │
│                                                           │
│  7. POST application/x-www-form-urlencoded to             │
│     platform.claude.com/v1/oauth/token:                   │
│       grant_type    = "authorization_code"                │
│       code          = "<pasted code>"                     │
│       client_id     = "9d1c250a-..."                      │
│       redirect_uri  = "https://platform.claude.com/..."   │
│       code_verifier = "<128-char verifier>"               │
│       state         = "<32-char random>"                  │
│                                                           │
│  8. Receive: { access_token, refresh_token, expires_in }  │
│  9. expires_at_ms = now_ms() + expires_in * 1000          │
│ 10. save_credentials() → ~/.config/moha/credentials.json  │
│                                                           │
└───────────────────────────────────────────────────────────┘
```

The authorize URL is built with `&code=true`, which is what Claude's OAuth
endpoint uses to render the code on a copy-paste page rather than redirecting
the browser to localhost.

## Token Refresh

OAuth tokens expire (typically after 1 hour). `auth::resolve()` checks
`is_expired()` on startup and calls `refresh_access_token()` if a refresh token
is available (`src/auth.cpp:321-331`):

```
POST https://platform.claude.com/v1/oauth/token
Content-Type: application/x-www-form-urlencoded

grant_type=refresh_token
&client_id=9d1c250a-e61b-44d9-88ed-5944d1962f5e
&refresh_token=<saved_refresh_token>
```

The response provides a new `access_token` and `expires_in`, and optionally a
new `refresh_token`. moha persists the refreshed values to disk and continues.

There is no in-flight refresh during a streaming request — if the token expires
mid-conversation, the server returns 401 and the user must restart moha (which
will refresh on the next startup).

## How Auth Headers Are Attached

The `anthropic::Request` struct carries the resolved auth header
(`include/moha/anthropic.hpp:21-31`):

```cpp
struct Request {
    std::string model;
    std::string system_prompt;
    std::vector<Message> messages;
    std::vector<ToolSpec> tools;
    int max_tokens = 8192;

    // Filled by caller from auth::resolve().
    std::string auth_header;             // "Bearer <t>" or raw API key
    auth::Style auth_style = auth::Style::ApiKey;
};
```

`main.cpp:142-143` populates these from the resolved credentials:

```cpp
req.auth_header = g_creds.header_value();   // "Bearer <t>" for OAuth, raw key otherwise
req.auth_style  = g_creds.style();          // Style::Bearer or Style::ApiKey
```

`anthropic::run_stream_sync()` then chooses the header name and beta flags
(`src/anthropic.cpp:203, 240-253`):

```cpp
const bool is_oauth = (req.auth_style == auth::Style::Bearer);

std::string auth_hdr = is_oauth
    ? (std::string("Authorization: ") + req.auth_header)
    : (std::string("x-api-key: ") + req.auth_header);
headers = curl_slist_append(headers, auth_hdr.c_str());
headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");
if (is_oauth) {
    headers = curl_slist_append(headers,
        "anthropic-beta: oauth-2025-04-20,prompt-caching-2024-07-31,"
        "context-management-2025-06-27,compact-2026-01-12");
} else {
    headers = curl_slist_append(headers,
        "anthropic-beta: prompt-caching-2024-07-31,"
        "context-management-2025-06-27,compact-2026-01-12");
}
```

The OAuth path adds `oauth-2025-04-20` to the `anthropic-beta` list — the API
key path omits it.

### OAuth System Prompt Billing Header

When `is_oauth`, `run_stream_sync()` rewrites `body["system"]` from a plain
string into a two-element text-block array, prepending a billing-tracking block
(`src/anthropic.cpp:210-227`):

```json
[
  { "type": "text",
    "text": "x-anthropic-billing-header: cc_version=0.1.0; cc_entrypoint=cli; cch=00000;" },
  { "type": "text",
    "text": "<actual system prompt>",
    "cache_control": { "type": "ephemeral" } }
]
```

This is **not** an HTTP header — it's a content block in the request body.
Despite the `x-anthropic-billing-header:` prefix, the API recognises the block
itself and routes the request to Pro/Max subscription billing.

API-key requests skip this entirely: `body["system"] = req.system_prompt;` (a plain string).

## Error Handling

Authentication failures (HTTP 401/403) bubble up as a `StreamError` from
`run_stream_sync()` and surface in the TUI. The user must run `moha login`
again (or fix the env var). They are **not** automatically retried — there's no
recovery moha can do for a bad/revoked token mid-stream.

Network errors and overload (5xx) within a single request are surfaced as
`StreamError` events to the UI loop, which is responsible for any retry policy.

## Data Structures

### `Credentials` (`include/moha/auth.hpp:13-23`)

```cpp
enum class Method { None, ApiKey, OAuth };
enum class Style  { ApiKey, Bearer };

struct Credentials {
    Method method = Method::None;
    std::string access_token;
    std::string refresh_token;
    int64_t expires_at_ms = 0;          // 0 = no expiration info (api_key)

    bool is_valid() const;              // access_token not empty
    bool is_expired() const;            // OAuth-only; checks now_ms() >= expires_at_ms
    std::string header_value() const;   // "Bearer <t>" for OAuth, raw key otherwise
    Style style() const;                // OAuth → Bearer, ApiKey → ApiKey
};
```

JSON disk format uses `"expires_at"` as the key for the `expires_at_ms` field
(historical naming — value is still milliseconds).

### `OAuthConfig` (`include/moha/auth.hpp:26-34`)

A `struct` of `static constexpr const char*` constants for the OAuth client.
Not instantiated — accessed as `OAuthConfig::client_id`, etc.

### `TokenResponse` (`include/moha/auth.hpp:56-62`)

Result of `exchange_code()` and `refresh_access_token()`:

```cpp
struct TokenResponse {
    bool ok = false;
    std::string error;
    std::string access_token;
    std::string refresh_token;
    int64_t expires_in_s = 0;
};
```

## Full Flow: Startup to Authenticated API Call

```
main()                                                    (src/main.cpp:1400)
  ├─ parse args                                           — -k, -m, subcommand
  ├─ subcommand?
  │   ├─ "login"   → auth::cmd_login()                   (src/auth.cpp:409)
  │   ├─ "logout"  → auth::cmd_logout()                  (src/auth.cpp:480)
  │   ├─ "status"  → auth::cmd_status()                  (src/auth.cpp:493)
  │   └─ "help"    → print_usage()
  │
  ├─ g_creds = auth::resolve(cli_key)                    (src/auth.cpp:337)
  │   ├─ -k flag?                  → ApiKey
  │   ├─ ANTHROPIC_API_KEY?        → ApiKey
  │   ├─ CLAUDE_CODE_OAUTH_TOKEN?  → OAuth (no refresh token)
  │   ├─ credentials.json?         → load_credentials()
  │   │   └─ OAuth + expired + refresh_token
  │   │       → refresh_access_token() + save_credentials()
  │   └─ none / refresh failed     → Credentials{} (invalid)
  │
  ├─ if !g_creds.is_valid() → print "not authenticated" + exit 1
  │
  ├─ persistence::load_settings(), apply --model override
  │
  └─ maya::run<MohaApp>()                                 — TUI event loop
        │
        └─ user submits message
              │
              └─ build anthropic::Request:
                  req.auth_header = g_creds.header_value()
                  req.auth_style  = g_creds.style()
                    │
                    └─ anthropic::run_stream_sync(req, sink)
                          ├─ is_oauth = (auth_style == Style::Bearer)
                          ├─ if is_oauth: prepend billing-header text block
                          ├─ Authorization: Bearer ...   (or)  x-api-key: ...
                          ├─ anthropic-beta: + oauth-2025-04-20 if OAuth
                          ├─ POST https://api.anthropic.com/v1/messages
                          └─ stream SSE events back through sink
```

## File Index

| File | Purpose |
|---|---|
| `include/moha/auth.hpp` | Public types: `Method`, `Style`, `Credentials`, `OAuthConfig`, `TokenResponse`; function declarations |
| `src/auth.cpp` | All auth logic: paths, TLS opts, load/save/clear, PKCE helpers, token exchange/refresh, `resolve()`, login/logout/status subcommands |
| `include/moha/anthropic.hpp` | `Request` struct (carries `auth_header` + `auth_style`), `run_stream_sync()` |
| `src/anthropic.cpp` | Attaches auth headers, rewrites system prompt with billing block for OAuth |
| `src/main.cpp` | CLI arg parsing, subcommand dispatch, calls `auth::resolve()`, wires `Credentials` into each `Request` |
