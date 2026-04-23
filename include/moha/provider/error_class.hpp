#pragma once
// moha::provider::error_class — classify a stream-level failure into
// one of {Transient, RateLimit, Auth, Cancelled, Terminal}. The reducer
// reads the result to decide between auto-retry, re-auth, and surface-
// to-the-user.
//
// Two entry points, by source:
//
//   classify(HttpError)        — typed dispatch on `kind`/`http_status`.
//                                Use this when the source is an HTTP-layer
//                                failure (no SSE event). Zero string
//                                inspection, exhaustive on the enum.
//
//   classify(std::string_view) — substring sniff. Use for SSE
//                                `event: error` payloads where the wire
//                                gives us only Anthropic's `message` text
//                                (e.g. "Overloaded", "rate_limit_error").
//                                The HTTP path is the typed one;
//                                this string path exists for the
//                                wire-only error shape.
//
// Adding a new failure kind is one new enum entry plus one switch arm
// in classify(HttpError); the string-sniff list grows only when
// Anthropic ships a new error_type.

#include <chrono>
#include <string>
#include <string_view>

#include "moha/io/http.hpp"

namespace moha::provider {

enum class ErrorClass {
    // Transient — retryable with backoff. Server is up but momentarily
    // unhappy (load shed, queue full, 5xx). Same request will likely
    // succeed in a few seconds.
    Transient,
    // Rate-limited — retryable with longer backoff. Often carries a
    // Retry-After hint upstream; we use a flat schedule here.
    RateLimit,
    // Authentication — the OAuth token expired mid-session or was
    // revoked. Caller should refresh and retry once; if refresh fails,
    // surface as terminal so user can `moha login`.
    Auth,
    // Cancelled — user pressed Esc; never retry. Final.
    Cancelled,
    // Terminal — invalid request, model not found, billing, etc.
    // Re-sending will fail the same way. Surface to the user and stop.
    Terminal,
};

// Typed dispatch — the preferred path. Reads HttpError::kind directly,
// no string inspection. Use this whenever the failure originates from
// the HTTP layer (Client::send / Client::stream returned `unexpected`).
[[nodiscard]] constexpr ErrorClass classify(const moha::http::HttpError& e) noexcept {
    using K = moha::http::HttpErrorKind;
    switch (e.kind) {
        case K::Cancelled:    return ErrorClass::Cancelled;
        case K::Resolve:
        case K::Connect:
        case K::Tls:
        case K::Protocol:
        case K::SocketHangup:
        case K::Timeout:
        case K::PeerClosed:
            return ErrorClass::Transient;
        case K::Status:
            // Map HTTP-status semantics to ErrorClass. 401/403 are auth;
            // 429 is rate limit; 408/5xx (502/503/504/529) are transient;
            // everything else (4xx) is terminal.
            if (e.http_status == 401 || e.http_status == 403)
                return ErrorClass::Auth;
            if (e.http_status == 429) return ErrorClass::RateLimit;
            if (e.http_status == 408 || e.http_status == 502
             || e.http_status == 503 || e.http_status == 504
             || e.http_status == 529)
                return ErrorClass::Transient;
            return ErrorClass::Terminal;
        case K::Body:
        case K::Unknown:
            return ErrorClass::Terminal;
    }
    return ErrorClass::Terminal;
}

// String-sniff fallback. Used for SSE `event: error` payloads where the
// wire gives us only Anthropic's `error.message` text — there's no
// HttpError to dispatch on at that boundary. Also covers the legacy
// path where pre-typed errors flowed through `StreamError{string}` for
// historical reasons. Keep this list aligned with Anthropic's error
// taxonomy; one entry per shape we want to surface specifically.
[[nodiscard]] inline ErrorClass classify(std::string_view msg) noexcept {
    auto contains = [&](std::string_view needle) noexcept -> bool {
        if (needle.size() > msg.size()) return false;
        for (std::size_t i = 0; i + needle.size() <= msg.size(); ++i) {
            bool ok = true;
            for (std::size_t j = 0; j < needle.size(); ++j) {
                char a = msg[i + j];
                char b = needle[j];
                if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + 32);
                if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + 32);
                if (a != b) { ok = false; break; }
            }
            if (ok) return true;
        }
        return false;
    };

    // Cancellation comes through as a dedicated string from the worker.
    if (contains("cancel")) return ErrorClass::Cancelled;

    // Auth surfaces as HTTP 401/403 from the wire layer or as the
    // explicit "authentication_error" type from Anthropic's body.
    if (contains("401")
     || contains("403")
     || contains("authentication_error")
     || contains("invalid api key")
     || contains("not authenticated"))
        return ErrorClass::Auth;

    // Rate limit — Anthropic's "rate_limit_error" or HTTP 429.
    if (contains("rate_limit") || contains("429"))
        return ErrorClass::RateLimit;

    // Overload / server / network — transient.
    if (contains("overloaded")
     || contains("overload_error")
     || contains("502")
     || contains("503")
     || contains("504")
     || contains("529")              // Anthropic's "overloaded" HTTP code
     || contains("connection")       // "connection refused/reset"
     || contains("timeout")
     || contains("eof")
     || contains("broken pipe")
     || contains("network")
     || contains("stall"))           // synthetic from the runtime stall watchdog
        return ErrorClass::Transient;

    return ErrorClass::Terminal;
}

