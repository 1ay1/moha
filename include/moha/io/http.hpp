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

struct Request {
    std::string method;   // "GET" / "POST"
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

// Callbacks for a streaming request. on_headers fires once when the :status
// frame arrives; on_chunk fires for every DATA frame slab. Returning false
// from on_chunk aborts the stream (equivalent to cancelling the token).
struct StreamHandler {
    std::function<void(int status, const Headers&)>     on_headers;
    std::function<bool(std::string_view body_chunk)>    on_chunk;
};

// ---------------------------------------------------------------------------
// Timeouts. All durations are absolute per-operation caps, not idle guards.
// ---------------------------------------------------------------------------
// Matches Zed's choice: connect timeout is strict, the streaming phase is
// unbounded (rely on cancellation for hung servers).
struct Timeouts {
    std::chrono::milliseconds connect{10'000};
    std::chrono::milliseconds total  {0};  // 0 = unbounded (streams only).
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
    // while frames are in-flight.
    [[nodiscard]] std::expected<Response, std::string>
    send(const Request& req,
         Timeouts timeouts = {},
         CancelTokenPtr cancel = {});

    // Blocking streaming request. Returns when the peer closes the stream,
    // cancel is tripped, or on_chunk returns false. Body chunks arrive on
    // the calling thread (no internal threads — this is meant to run inside
    // a worker that owns the turn lifetime).
    [[nodiscard]] std::expected<void, std::string>
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
