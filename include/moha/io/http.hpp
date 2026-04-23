#pragma once
// moha::http — HTTP/2 client over OpenSSL, tuned for long-lived SSE streams.
//
// Mirrors Zed's layering (reqwest_client → hyper → h2 → rustls) but in C++:
// tls_context (OpenSSL SSL_CTX with platform-root verification) is opened once,
// a process-wide Client keeps a pool of HTTP/2 connections keyed by (host,port),
// and each request multiplexes a stream over whichever connection is idle.
//
// Cancellation is cooperative: every request accepts a CancelToken that the
// I/O loop polls between nghttp2 frame boundaries. Tripping the token closes
// the stream with RST_STREAM and the blocking send()/stream() call returns
// promptly, so a Ctrl-C from the UI terminates the turn cleanly.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace moha::http {

// ---------------------------------------------------------------------------
// CancelToken — cooperative abort signal shared between caller and I/O loop.
// ---------------------------------------------------------------------------
// Safe to flip from any thread. The I/O loop polls it at every nghttp2 frame
// boundary and between poll() waits, so a trip lands within a few ms on a
// live connection and immediately on an idle one (the socket is shut down,
// which wakes poll()).
class CancelToken {
public:
    void cancel() noexcept { flag_.store(true, std::memory_order_release); }
    [[nodiscard]] bool is_cancelled() const noexcept {
        return flag_.load(std::memory_order_acquire);
    }
private:
    std::atomic<bool> flag_{false};
};
using CancelTokenPtr = std::shared_ptr<CancelToken>;

// ---------------------------------------------------------------------------
// Requests / responses.
// ---------------------------------------------------------------------------
struct Header {
    std::string name;   // lowercase by convention; nghttp2 enforces for HTTP/2.
    std::string value;
};
using Headers = std::vector<Header>;

// Strongly-typed HTTP method. The wire spelling lives in one place
// (`wire_name`) and the runtime never sees a free-form string at this
// seam — "GET" vs "Get" vs "get" can't diverge between call sites.
enum class HttpMethod : std::uint8_t { Get, Post, Head };

[[nodiscard]] constexpr std::string_view wire_name(HttpMethod m) noexcept {
    switch (m) {
        case HttpMethod::Get:  return "GET";
        case HttpMethod::Post: return "POST";
        case HttpMethod::Head: return "HEAD";
    }
    return "GET";
}

struct Request {
    HttpMethod  method = HttpMethod::Get;
    std::string host;     // "api.anthropic.com"
    uint16_t    port = 443;
    std::string path;     // "/v1/messages"
    Headers     headers;
    std::string body;     // empty for GET; utf-8 bytes for POST.
};

struct Response {
    int     status = 0;
    Headers headers;
    std::string body;
};

// ---------------------------------------------------------------------------
// HttpError — the typed failure value returned by Client::send / stream.
// ---------------------------------------------------------------------------
// Replaces `std::expected<T, std::string>` so downstream callers (notably
// `provider::error_class::classify`) can dispatch on `kind` instead of
// substring-matching free-form messages. Adding a new failure mode is one
// new enum entry + one new switch arm in `to_string` / `is_transient` —
// the compiler tells you everywhere that needs to change.
//
// `http_status` is meaningful only when kind == Status; otherwise 0.
// `detail` is for human reading (logs, error banners) — never for
// programmatic dispatch.
enum class HttpErrorKind : std::uint8_t {
    Cancelled,    // CancelToken tripped — user-initiated (Esc) or watchdog
    Resolve,      // DNS / getaddrinfo failure
    Connect,      // TCP connect refused / network unreachable
    Tls,          // TLS handshake failure (cert, ALPN, etc.)
    Protocol,     // HTTP/2 protocol error (nghttp2 frame violation)
    SocketHangup, // POLLHUP / POLLERR / EPIPE mid-request
    Timeout,      // connect, total, or idle deadline expired
    PeerClosed,   // peer half-closed before completing the response
    Status,       // response received with status >= 400
    Body,         // body parse failure or size limit exceeded
    Unknown,      // unexpected — should never happen
};

struct HttpError {
    HttpErrorKind kind        = HttpErrorKind::Unknown;
    int           http_status = 0;       // valid iff kind == Status
    std::string   detail;                // human-readable

    // "[h2 timeout] no bytes for 45s" — the UI's default stringification
    // when it doesn't care to branch on kind. Layered errors call this
    // when wrapping into their own typed errors.
    [[nodiscard]] std::string render() const;