// Backoff duration for the Nth retry attempt (0-indexed). Caps at 5
// attempts; longer schedules for RateLimit since Anthropic's per-minute
// window doesn't reset on demand. Returning `std::chrono::milliseconds`
// so the unit is in the type — callers scheduling with `Cmd::after(d)`
// can't accidentally feed seconds where ms were expected.
[[nodiscard]] constexpr std::chrono::milliseconds
backoff(ErrorClass kind, int attempt) noexcept {
    using std::chrono::milliseconds;
    if (attempt < 0) attempt = 0;
    if (attempt > 4) attempt = 4;
    if (kind == ErrorClass::RateLimit) {
        constexpr milliseconds table[5] = {
            milliseconds{3000},  milliseconds{8000},  milliseconds{20000},
            milliseconds{40000}, milliseconds{60000},
        };
        return table[attempt];
    }
    // Transient / Auth retry — fast first, then linger.
    constexpr milliseconds table[5] = {
        milliseconds{1000},  milliseconds{3000},  milliseconds{7000},
        milliseconds{15000}, milliseconds{30000},
    };
    return table[attempt];
}

// Hard cap on automatic retries. Past this, surface as terminal.
inline constexpr int kMaxRetries = 4;

[[nodiscard]] constexpr std::string_view to_string(ErrorClass k) noexcept {
    switch (k) {
        case ErrorClass::Transient: return "transient";
        case ErrorClass::RateLimit: return "rate_limit";
        case ErrorClass::Auth:      return "auth";
        case ErrorClass::Cancelled: return "cancelled";
        case ErrorClass::Terminal:  return "terminal";
    }
    return "unknown";
}

// ── Compile-time proofs of the HTTP→ErrorClass mapping ──────────────────
// Every kind/status that the HTTP layer can return is checked against
// the classifier here. If the table drifts (someone reorders the switch,
// adds a new HttpErrorKind without updating this overload, or changes
// the auth/rate-limit status mapping), the build breaks before any
// runtime path can take a wrong branch.
namespace proofs {
using moha::http::HttpError;
using moha::http::HttpErrorKind;

// Cancelled is always Cancelled — never re-issued, never reclassified.
static_assert(classify(HttpError{HttpErrorKind::Cancelled, 0, ""}) == ErrorClass::Cancelled);

// All transport-layer kinds map to Transient.
static_assert(classify(HttpError{HttpErrorKind::Resolve,      0, ""}) == ErrorClass::Transient);
static_assert(classify(HttpError{HttpErrorKind::Connect,      0, ""}) == ErrorClass::Transient);
static_assert(classify(HttpError{HttpErrorKind::Tls,          0, ""}) == ErrorClass::Transient);
static_assert(classify(HttpError{HttpErrorKind::Protocol,     0, ""}) == ErrorClass::Transient);
static_assert(classify(HttpError{HttpErrorKind::SocketHangup, 0, ""}) == ErrorClass::Transient);
static_assert(classify(HttpError{HttpErrorKind::Timeout,      0, ""}) == ErrorClass::Transient);
static_assert(classify(HttpError{HttpErrorKind::PeerClosed,   0, ""}) == ErrorClass::Transient);

// Body / Unknown are terminal — re-issuing won't change a malformed
// response or a programmer bug.
static_assert(classify(HttpError{HttpErrorKind::Body,    0, ""}) == ErrorClass::Terminal);
static_assert(classify(HttpError{HttpErrorKind::Unknown, 0, ""}) == ErrorClass::Terminal);

// Status mapping — the HTTP-status sub-table.
static_assert(classify(HttpError{HttpErrorKind::Status, 401, ""}) == ErrorClass::Auth);
static_assert(classify(HttpError{HttpErrorKind::Status, 403, ""}) == ErrorClass::Auth);
static_assert(classify(HttpError{HttpErrorKind::Status, 429, ""}) == ErrorClass::RateLimit);
static_assert(classify(HttpError{HttpErrorKind::Status, 408, ""}) == ErrorClass::Transient);
static_assert(classify(HttpError{HttpErrorKind::Status, 502, ""}) == ErrorClass::Transient);
static_assert(classify(HttpError{HttpErrorKind::Status, 503, ""}) == ErrorClass::Transient);
static_assert(classify(HttpError{HttpErrorKind::Status, 504, ""}) == ErrorClass::Transient);
static_assert(classify(HttpError{HttpErrorKind::Status, 529, ""}) == ErrorClass::Transient);
// Anything else 4xx is terminal — model said no, retrying won't change
// its mind.
static_assert(classify(HttpError{HttpErrorKind::Status, 400, ""}) == ErrorClass::Terminal);
static_assert(classify(HttpError{HttpErrorKind::Status, 404, ""}) == ErrorClass::Terminal);
static_assert(classify(HttpError{HttpErrorKind::Status, 422, ""}) == ErrorClass::Terminal);
// 200 OK reaching the classifier means application-level disagreement
// (caller surfaced it as Status with 200 — only happens via Body kind
// in practice; kept for completeness).
static_assert(classify(HttpError{HttpErrorKind::Status, 200, ""}) == ErrorClass::Terminal);

} // namespace proofs

} // namespace moha::provider
