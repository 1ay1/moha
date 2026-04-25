// moha wire-impersonation smoke test
//
// Fires one real `/v1/messages` request through provider::anthropic::transport
// — the same wire path production uses, header pinning + beta cocktail and
// all — to detect when our Claude Code CLI impersonation falls out of date
// with what api.anthropic.com accepts.
//
// Why this exists: moha's HTTP layer impersonates `claude-cli/2.1.44` so
// OAuth tokens (the `sk-ant-oat01-…` flavour granted to Pro/Max
// subscribers) are accepted. Anthropic's edge gates that path on a
// matching client identity — UA, beta cocktail, x-stainless-* headers,
// body shape. If a future CC release lands new beta gates and we don't
// mirror them, every OAuth-authed user silently breaks. The reducer
// suite would never catch this; it doesn't touch the network.
//
// Usage:
//   cmake -B build -DMOHA_BUILD_WIRE_TESTS=ON
//   cmake --build build --target moha_wire_smoke
//   ./build/moha_wire_smoke
//
// Credentials: resolved through the normal moha auth chain
// (`auth::resolve("")`) — env vars `ANTHROPIC_API_KEY` /
// `CLAUDE_CODE_OAUTH_TOKEN` then `~/.config/moha/credentials.json`.
// OAuth credentials are the strongest test (they exercise the whole
// gated header path); an API key still validates body shape but skips
// the OAuth-only beta gate.
//
// Model: defaults to `claude-sonnet-4-5` so the non-haiku OAuth beta
// gate is actually exercised. Override with `MOHA_WIRE_TEST_MODEL=…`.
//
// Exit codes (autoconf convention):
//   0   PASS   — stream completed cleanly, wire impersonation accepted
//   1   FAIL   — auth-shaped error (401/403/unauthorized), likely wire
//              broken; this is the canary
//   2   INDETERMINATE — non-auth failure (timeout, connection,
//              rate limit, 5xx); CI should warn, not hard-fail
//   77  SKIP   — no usable credentials available

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <variant>

#include "moha/auth/auth.hpp"
#include "moha/domain/conversation.hpp"
#include "moha/provider/anthropic/transport.hpp"
#include "moha/runtime/msg.hpp"

namespace {

const char* env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : fallback;
}

// Lower-cased haystack/needle search — we don't care about the exact
// casing the provider uses for "Unauthorized" / "unauthorized" / etc.
bool contains_ci(const std::string& haystack, const char* needle) {
    auto lower = [](char c) -> char {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
    };
    std::size_t n = std::strlen(needle);
    if (n == 0 || haystack.size() < n) return false;
    for (std::size_t i = 0; i + n <= haystack.size(); ++i) {
        bool match = true;
        for (std::size_t k = 0; k < n; ++k) {
            if (lower(haystack[i + k]) != lower(needle[k])) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

bool looks_like_auth_failure(const std::string& msg) {
    // The provider/transport layer usually surfaces HTTP status as a
    // bare number embedded in the error message ("status 401",
    // "HTTP 403", etc.) plus body text from Anthropic that includes
    // "unauthorized" / "invalid_api_key" / "authentication_error". Any
    // of these means our wire identity was rejected.
    return contains_ci(msg, "401")
        || contains_ci(msg, "403")
        || contains_ci(msg, "unauthorized")
        || contains_ci(msg, "authentication")
        || contains_ci(msg, "invalid_api_key")
        || contains_ci(msg, "invalid_token");
}

} // namespace

int main() {
    using namespace moha;

    // ── 1. Resolve credentials ────────────────────────────────────────
    auth::Credentials creds = auth::resolve(/*cli_api_key=*/"");
    if (!auth::is_valid(creds)) {
        std::fprintf(stderr,
            "[SKIP] no usable credentials — set ANTHROPIC_API_KEY,\n"
            "       CLAUDE_CODE_OAUTH_TOKEN, or run `moha login`.\n");
        return 77;
    }

    const auto cred_kind = auth::persist_tag(creds);   // "api_key" | "oauth"
    std::fprintf(stderr, "[INFO] credentials resolved: %.*s\n",
                 static_cast<int>(cred_kind.size()), cred_kind.data());

    // ── 2. Build the request ──────────────────────────────────────────
    // Use a non-haiku model so the OAuth beta gate
    // (claude-code-20250219, oauth-2025-04-20, prompt-caching-...) is
    // actually exercised. Haiku skips that gate, so a haiku-only test
    // would silently miss wire-format breakage that affects everything
    // else. Override via env for ad-hoc testing.
    const std::string model = env_or("MOHA_WIRE_TEST_MODEL", "claude-sonnet-4-5");
    std::fprintf(stderr, "[INFO] model: %s\n", model.c_str());

    provider::anthropic::Request req;
    req.model         = model;
    req.system_prompt = "Reply with exactly one word: pong.";
    req.max_tokens    = 16;
    req.auth_header   = auth::header_value(creds);
    req.auth_style    = auth::style(creds);

    Message user;
    user.role = Role::User;
    user.text = "ping";
    req.messages.push_back(std::move(user));

    // ── 3. Run the stream and observe ─────────────────────────────────
    std::atomic<bool> got_started{false};
    std::atomic<bool> got_finished{false};
    std::atomic<bool> got_text{false};
    std::string error_msg;

    auto sink = [&](Msg m) {
        std::visit([&]<class T>(const T& e) {
            using D = std::decay_t<T>;
            if constexpr (std::is_same_v<D, StreamStarted>) {
                got_started = true;
            } else if constexpr (std::is_same_v<D, StreamTextDelta>) {
                got_text = true;
            } else if constexpr (std::is_same_v<D, StreamFinished>) {
                got_finished = true;
            } else if constexpr (std::is_same_v<D, StreamError>) {
                error_msg = e.message;
            }
        }, m);
    };

    const auto t0 = std::chrono::steady_clock::now();
    provider::anthropic::run_stream_sync(std::move(req), std::move(sink), {});
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    // ── 4. Verdict ────────────────────────────────────────────────────
    std::fprintf(stderr,
        "[INFO] stream done in %lldms: started=%d text=%d finished=%d error=%s\n",
        static_cast<long long>(elapsed_ms),
        got_started.load(), got_text.load(), got_finished.load(),
        error_msg.empty() ? "<none>" : error_msg.c_str());

    if (!error_msg.empty()) {
        if (looks_like_auth_failure(error_msg)) {
            std::fprintf(stderr,
                "\n[FAIL] auth-shaped error from api.anthropic.com — wire\n"
                "       impersonation likely rejected. Anthropic may have\n"
                "       added a new gate; cross-check src/io/anthropic.cpp\n"
                "       headers/beta against the current Claude Code CLI.\n");
            return 1;
        }
        std::fprintf(stderr,
            "\n[INDETERMINATE] non-auth failure — could be transient\n"
            "       (timeout, network, rate limit, server 5xx) or genuine.\n"
            "       Inspect the message above and re-run before treating\n"
            "       this as a wire-format regression.\n");
        return 2;
    }

    if (!got_started || !got_finished) {
        std::fprintf(stderr,
            "\n[INDETERMINATE] stream ended without explicit error but also\n"
            "       without a clean finish frame (started=%d, finished=%d).\n"
            "       Likely transport-level: connection dropped mid-stream.\n",
            got_started.load(), got_finished.load());
        return 2;
    }

    std::fprintf(stderr, "\n[PASS] wire impersonation accepted.\n");
    return 0;
}