    // Factories for one-line return-site idioms.
    [[nodiscard]] static HttpError cancelled(std::string d = "cancelled")
        noexcept { return {HttpErrorKind::Cancelled, 0, std::move(d)}; }
    [[nodiscard]] static HttpError resolve(std::string d)
        noexcept { return {HttpErrorKind::Resolve, 0, std::move(d)}; }
    [[nodiscard]] static HttpError connect(std::string d)
        noexcept { return {HttpErrorKind::Connect, 0, std::move(d)}; }
    [[nodiscard]] static HttpError tls(std::string d)
        noexcept { return {HttpErrorKind::Tls, 0, std::move(d)}; }
    [[nodiscard]] static HttpError protocol(std::string d)
        noexcept { return {HttpErrorKind::Protocol, 0, std::move(d)}; }
    [[nodiscard]] static HttpError socket_hangup(std::string d)
        noexcept { return {HttpErrorKind::SocketHangup, 0, std::move(d)}; }
    [[nodiscard]] static HttpError timeout(std::string d)
        noexcept { return {HttpErrorKind::Timeout, 0, std::move(d)}; }
    [[nodiscard]] static HttpError peer_closed(std::string d)
        noexcept { return {HttpErrorKind::PeerClosed, 0, std::move(d)}; }
    [[nodiscard]] static HttpError status(int s, std::string d)
        noexcept { return {HttpErrorKind::Status, s, std::move(d)}; }
    [[nodiscard]] static HttpError body(std::string d)
        noexcept { return {HttpErrorKind::Body, 0, std::move(d)}; }
    [[nodiscard]] static HttpError unknown(std::string d)
        noexcept { return {HttpErrorKind::Unknown, 0, std::move(d)}; }

    // True if a retry might succeed (transient transport conditions). Used
    // by callers' retry policies. Cancellation, status 4xx (except 408,
    // 429), and Body errors are considered terminal.
    [[nodiscard]] bool is_transient() const noexcept;
};

[[nodiscard]] std::string_view to_string(HttpErrorKind k) noexcept;

// Public typed result aliases — these are what callers pattern-match on.
using HttpResult       = std::expected<Response, HttpError>;
using HttpStreamResult = std::expected<void,     HttpError>;

// Callbacks for a streaming request. on_headers fires once when the :status
// frame arrives; on_chunk fires for every DATA frame slab. Returning false
// from on_chunk aborts the stream (equivalent to cancelling the token).
struct StreamHandler {
    std::function<void(int status, const Headers&)>     on_headers;
    std::function<bool(std::string_view body_chunk)>    on_chunk;
};

// ---------------------------------------------------------------------------
// Timeouts.
// ---------------------------------------------------------------------------
// `connect` / `total` are absolute per-operation caps. `idle` and `ping` are
// liveness guardrails for long-lived streams, because a silent peer (half-
// dead TCP, proxy stall) produces no frames and no error — poll() just
// returns 0 forever. We need an inter-event idle clock + HTTP/2 PINGs to
// detect that case and fail fast so the caller can retry or surface an error
// rather than hang.
//
//   connect  strict — time to complete TCP + TLS + h2 preamble.
//   total    absolute cap for the whole request; 0 = unbounded (streams).
//   idle     error out if no bytes received for this long; 0 = disabled.
//   ping     send an HTTP/2 PING after this long without inbound bytes, to
//            probe the peer and coax a reply (PING ACK resets the idle
//            clock if the connection is still alive). 0 = disabled.
//
// For unary requests we leave idle/ping at 0 and rely on `total`. For
// streams we set both so a silent peer trips `idle` within a known bound.
struct Timeouts {
    std::chrono::milliseconds connect{10'000};
    std::chrono::milliseconds total  {0};
    std::chrono::milliseconds idle   {0};
    std::chrono::milliseconds ping   {0};
};

// ---------------------------------------------------------------------------
// Client. One instance = one process-wide HTTP/2 connection pool.
// ---------------------------------------------------------------------------
class Client {
public:
    struct Config {
        // Defaults to a descriptive UA with the moha version; override for tests.
        std::string user_agent = "moha/0.1.0";
        // When set, skip TLS chain verification — wire matches `-k` in curl.
        // Honors MOHA_INSECURE=1 env automatically in the ctor path.
        bool insecure = false;
    };

    Client();
    explicit Client(Config cfg);
    ~Client();

    Client(const Client&)            = delete;
    Client& operator=(const Client&) = delete;

    // Blocking unary request. The response body is fully buffered.
    // `cancel` may be null; non-null tokens are polled between I/O waits and
    // while frames are in-flight. Returns a typed `HttpError` on failure —
    // callers dispatch on `kind` rather than substring-matching the detail.
    [[nodiscard]] HttpResult
    send(const Request& req,
         Timeouts timeouts = {},
         CancelTokenPtr cancel = {});

    // Blocking streaming request. Returns when the peer closes the stream,
    // cancel is tripped, or on_chunk returns false. Body chunks arrive on
    // the calling thread (no internal threads — this is meant to run inside
    // a worker that owns the turn lifetime).
    [[nodiscard]] HttpStreamResult
    stream(const Request& req,
           StreamHandler handler,
           Timeouts timeouts = {},
           CancelTokenPtr cancel = {});

    // Fire-and-forget: prewarm a TLS connection to (host,port) on a detached
    // thread so the first real request skips the handshake. Safe to call
    // multiple times; idempotent after first success.
    void prewarm(std::string host, uint16_t port = 443);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Process-wide default client — lazy, constructed on first access, shared
// across all call sites. Equivalent to Zed's `GlobalHttpClient`.
[[nodiscard]] Client& default_client();

} // namespace moha::http
