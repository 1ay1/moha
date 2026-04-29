#include "moha/io/http.hpp"
#include "moha/io/tls.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// MSVC doesn't provide ssize_t (POSIX-only). nghttp2.h uses it for the legacy
// read callback typedef guarded by NGHTTP2_NO_SSIZE_T. Define it from BaseTsd
// before the include so nghttp2.h compiles cleanly without skipping the typedef.
#if defined(_MSC_VER) && !defined(__clang__)
#  include <BaseTsd.h>
   using ssize_t = SSIZE_T;
#endif
#include <nghttp2/nghttp2.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

// ---------------------------------------------------------------------------
// Platform socket shim.  Everything below the shim treats a socket as an
// `int` that supports read/write/poll/close; BSD + Winsock agree on the
// surface, they differ on the headers and a couple of lifecycle calls.
// ---------------------------------------------------------------------------
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  ifdef _MSC_VER
#    pragma comment(lib, "ws2_32.lib")
#  endif
using socket_t = SOCKET;
constexpr socket_t kBadSocket = INVALID_SOCKET;
static int sock_last_err() { return WSAGetLastError(); }
static int sock_close(socket_t s) { return ::closesocket(s); }
static int sock_poll(pollfd* fds, unsigned n, int ms) { return ::WSAPoll(fds, n, ms); }
// errno-equivalent constants used in error-classifying retries.
static bool sock_in_progress(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
static bool sock_intr(int e) { return e == WSAEINTR; }
static void sock_set_nonblock(socket_t s) {
    u_long nb = 1; ::ioctlsocket(s, FIONBIO, &nb);
}
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
using socket_t = int;
constexpr socket_t kBadSocket = -1;
static int sock_last_err() { return errno; }
static int sock_close(socket_t s) { return ::close(s); }
static int sock_poll(pollfd* fds, unsigned n, int ms) { return ::poll(fds, n, ms); }
static bool sock_in_progress(int e) { return e == EINPROGRESS || e == EWOULDBLOCK || e == EAGAIN; }
static bool sock_intr(int e) { return e == EINTR; }
static void sock_set_nonblock(socket_t s) {
    int fl = ::fcntl(s, F_GETFL, 0);
    if (fl >= 0) ::fcntl(s, F_SETFL, fl | O_NONBLOCK);
}
#endif

namespace moha::http {

using clock_t_ = std::chrono::steady_clock;
using ms_t     = std::chrono::milliseconds;

// ---------------------------------------------------------------------------
// HttpError implementations. The static factories are inline in the header;
// these are the rendering + retry-policy helpers that need switch arms.
// ---------------------------------------------------------------------------

std::string_view to_string(HttpErrorKind k) noexcept {
    switch (k) {
        case HttpErrorKind::Cancelled:    return "cancelled";
        case HttpErrorKind::Resolve:      return "resolve";
        case HttpErrorKind::Connect:      return "connect";
        case HttpErrorKind::Tls:          return "tls";
        case HttpErrorKind::Protocol:     return "h2";
        case HttpErrorKind::SocketHangup: return "hangup";
        case HttpErrorKind::Timeout:      return "timeout";
        case HttpErrorKind::PeerClosed:   return "peer_closed";
        case HttpErrorKind::Status:       return "status";
        case HttpErrorKind::Body:         return "body";
        case HttpErrorKind::Unknown:      return "unknown";
    }
    return "unknown";
}

std::string HttpError::render() const {
    std::string out = "[";
    out += to_string(kind);
    out += "] ";
    out += detail;
    if (kind == HttpErrorKind::Status && http_status > 0) {
        out += " (HTTP ";
        out += std::to_string(http_status);
        out += ")";
    }
    return out;
}

bool HttpError::is_transient() const noexcept {
    switch (kind) {
        // Re-issue-safe transport hiccups.
        case HttpErrorKind::Resolve:
        case HttpErrorKind::Connect:
        case HttpErrorKind::Tls:
        case HttpErrorKind::Protocol:
        case HttpErrorKind::SocketHangup:
        case HttpErrorKind::Timeout:
        case HttpErrorKind::PeerClosed:
            return true;
        // Server-side load shedding / rate limit / one-shot transients.
        case HttpErrorKind::Status:
            return http_status == 408 || http_status == 429
                || http_status == 502 || http_status == 503
                || http_status == 504 || http_status == 529;
        // User-driven or shape errors — never re-issue.
        case HttpErrorKind::Cancelled:
        case HttpErrorKind::Body:
        case HttpErrorKind::Unknown:
            return false;
    }
    return false;
}

namespace {

// -----------------------------------------------------------------------
// One-time Winsock init.  The matching WSACleanup is deliberately skipped —
// process-lifetime, and tearing down Winsock while workers are draining
// kills in-flight requests in ways that are hard to diagnose.
// -----------------------------------------------------------------------
void ensure_net_init() {
#if defined(_WIN32)
    static std::once_flag once;
    std::call_once(once, []{
        WSADATA data;
        (void)WSAStartup(MAKEWORD(2, 2), &data);
    });
#endif
}

// -----------------------------------------------------------------------
// Timeout helper: milliseconds remaining before a deadline, clamped to a
// sensible upper bound so our poll loop still checks cancellation tokens
// in a timely fashion when total==unbounded.
// -----------------------------------------------------------------------
int remaining_ms(std::optional<clock_t_::time_point> deadline, int cap_ms) {
    if (!deadline) return cap_ms;
    auto now = clock_t_::now();
    if (now >= *deadline) return 0;
    auto rem = std::chrono::duration_cast<ms_t>(*deadline - now).count();
    return static_cast<int>(std::min<int64_t>(rem, cap_ms));
}

// -----------------------------------------------------------------------
// Endpoint — pool key.  Identity is (host,port,dial_host,dial_port); two
// requests to the same logical API collapse into the same pool slot.  The
// dial fields are part of the key because two endpoints with the same SNI
// host but different TCP targets (e.g. one direct, one through an SSH
// tunnel) are physically distinct connections.
// -----------------------------------------------------------------------
struct Endpoint {
    std::string host;       // SNI + cert + Host header
    uint16_t    port = 443;
    std::string dial_host;  // TCP target override; empty = use host
    uint16_t    dial_port = 0; // TCP port override; 0 = use port

    [[nodiscard]] std::string_view tcp_host() const noexcept {
        return dial_host.empty() ? std::string_view{host} : std::string_view{dial_host};
    }
    [[nodiscard]] uint16_t tcp_port() const noexcept {
        return dial_port == 0 ? port : dial_port;
    }

    bool operator==(const Endpoint& o) const {
        return port == o.port && host == o.host
            && dial_port == o.dial_port && dial_host == o.dial_host;
    }
};
struct EndpointHash {
    size_t operator()(const Endpoint& e) const noexcept {
        // djb2 over host + dial_host plus the two ports — good enough for
        // the handful of endpoints moha ever talks to.
        size_t h = 5381;
        for (char c : e.host) h = ((h << 5) + h) + static_cast<unsigned char>(c);
        for (char c : e.dial_host) h = ((h << 5) + h) + static_cast<unsigned char>(c);
        h ^= static_cast<size_t>(e.port) * 0x9E3779B97F4A7C15ull;
        h ^= static_cast<size_t>(e.dial_port) * 0xBF58476D1CE4E5B9ull;
        return h;
    }
};

// -----------------------------------------------------------------------
// StreamCtx — per-request state owned by the Connection while a stream
// is in flight.  One at a time per connection in our design (we don't
// multiplex tool streams over one connection, which keeps the lifecycle
// simple and sidesteps the CURLSH-class bug).  The connection still
// benefits from h2 framing + keepalive.
// -----------------------------------------------------------------------
struct StreamCtx {
    // Request side
    const std::string* body = nullptr;   // POST body; null for GET
    size_t             body_off = 0;

    // Response side — either fully buffered (send) or streamed (stream)
    int                 status = 0;
    Headers             headers;
    std::string         buffered_body;
    StreamHandler*      handler = nullptr;  // non-null for stream()
    bool                handler_aborted = false;
    bool                headers_delivered = false;

    // Lifecycle
    int32_t stream_id = -1;
    bool    completed = false;
    bool    reset     = false;   // peer sent RST_STREAM
    // Result propagation — if we hit an abort during a stream, we keep the
    // reason to hand to the caller.
    std::string error;
};

// -----------------------------------------------------------------------
// Connection: one TCP+TLS+nghttp2 session.  Used by exactly one request at
// a time; returned to the pool after the stream closes.  Non-copyable;
// unique_ptr-owned.
// -----------------------------------------------------------------------
class Connection {
public:
    Connection(socket_t fd, tls::SSL* ssl, nghttp2_session* session,
               Endpoint ep)
        : fd_(fd), ssl_(ssl), session_(session), endpoint_(std::move(ep)) {}

    ~Connection() {
        if (session_) nghttp2_session_del(session_);
        tls::free_ssl(ssl_);
        if (fd_ != kBadSocket) sock_close(fd_);
    }

    Connection(const Connection&)            = delete;
    Connection& operator=(const Connection&) = delete;

    [[nodiscard]] socket_t fd() const { return fd_; }
    [[nodiscard]] nghttp2_session* session() { return session_; }
    [[nodiscard]] const Endpoint& endpoint() const { return endpoint_; }
    [[nodiscard]] bool is_alive() const {
        if (!session_) return false;
        // nghttp2 flags EOF / GOAWAY internally; this catches both.
        return nghttp2_session_want_read(session_)
            || nghttp2_session_want_write(session_);
    }

    // Execute a single request/stream on this connection.  The connection
    // must be idle on entry; it's usable again on clean return.
    std::expected<void, HttpError> run(
        const Request& req,
        StreamCtx& sctx,
        Timeouts tos,
        CancelTokenPtr cancel);

    // Drive the session until an event flushes all pending frames in one
    // direction, used by the ctor to complete SETTINGS exchange.
    std::expected<void, HttpError> pump_initial(Timeouts tos);

private:
    socket_t         fd_      = kBadSocket;
    tls::SSL*        ssl_     = nullptr;
    nghttp2_session* session_ = nullptr;
    Endpoint         endpoint_;
    // Pending outbound bytes from a previous `nghttp2_session_mem_send` that
    // SSL_write couldn't fully flush (WANT_WRITE, partial return). We *must*
    // drain these before pulling the next chunk — nghttp2 invalidates the
    // prior pointer on the next mem_send call, and OpenSSL raises
    // SSL_R_BAD_LENGTH on a retry whose length shrinks vs. the pending
    // write. Kept as (ptr into nghttp2-owned buffer, offset, size).
    const uint8_t*   pend_buf_ = nullptr;
    size_t           pend_off_ = 0;
    size_t           pend_len_ = 0;
};

// -----------------------------------------------------------------------
// nghttp2 callbacks.  All static; session's user_data holds the StreamCtx*.
// Only one request at a time per session, so a single pointer is enough
// (vs. a stream_id → ctx map).
// -----------------------------------------------------------------------
static int on_frame_recv(nghttp2_session* /*s*/, const nghttp2_frame* frame,
                         void* user_data) {
    auto* sc = static_cast<StreamCtx*>(user_data);
    if (!sc) return 0;
    if (frame->hd.stream_id != sc->stream_id) return 0;
    if (frame->hd.type == NGHTTP2_HEADERS
        && frame->headers.cat == NGHTTP2_HCAT_RESPONSE) {
        if (sc->handler && sc->handler->on_headers && !sc->headers_delivered) {
            sc->handler->on_headers(sc->status, sc->headers);
            sc->headers_delivered = true;
        }
    }
    return 0;
}

static int on_data_chunk(nghttp2_session* s, uint8_t /*flags*/,
                         int32_t stream_id, const uint8_t* data, size_t len,
                         void* user_data) {
    auto* sc = static_cast<StreamCtx*>(user_data);
    if (!sc || stream_id != sc->stream_id) return 0;
    if (sc->handler) {
        // Streaming mode: hand the chunk straight to the user.  A false
        // return from on_chunk aborts the stream; we close it below so
        // nghttp2 stops pumping data.
        if (sc->handler->on_chunk) {
            if (!sc->handler->on_chunk(
                    std::string_view{reinterpret_cast<const char*>(data), len})) {
                sc->handler_aborted = true;
                nghttp2_submit_rst_stream(s, NGHTTP2_FLAG_NONE, stream_id,
                                          NGHTTP2_CANCEL);
                return 0;
            }
        }
    } else {
        // Hard cap on the buffered (unary) response body. Anthropic's
        // /v1/messages reply is well under a megabyte; a runaway upstream
        // (test harness, misconfigured proxy, body-replay loop) would
        // otherwise grow this string until the process OOMs. 100 MiB
        // dwarfs any legitimate API response while still being orders of
        // magnitude under typical RAM. Once exceeded, RST_STREAM the
        // request and surface a typed Body error from the public API.
        constexpr std::size_t kMaxBufferedBody = 100ull * 1024 * 1024;
        if (sc->buffered_body.size() + len > kMaxBufferedBody) {
            sc->error = "response body exceeded "
                      + std::to_string(kMaxBufferedBody) + " bytes";
            sc->handler_aborted = true;
            nghttp2_submit_rst_stream(s, NGHTTP2_FLAG_NONE, stream_id,
                                      NGHTTP2_CANCEL);
            return 0;
        }
        sc->buffered_body.append(reinterpret_cast<const char*>(data), len);
    }
    return 0;
}

static int on_stream_close(nghttp2_session* /*s*/, int32_t stream_id,
                           uint32_t error_code, void* user_data) {
    auto* sc = static_cast<StreamCtx*>(user_data);
    if (!sc || stream_id != sc->stream_id) return 0;
    sc->completed = true;
    if (error_code != NGHTTP2_NO_ERROR) {
        sc->reset = true;
        if (sc->error.empty())
            sc->error = "stream reset (" + std::to_string(error_code) + ")";
    }
    return 0;
}

static int on_header(nghttp2_session* /*s*/, const nghttp2_frame* frame,
                     const uint8_t* name, size_t nlen,
                     const uint8_t* value, size_t vlen,
                     uint8_t /*flags*/, void* user_data) {
    auto* sc = static_cast<StreamCtx*>(user_data);
    if (!sc || frame->hd.stream_id != sc->stream_id) return 0;
    std::string_view n{reinterpret_cast<const char*>(name),  nlen};
    std::string_view v{reinterpret_cast<const char*>(value), vlen};
    if (n == ":status") {
        sc->status = std::atoi(std::string{v}.c_str());
    } else {
        sc->headers.push_back({std::string{n}, std::string{v}});
    }
    return 0;
}

// Data provider for POST bodies.  nghttp2 pulls bytes from here when it's
// ready to emit them; we read straight out of sctx->body and mark EOF when
// we hit the end.
static ssize_t data_read_cb(nghttp2_session* /*s*/, int32_t /*stream_id*/,
                            uint8_t* buf, size_t length, uint32_t* data_flags,
                            nghttp2_data_source* source, void* /*user_data*/) {
    auto* sc = static_cast<StreamCtx*>(source->ptr);
    if (!sc || !sc->body) {
        *data_flags = NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }
    const size_t remaining = sc->body->size() - sc->body_off;
    const size_t n = std::min(remaining, length);
    if (n) std::memcpy(buf, sc->body->data() + sc->body_off, n);
    sc->body_off += n;
    if (sc->body_off >= sc->body->size())
        *data_flags = NGHTTP2_DATA_FLAG_EOF;
    return static_cast<ssize_t>(n);
}

// -----------------------------------------------------------------------
// DNS + connect.  Non-blocking connect so we can honor the connect timeout;
// poll()s until writable or deadline.
// -----------------------------------------------------------------------
// SOCKS5 negotiation over an already-TCP-connected, non-blocking fd.
// Tells the proxy to open a tunnel to (dest_host, dest_port) using
// ATYP=DOMAIN so DNS happens proxy-side (which is the whole point on an
// air-gapped box whose own DNS can't see the public internet).  After a
// successful return the caller's fd carries an opaque byte stream to
// the destination — TLS goes on top of it like a direct connection.
//
// Connection-failure response codes (REP) are mapped to HttpError kinds:
// host-unreachable / network-unreachable / TTL-expired -> Resolve;
// connection-refused -> Connect; everything else (including version
// mismatch and forbidden) -> Tls (a misconfigured proxy is closer to a
// trust failure than a transport one).
namespace {
std::expected<void, HttpError>
socks5_send_all(socket_t fd, const unsigned char* data, size_t len,
                clock_t_::time_point deadline, CancelToken* cancel) {
    while (len > 0) {
        if (cancel && cancel->is_cancelled())
            return std::unexpected(HttpError::cancelled("socks5 send"));
#if defined(_WIN32)
        int n = ::send(fd, reinterpret_cast<const char*>(data),
                       static_cast<int>(len), 0);
#else
        ssize_t n = ::send(fd, data, len, MSG_NOSIGNAL);
#endif
        if (n > 0) { data += n; len -= static_cast<size_t>(n); continue; }
        int e = sock_last_err();
        if (sock_intr(e)) continue;
        if (!sock_in_progress(e))
            return std::unexpected(HttpError::tls(
                "socks5 send: errno=" + std::to_string(e)));
        int rem = remaining_ms(deadline, 200);
        if (rem <= 0)
            return std::unexpected(HttpError::timeout("socks5 send timed out"));
        pollfd pfd{ fd, POLLOUT, 0 };
        int pr = sock_poll(&pfd, 1, rem);
        if (pr < 0 && !sock_intr(sock_last_err()))
            return std::unexpected(HttpError::tls(
                "socks5 poll: errno=" + std::to_string(sock_last_err())));
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            return std::unexpected(HttpError::tls("socks5 send: poll hangup"));
    }
    return {};
}

std::expected<void, HttpError>
socks5_recv_exact(socket_t fd, unsigned char* out, size_t want,
                  clock_t_::time_point deadline, CancelToken* cancel) {
    while (want > 0) {
        if (cancel && cancel->is_cancelled())
            return std::unexpected(HttpError::cancelled("socks5 recv"));
#if defined(_WIN32)
        int n = ::recv(fd, reinterpret_cast<char*>(out),
                       static_cast<int>(want), 0);
#else
        ssize_t n = ::recv(fd, out, want, 0);
#endif
        if (n > 0) { out += n; want -= static_cast<size_t>(n); continue; }
        if (n == 0)
            return std::unexpected(HttpError::peer_closed(
                "socks5: proxy closed during handshake"));
        int e = sock_last_err();
        if (sock_intr(e)) continue;
        if (!sock_in_progress(e))
            return std::unexpected(HttpError::tls(
                "socks5 recv: errno=" + std::to_string(e)));
        int rem = remaining_ms(deadline, 200);
        if (rem <= 0)
            return std::unexpected(HttpError::timeout("socks5 recv timed out"));
        pollfd pfd{ fd, POLLIN, 0 };
        int pr = sock_poll(&pfd, 1, rem);
        if (pr < 0 && !sock_intr(sock_last_err()))
            return std::unexpected(HttpError::tls(
                "socks5 poll: errno=" + std::to_string(sock_last_err())));
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            return std::unexpected(HttpError::tls("socks5 recv: poll hangup"));
    }
    return {};
}

std::expected<void, HttpError>
socks5_negotiate(socket_t fd, std::string_view dest_host, uint16_t dest_port,
                 clock_t_::time_point deadline, CancelToken* cancel) {
    if (dest_host.empty() || dest_host.size() > 255)
        return std::unexpected(HttpError::tls(
            "socks5: destination hostname empty or > 255 bytes"));

    // 1. Greeting: ver=5, 1 method, NO_AUTH.  The 'remote SOCKS' that
    //    `ssh -R <port>` exposes only speaks NO_AUTH (the SSH session
    //    itself is already the trust boundary), so we don't bother
    //    advertising USERPASS.
    {
        unsigned char greet[3] = { 0x05, 0x01, 0x00 };
        if (auto r = socks5_send_all(fd, greet, 3, deadline, cancel); !r)
            return r;
        unsigned char gresp[2];
        if (auto r = socks5_recv_exact(fd, gresp, 2, deadline, cancel); !r)
            return r;
        if (gresp[0] != 0x05 || gresp[1] != 0x00)
            return std::unexpected(HttpError::tls(
                "socks5: proxy refused NO_AUTH (resp "
                + std::to_string(gresp[0]) + "/" + std::to_string(gresp[1]) + ")"));
    }

    // 2. CONNECT request with ATYP=DOMAIN so DNS lands proxy-side.
    {
        std::vector<unsigned char> req;
        req.reserve(7 + dest_host.size());
        req.push_back(0x05);                                          // ver
        req.push_back(0x01);                                          // CMD = CONNECT
        req.push_back(0x00);                                          // RSV
        req.push_back(0x03);                                          // ATYP = DOMAIN
        req.push_back(static_cast<unsigned char>(dest_host.size()));
        req.insert(req.end(), dest_host.begin(), dest_host.end());
        req.push_back(static_cast<unsigned char>(dest_port >> 8));
        req.push_back(static_cast<unsigned char>(dest_port & 0xff));
        if (auto r = socks5_send_all(fd, req.data(), req.size(), deadline, cancel); !r)
            return r;
    }

    // 3. CONNECT reply: 4-byte head + variable BND.ADDR + 2-byte BND.PORT.
    //    We don't care about the bound address — we just have to drain it
    //    so the next read on the fd starts on the tunnelled byte stream.
    unsigned char head[4];
    if (auto r = socks5_recv_exact(fd, head, 4, deadline, cancel); !r)
        return r;
    if (head[0] != 0x05)
        return std::unexpected(HttpError::tls(
            "socks5: bad reply version " + std::to_string(head[0])));
    if (head[1] != 0x00) {
        const char* msg;
        switch (head[1]) {
            case 0x01: msg = "general SOCKS server failure"; break;
            case 0x02: msg = "connection not allowed by ruleset"; break;
            case 0x03: msg = "network unreachable"; break;
            case 0x04: msg = "host unreachable"; break;
            case 0x05: msg = "connection refused"; break;
            case 0x06: msg = "TTL expired"; break;
            case 0x07: msg = "command not supported"; break;
            case 0x08: msg = "address type not supported"; break;
            default:   msg = "unknown reply code";
        }
        std::string detail = "socks5 CONNECT to ";
        detail.append(dest_host).append(":").append(std::to_string(dest_port))
              .append(" rejected: ").append(msg)
              .append(" (REP=").append(std::to_string(head[1])).append(")");
        // Map the network-shape failures back to the equivalent transport
        // HttpErrorKinds so retry / reporting code paths stay uniform.
        if (head[1] == 0x03 || head[1] == 0x04 || head[1] == 0x06)
            return std::unexpected(HttpError::resolve(std::move(detail)));
        if (head[1] == 0x05)
            return std::unexpected(HttpError::connect(std::move(detail)));
        return std::unexpected(HttpError::tls(std::move(detail)));
    }

    int bnd_addr_len;
    switch (head[3]) {
        case 0x01: bnd_addr_len = 4;  break; // IPv4
        case 0x04: bnd_addr_len = 16; break; // IPv6
        case 0x03: {                         // DOMAIN: 1 length byte then octets
            unsigned char l;
            if (auto r = socks5_recv_exact(fd, &l, 1, deadline, cancel); !r)
                return r;
            bnd_addr_len = l;
            break;
        }
        default:
            return std::unexpected(HttpError::tls(
                "socks5: bad reply ATYP " + std::to_string(head[3])));
    }
    std::vector<unsigned char> drop(static_cast<size_t>(bnd_addr_len) + 2);
    if (auto r = socks5_recv_exact(fd, drop.data(), drop.size(), deadline, cancel); !r)
        return r;
    return {};
}
} // namespace

std::expected<socket_t, HttpError>
dial_tcp(const Endpoint& ep, Timeouts tos, CancelToken* cancel) {
    ensure_net_init();
    // SOCKS5 short-circuit: dial the proxy address (not the upstream),
    // then negotiate a tunnel to the upstream's tcp_host()/tcp_port().
    // The fd we return looks like a direct connection to the upstream,
    // ready for TLS — wrap_client below uses ep.host for SNI as ever.
    const auto& sx = moha_socks_proxy();

    addrinfo hints{}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string conn_host =
        sx.active() ? sx.host : std::string{ep.tcp_host()};
    const uint16_t    conn_port = sx.active() ? sx.port : ep.tcp_port();
    char port_buf[12]; std::snprintf(port_buf, sizeof(port_buf), "%u", conn_port);
    int gai = ::getaddrinfo(conn_host.c_str(), port_buf, &hints, &res);
    if (gai != 0 || !res) {
        return std::unexpected(HttpError::resolve(
            std::string{"getaddrinfo: "} + gai_strerror(gai)));
    }

    auto deadline = clock_t_::now() + tos.connect;

    // After a successful TCP connect, this finishes the dial: SOCKS5
    // negotiation if a proxy is configured (so the returned fd is a
    // tunnel to ep's logical upstream); otherwise pass-through.
    auto finalize = [&](socket_t fd) -> std::expected<socket_t, HttpError> {
        if (!sx.active()) return fd;
        if (auto r = socks5_negotiate(fd, ep.tcp_host(), ep.tcp_port(),
                                      deadline, cancel); !r) {
            sock_close(fd);
            return std::unexpected(std::move(r).error());
        }
        return fd;
    };

    std::string last_err;
    for (addrinfo* p = res; p; p = p->ai_next) {
        socket_t fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd == kBadSocket) { last_err = "socket() failed"; continue; }
#if !defined(_WIN32)
        // Lower Nagle + keep stream responsive.  Anthropic's SSE frames are
        // small and bursty; we want them flushed to user space immediately.
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#  if defined(__linux__) && defined(TCP_QUICKACK)
        // Disable delayed ACKs. Linux only — kernel coalesces ACKs by
        // default to amortize wakeups, which adds 40 ms to every SSE
        // frame round-trip when the receiver isn't sending data back.
        // For pure-receive streams (which SSE is), QUICKACK fires the
        // ACK on each segment, prompting the sender to push the next
        // window segment immediately. Effect: ~10-40 ms tighter token
        // cadence on bursty deltas.
        ::setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one));
#  endif
#else
        BOOL one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&one), sizeof(one));
        ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE,
                     reinterpret_cast<const char*>(&one), sizeof(one));
        // Windows defaults to a 64 KiB receive buffer; that's cramped for
        // Anthropic's SSE bursts, where a single content_block_delta chunk
        // can be 8–32 KiB and the TCP window capping the receive side
        // forces the sender to pause for round-trip ACKs mid-frame. A 1 MiB
        // buffer lets the kernel soak up 10+ back-to-back segments without
        // flow-control stalls; measurable on wall-clock TTFT of longer
        // completions. Send buffer bumped similarly for request bodies
        // (bash tool output, large Edit payloads going back to the API).
        int rcvbuf = 1 << 20;            // 1 MiB
        int sndbuf = 256 << 10;          // 256 KiB
        ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF,
                     reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF,
                     reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf));
#endif
        sock_set_nonblock(fd);
        int r = ::connect(fd, p->ai_addr, static_cast<int>(p->ai_addrlen));
        if (r == 0) { ::freeaddrinfo(res); return finalize(fd); }
        int e = sock_last_err();
        if (!sock_in_progress(e)) {
            sock_close(fd);
            last_err = "connect() errno=" + std::to_string(e);
            continue;
        }
        // Wait until writable or deadline.  Short slices so cancel lands fast.
        while (true) {
            if (cancel && cancel->is_cancelled()) {
                sock_close(fd); ::freeaddrinfo(res);
                return std::unexpected(HttpError::cancelled("during connect"));
            }
            int rem = remaining_ms(deadline, 200);
            if (rem <= 0) {
                sock_close(fd);
                last_err = "connect: timed out";
                break;
            }
            pollfd pfd{ fd, POLLOUT, 0 };
            int pr = sock_poll(&pfd, 1, rem);
            if (pr < 0) {
                if (sock_intr(sock_last_err())) continue;
                sock_close(fd);
                last_err = "poll: errno=" + std::to_string(sock_last_err());
                break;
            }
            if (pr == 0) continue;
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                sock_close(fd);
                last_err = "connect: poll hangup";
                break;
            }
            int soerr = 0;
#if defined(_WIN32)
            int sl = sizeof(soerr);
            ::getsockopt(fd, SOL_SOCKET, SO_ERROR,
                         reinterpret_cast<char*>(&soerr), &sl);
#else
            socklen_t sl = sizeof(soerr);
            ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
#endif
            if (soerr != 0) {
                sock_close(fd);
                last_err = "connect: SO_ERROR=" + std::to_string(soerr);
                break;
            }
            ::freeaddrinfo(res);
            return finalize(fd);
        }
    }
    ::freeaddrinfo(res);
    return std::unexpected(HttpError::connect(
        last_err.empty() ? std::string{"no address worked"} : last_err));
}

// -----------------------------------------------------------------------
// TLS handshake loop.  SSL is already attached to fd; we drive SSL_connect
// and translate WANT_READ/WANT_WRITE into poll() waits, honoring the same
// connect deadline.
// -----------------------------------------------------------------------
std::expected<void, HttpError>
tls_handshake(socket_t fd, tls::SSL* ssl, Timeouts tos, CancelToken* cancel) {
    auto deadline = clock_t_::now() + tos.connect;
    while (true) {
        int r = SSL_connect(ssl);
        if (r == 1) return {};
        int e = SSL_get_error(ssl, r);
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE)
            return std::unexpected(HttpError::tls(tls::last_error(ssl)));
        if (cancel && cancel->is_cancelled())
            return std::unexpected(HttpError::cancelled("during tls handshake"));
        int rem = remaining_ms(deadline, 200);
        if (rem <= 0) return std::unexpected(HttpError::timeout("tls handshake timed out"));
        pollfd pfd{ fd, (e == SSL_ERROR_WANT_READ) ? (short)POLLIN : (short)POLLOUT, 0 };
        int pr = sock_poll(&pfd, 1, rem);
        if (pr < 0) {
            if (sock_intr(sock_last_err())) continue;
            return std::unexpected(HttpError::tls(
                "poll errno=" + std::to_string(sock_last_err())));
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            return std::unexpected(HttpError::tls("poll hangup"));
    }
}

// Pump outgoing frames: pull bytes off nghttp2 and write through TLS.
// Returns WANT_* if TLS couldn't write the whole mem_send() chunk.
//
// Invariant: `pend_*` on the Connection holds any bytes that the previous
// call left unflushed. We must drain those *before* pulling a new chunk
// from nghttp2 — mem_send invalidates its prior pointer on the next call
// and advances internal state, so a partially-written frame would be lost
// (and OpenSSL would reject the retry with SSL_R_BAD_LENGTH once the
// pending-write length changed under it).
enum class PumpOut { Idle, WantRead, WantWrite, Error };
static PumpOut pump_send_impl(tls::SSL* ssl, nghttp2_session* session,
                              const uint8_t*& pend_buf, size_t& pend_off,
                              size_t& pend_len, std::string* err) {
    auto drain_pending = [&]() -> PumpOut {
        while (pend_off < pend_len) {
            int chunk = static_cast<int>(pend_len - pend_off);
            int r = SSL_write(ssl, pend_buf + pend_off, chunk);
            if (r > 0) { pend_off += static_cast<size_t>(r); continue; }
            int e = SSL_get_error(ssl, r);
            if (e == SSL_ERROR_WANT_WRITE) return PumpOut::WantWrite;
            if (e == SSL_ERROR_WANT_READ)  return PumpOut::WantRead;
            if (err) *err = "SSL_write: " + tls::last_error(ssl);
            return PumpOut::Error;
        }
        pend_buf = nullptr;
        pend_off = 0;
        pend_len = 0;
        return PumpOut::Idle;
    };

    // First, flush whatever nghttp2 handed us last time.
    if (pend_buf && pend_off < pend_len) {
        auto s = drain_pending();
        if (s != PumpOut::Idle) return s;
    }

    // Pull & flush successive chunks until nghttp2 has no more or TLS blocks.
    while (true) {
        const uint8_t* data = nullptr;
        ssize_t n = nghttp2_session_mem_send(session, &data);
        if (n < 0) {
            if (err) *err = std::string{"nghttp2_session_mem_send: "}
                          + nghttp2_strerror(static_cast<int>(n));
            return PumpOut::Error;
        }
        if (n == 0) return PumpOut::Idle;
        pend_buf = data;
        pend_off = 0;
        pend_len = static_cast<size_t>(n);
        auto s = drain_pending();
        if (s != PumpOut::Idle) return s;
    }
}

// Pump incoming bytes: read from TLS, feed to nghttp2.  Returns WANT_* when
// TLS has nothing more buffered. `bytes_in` accumulates the total number of
// plaintext bytes successfully delivered to nghttp2 — the caller uses this
// to refresh the idle-timeout clock so *any* inbound activity (DATA, PING
// ACK, WINDOW_UPDATE) counts as liveness.
enum class PumpIn { Idle, WantRead, WantWrite, Closed, Error };
static PumpIn pump_recv(tls::SSL* ssl, nghttp2_session* session,
                        std::string* err, size_t* bytes_in) {
    // 64 KiB read chunks — drains a typical SSE burst (10–30 KiB of
    // text deltas across 3-4 TCP segments) in a single SSL_read +
    // nghttp2_session_mem_recv pair, instead of looping the poll/read
    // cycle 3-4 times. Saves ~2 ms / burst on bursty streaming, and
    // costs nothing on idle connections (SSL_read returns immediately
    // when no data is available). Stack-allocated, no churn.
    uint8_t buf[64 * 1024];
    while (true) {
        int r = SSL_read(ssl, buf, sizeof(buf));
        if (r > 0) {
            if (bytes_in) *bytes_in += static_cast<size_t>(r);
            ssize_t rv = nghttp2_session_mem_recv(session, buf, static_cast<size_t>(r));
            if (rv < 0) {
                if (err) *err = std::string{"nghttp2_session_mem_recv: "}
                              + nghttp2_strerror(static_cast<int>(rv));
                return PumpIn::Error;
            }
            continue;
        }
        int e = SSL_get_error(ssl, r);
        if (e == SSL_ERROR_WANT_READ)   return PumpIn::WantRead;
        if (e == SSL_ERROR_WANT_WRITE)  return PumpIn::WantWrite;
        if (e == SSL_ERROR_ZERO_RETURN) return PumpIn::Closed;
        if (err) *err = "SSL_read: " + tls::last_error(ssl);
        return PumpIn::Error;
    }
}

std::expected<void, HttpError>
Connection::pump_initial(Timeouts tos) {
    // Drive the first SETTINGS / window-update exchange so the connection
    // is fully usable before we return it from dial_*.  Bounded by the
    // connect deadline passed in.
    auto deadline = clock_t_::now() + tos.connect;
    std::string err;
    while (nghttp2_session_want_write(session_)) {
        auto s = pump_send_impl(ssl_, session_, pend_buf_, pend_off_,
                                pend_len_, &err);
        if (s == PumpOut::Error) return std::unexpected(HttpError::protocol(err));
        if (s == PumpOut::Idle) break;
        int rem = remaining_ms(deadline, 200);
        if (rem <= 0) return std::unexpected(HttpError::timeout("h2 initial send timed out"));
        pollfd pfd{ fd_, (s == PumpOut::WantRead) ? (short)POLLIN : (short)POLLOUT, 0 };
        int pr = sock_poll(&pfd, 1, rem);
        if (pr < 0 && !sock_intr(sock_last_err()))
            return std::unexpected(HttpError::protocol(
                "h2 poll errno=" + std::to_string(sock_last_err())));
    }
    return {};
}

std::expected<void, HttpError>
Connection::run(const Request& req, StreamCtx& sctx, Timeouts tos,
                CancelTokenPtr cancel) {
    // --- build headers list.  All names must be lowercase in HTTP/2. ---
    // :method, :scheme, :authority, :path come first by convention.
    std::string authority = endpoint_.host;
    if (endpoint_.port != 443) {
        authority += ':';
        authority += std::to_string(endpoint_.port);
    }
    std::vector<nghttp2_nv> nvs;
    nvs.reserve(req.headers.size() + 4);
    auto make_nv = [](std::string_view k, std::string_view v) {
        // Values are referenced by pointer; the caller ensures strings outlive
        // the submit call.
        return nghttp2_nv{
            const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(k.data())),
            const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(v.data())),
            k.size(), v.size(), NGHTTP2_NV_FLAG_NONE,
        };
    };
    nvs.push_back(make_nv(":method",    wire_name(req.method)));
    nvs.push_back(make_nv(":scheme",    "https"));
    nvs.push_back(make_nv(":authority", authority));
    nvs.push_back(make_nv(":path",      req.path));
    for (const auto& h : req.headers) nvs.push_back(make_nv(h.name, h.value));

    // --- submit ---
    sctx.body    = req.body.empty() ? nullptr : &req.body;
    sctx.body_off = 0;
    nghttp2_data_provider dp{};
    dp.source.ptr   = &sctx;
    dp.read_callback = data_read_cb;
    nghttp2_data_provider* dpp = req.body.empty() ? nullptr : &dp;

    // Rebind user_data so the static callbacks find this request's ctx.
    nghttp2_session_set_user_data(session_, &sctx);

    int32_t sid = nghttp2_submit_request(session_, nullptr,
                                         nvs.data(), nvs.size(), dpp, &sctx);
    if (sid < 0)
        return std::unexpected(HttpError::protocol(
            std::string{"nghttp2_submit_request: "} + nghttp2_strerror(sid)));
    sctx.stream_id = sid;

    // --- I/O loop ---
    std::optional<clock_t_::time_point> deadline;
    if (tos.total.count() > 0) deadline = clock_t_::now() + tos.total;

    // Liveness clocks. `last_rx` is refreshed whenever SSL_read returns
    // plaintext bytes (DATA, PING ACK, WINDOW_UPDATE — any frame counts as
    // proof the connection is alive). `last_ping` throttles how often we
    // probe a silent peer.
    const auto start_at = clock_t_::now();
    auto last_rx   = start_at;
    auto last_ping = start_at;

    std::string err;
    while (!sctx.completed) {
        if (cancel && cancel->is_cancelled()) {
            nghttp2_submit_rst_stream(session_, NGHTTP2_FLAG_NONE, sid,
                                      NGHTTP2_CANCEL);
            sctx.error = "cancelled";
        }

        // If we've been silent long enough, poke the peer with a PING. The
        // ACK (or an SSL_write failure on a dead socket) tells us whether
        // the connection is actually alive. Opaque data is nghttp2-supplied.
        if (tos.ping.count() > 0) {
            auto since_rx = clock_t_::now() - last_rx;
            auto since_ping = clock_t_::now() - last_ping;
            if (since_rx >= tos.ping && since_ping >= tos.ping) {
                nghttp2_submit_ping(session_, NGHTTP2_FLAG_NONE, nullptr);
                last_ping = clock_t_::now();
            }
        }

        auto out = pump_send_impl(ssl_, session_, pend_buf_, pend_off_,
                                  pend_len_, &err);
        if (out == PumpOut::Error) return std::unexpected(HttpError::protocol(err));
        size_t bytes_in = 0;
        auto in = pump_recv(ssl_, session_, &err, &bytes_in);
        if (in == PumpIn::Error)   return std::unexpected(HttpError::protocol(err));
        if (bytes_in > 0) last_rx = clock_t_::now();
        if (in == PumpIn::Closed) {
            // Peer closed the TLS session.  If the stream also closed,
            // we're done; otherwise surface as an error.
            if (sctx.completed) break;
            return std::unexpected(HttpError::peer_closed(
                "connection closed by peer mid-stream"));
        }

        if (sctx.completed) break;

        // Enforce the idle guardrail *after* draining whatever arrived this
        // iteration — a chatty peer shouldn't be killed by a clock that ran
        // past the deadline while we were processing its bytes.
        if (tos.idle.count() > 0) {
            auto since_rx = clock_t_::now() - last_rx;
            if (since_rx >= tos.idle) {
                nghttp2_submit_rst_stream(session_, NGHTTP2_FLAG_NONE, sid,
                                          NGHTTP2_CANCEL);
                auto secs = std::chrono::duration_cast<std::chrono::seconds>(since_rx).count();
                return std::unexpected(HttpError::timeout(
                    "h2 idle timeout (no bytes for "
                    + std::to_string(secs) + "s)"));
            }
        }

        // Decide the poll mask.  Always want POLLIN so we can catch
        // unsolicited frames (WINDOW_UPDATE, PING) promptly.  Add POLLOUT
        // only if nghttp2 has bytes ready to send.
        short mask = POLLIN;
        if (nghttp2_session_want_write(session_) || out == PumpOut::WantWrite)
            mask |= POLLOUT;
        if (!nghttp2_session_want_read(session_)
            && !nghttp2_session_want_write(session_))
            break;

        // Cap the poll wait so cancellation tokens land quickly, so we honor
        // the overall deadline if set, and so we re-check the idle/ping
        // clocks on schedule. 200 ms matches Zed's cancellation-latency
        // profile; we further clamp to the nearest liveness deadline so a
        // 90 s idle guard actually fires at t=90 s, not t=90 s+200 ms*N.
        int rem = remaining_ms(deadline, 200);
        if (rem == 0 && deadline) {
            nghttp2_submit_rst_stream(session_, NGHTTP2_FLAG_NONE, sid,
                                      NGHTTP2_CANCEL);
            return std::unexpected(HttpError::timeout("request timed out"));
        }
        auto clamp_to = [&](std::chrono::milliseconds budget,
                            clock_t_::time_point marker) {
            if (budget.count() <= 0) return;
            auto now = clock_t_::now();
            auto until = marker + budget;
            if (until <= now) { rem = 0; return; }
            auto left = std::chrono::duration_cast<ms_t>(until - now).count();
            if (left < rem) rem = static_cast<int>(left);
        };
        clamp_to(tos.idle, last_rx);
        clamp_to(tos.ping, last_rx);
        if (rem < 1) rem = 1;  // always give poll something to chew on
        pollfd pfd{ fd_, mask, 0 };
        int pr = sock_poll(&pfd, 1, rem);
        if (pr < 0) {
            if (sock_intr(sock_last_err())) continue;
            return std::unexpected(HttpError::protocol(
                "h2 poll errno=" + std::to_string(sock_last_err())));
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            return std::unexpected(HttpError::socket_hangup("h2 socket hangup"));
        }
    }

    // Clear the ctx pointer so callbacks for a late peer frame (unlikely)
    // after the stream closes don't touch a dead StreamCtx.
    nghttp2_session_set_user_data(session_, nullptr);

    if (sctx.handler_aborted) {
        if (sctx.error == "cancelled")
            return std::unexpected(HttpError::cancelled("stream aborted by caller"));
        return std::unexpected(HttpError::cancelled(
            sctx.error.empty() ? std::string{"stream aborted by caller"} : sctx.error));
    }
    if (sctx.reset) {
        if (sctx.error == "cancelled")
            return std::unexpected(HttpError::cancelled("stream reset"));
        return std::unexpected(HttpError::protocol(
            sctx.error.empty() ? std::string{"stream reset"} : sctx.error));
    }
    return {};
}

// -----------------------------------------------------------------------
// Dial a fresh connection: TCP, TLS, nghttp2 preamble + SETTINGS.  Returns
// a ready-to-go Connection.
// -----------------------------------------------------------------------
std::expected<std::unique_ptr<Connection>, HttpError>
dial_new(const Endpoint& ep, Timeouts tos, CancelTokenPtr cancel) {
    auto fd_or = dial_tcp(ep, tos, cancel.get());
    if (!fd_or) return std::unexpected(std::move(fd_or).error());
    socket_t fd = *fd_or;

    // SOCKET is a 64-bit handle on Win64, but OpenSSL's BIO_new_socket takes
    // int. The kernel only ever hands out small values (well under 2^31), so
    // the truncation is safe — make it explicit to silence C4244.
    tls::SSL* ssl = tls::wrap_client(static_cast<int>(fd), ep.host);
    if (!ssl) { sock_close(fd); return std::unexpected(HttpError::tls("SSL_new failed")); }

    if (auto r = tls_handshake(fd, ssl, tos, cancel.get()); !r) {
        tls::free_ssl(ssl); sock_close(fd);
        return std::unexpected(std::move(r).error());
    }

    // Require the peer to have negotiated h2 via ALPN.  Anthropic's edge does;
    // a proxy that strips ALPN back to http/1.1 would need separate support,
    // and loudly erroring here is better than a silent nghttp2 protocol error.
    const unsigned char* alpn = nullptr; unsigned int alpn_len = 0;
    SSL_get0_alpn_selected(ssl, &alpn, &alpn_len);
    if (alpn_len != 2 || !alpn || alpn[0] != 'h' || alpn[1] != '2') {
        tls::free_ssl(ssl); sock_close(fd);
        return std::unexpected(HttpError::tls("peer did not negotiate h2 (ALPN)"));
    }

    // --- nghttp2 session ---
    nghttp2_session_callbacks* cbs = nullptr;
    nghttp2_session_callbacks_new(&cbs);
    nghttp2_session_callbacks_set_on_frame_recv_callback       (cbs, on_frame_recv);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback  (cbs, on_data_chunk);
    nghttp2_session_callbacks_set_on_stream_close_callback     (cbs, on_stream_close);
    nghttp2_session_callbacks_set_on_header_callback           (cbs, on_header);

    nghttp2_session* session = nullptr;
    int rc = nghttp2_session_client_new(&session, cbs, /*user_data=*/nullptr);
    nghttp2_session_callbacks_del(cbs);
    if (rc != 0 || !session) {
        tls::free_ssl(ssl); sock_close(fd);
        return std::unexpected(HttpError::protocol(
            std::string{"nghttp2_session_client_new: "} + nghttp2_strerror(rc)));
    }

    // Client preface + SETTINGS.  We bump INITIAL_WINDOW_SIZE to 8 MiB on
    // our side so long response bodies (SSE for a 32k-token message) don't
    // stall on flow control.  The default 64 KiB is exactly the footgun
    // that bit moha under libcurl — we fix it here explicitly rather than
    // trusting an implementation detail.
    nghttp2_settings_entry iv[] = {
        { NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE,    8 * 1024 * 1024 },
        { NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100 },
        { NGHTTP2_SETTINGS_ENABLE_PUSH,            0 },
    };
    nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, iv,
                            sizeof(iv) / sizeof(iv[0]));
    // Raise connection-level window too — default is the same 64 KiB.
    constexpr int32_t kConnectionWindow = 32 * 1024 * 1024;
    nghttp2_session_set_local_window_size(session, NGHTTP2_FLAG_NONE, 0,
                                          kConnectionWindow);

    auto conn = std::make_unique<Connection>(fd, ssl, session, ep);
    if (auto r = conn->pump_initial(tos); !r) return std::unexpected(std::move(r).error());
    return conn;
}

// -----------------------------------------------------------------------
// Connection pool.  Simple LIFO stack per endpoint with two age caps:
// idle TTL (entry hasn't been used in N seconds) and total lifetime
// (entry has been around for N minutes regardless of use). The
// lifetime cap exists because intermediate proxies near Anthropic's
// edge typically run their own connection drains on a 5-15 min TTL —
// our connection looks alive (POLLIN says nothing's pending, nghttp2
// thinks it's healthy) but the proxy will RST the next request.
// Recycling proactively at 10 min sidesteps that ~rare-but-fatal case.
// -----------------------------------------------------------------------
// Direct-connect default: 90 s.  Anthropic's edge holds idle keepalive
// well past a minute, but we recycle proactively so the pool doesn't
// stagnate on a dead-but-silent socket.
//
// Through MOHA_SOCKS_PROXY (the air-gapped-via-SSH path), every reconnect
// is *much* more expensive: SOCKS5 handshake over the SSH channel + DNS
// on the proxy host + TLS handshake on the tunnelled byte stream.
// Together that's typically 200-500 ms of overhead per dial.  The
// progressive slowdown users see "after a few turns" is mostly the 90 s
// idle TTL evicting connections during composer breathing room and the
// next turn paying that overhead before its first SSE byte.  Holding
// connections 5× longer when SOCKS is active sidesteps that — the
// kMaxLifetime ceiling (10 min) still bounds tenure for proxy-drain
// safety so we don't hang on to a connection forever.
inline auto idle_ttl() noexcept -> std::chrono::seconds {
    static const std::chrono::seconds v =
        moha_socks_proxy().active() ? std::chrono::seconds(450)
                                    : std::chrono::seconds(90);
    return v;
}
constexpr auto kMaxLifetime = std::chrono::minutes(10);

struct PooledConn {
    std::unique_ptr<Connection>          conn;
    clock_t_::time_point                  created_at;
    clock_t_::time_point                  released_at;
};

// Cap entries-per-endpoint so a misbehaving caller (e.g. an auto-retry
// loop) can't grow the pool unboundedly and exhaust file descriptors.
// 16 covers any realistic concurrency for a single CLI session — moha
// currently never has more than one in-flight request to api.anthropic.com.
constexpr std::size_t kMaxPoolEntriesPerEndpoint = 16;

// Detect a TCP-level closed/reset socket without consuming any bytes.
// nghttp2's `want_read/write` flags only reflect the protocol-state machine —
// a peer FIN that hasn't been processed yet would still report "alive".
// We non-blocking poll() for POLLHUP/POLLERR/POLLNVAL/POLLIN; a POLLIN with
// a 0-byte MSG_PEEK recv means peer closed cleanly. No data consumed.
[[nodiscard]] bool socket_is_alive(socket_t fd) {
    if (fd == kBadSocket) return false;
    pollfd pfd{ fd, POLLIN, 0 };
    int r = sock_poll(&pfd, 1, 0);
    if (r < 0) {
        if (sock_intr(sock_last_err())) return true;  // EINTR — assume alive
        return false;
    }
    if (r == 0) return true;  // nothing happening — fine
    if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) return false;
    if (pfd.revents & POLLIN) {
        // Peek 1 byte. EOF (0) → peer closed; data → fine; EAGAIN → fine.
        char b;
#if defined(_WIN32)
        int n = ::recv(fd, &b, 1, MSG_PEEK);
#else
        ssize_t n = ::recv(fd, &b, 1, MSG_PEEK | MSG_DONTWAIT);
#endif
        if (n == 0) return false;
        if (n < 0 && !sock_in_progress(sock_last_err())) return false;
    }
    return true;
}

class Pool {
public:
    std::unique_ptr<Connection> acquire(const Endpoint& ep) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = map_.find(ep);
        if (it == map_.end()) return nullptr;
        auto& stack = it->second;
        const auto now = clock_t_::now();
        while (!stack.empty()) {
            auto p = std::move(stack.back());
            stack.pop_back();
            // Three-stage liveness check: idle TTL → nghttp2 protocol state →
            // socket-level FIN/RST. Each stage is cheap and catches a class
            // of stale connections the previous one misses.
            // Four-stage liveness check: total lifetime → idle TTL →
            // nghttp2 protocol state → socket-level FIN/RST. Each stage
            // is cheap and catches a class of stale connections the
            // previous one misses; lifetime in particular catches the
            // proxy-drain case that no client-side check can detect.
            if (now - p.created_at  > kMaxLifetime) continue;
            if (now - p.released_at > idle_ttl())   continue;
            if (!p.conn->is_alive())                continue;
            if (!socket_is_alive(p.conn->fd()))     continue;
            return std::move(p.conn);
        }
        return nullptr;
    }

    void release(std::unique_ptr<Connection> c) {
        if (!c || !c->is_alive()) return;
        std::lock_guard<std::mutex> lk(mu_);
        auto& stack = map_[c->endpoint()];
        if (stack.size() >= kMaxPoolEntriesPerEndpoint) {
            // Drop the oldest (front) — its idle clock is most advanced and
            // it's the most likely candidate for the peer to have closed by
            // the time we'd next reach for it. Keeping the freshest 16 also
            // matches stdio LRU intuition for a small fixed-size pool.
            stack.erase(stack.begin());
        }
        // We don't track the original dial time on Connection; release
        // time stands in for "how old is this entry in the pool." Good
        // enough — the lifetime cap is about pool tenure, not socket age.
        const auto now = clock_t_::now();
        stack.push_back({std::move(c), now, now});
    }

private:
    std::mutex mu_;
    std::unordered_map<Endpoint, std::vector<PooledConn>, EndpointHash> map_;
};

} // namespace

// ---------------------------------------------------------------------------
// Client::Impl
// ---------------------------------------------------------------------------
struct Client::Impl {
    Config cfg;
    Pool   pool;
};

Client::Client() : Client(Config{}) {}

Client::Client(Config cfg) : impl_(std::make_unique<Impl>()) {
    if (const char* e = std::getenv("MOHA_INSECURE"); e && *e == '1')
        cfg.insecure = true;
    impl_->cfg = std::move(cfg);
    ensure_net_init();
    (void)tls::shared_context(impl_->cfg.insecure);
}

Client::~Client() = default;

// Helper: grab a connection from the pool or dial fresh.
static std::expected<std::unique_ptr<Connection>, HttpError>
acquire_or_dial(Pool& pool, const Endpoint& ep, Timeouts tos, CancelTokenPtr cancel) {
    if (auto c = pool.acquire(ep)) return c;
    return dial_new(ep, tos, std::move(cancel));
}

// -----------------------------------------------------------------------
// Transparent retry policy.
// -----------------------------------------------------------------------
// The pool's three-stage liveness check (TTL, nghttp2, socket peek) catches
// most stale entries at acquire time, but a connection can still die in the
// window between "looked alive" and "first SSL_write" — proxies routinely
// kill idle h2 conns at odd intervals, and a TCP FIN can arrive a few ms
// after the peek. Surface that as `http: h2: socket hangup` in the UI is
// user-hostile; the right behaviour is to quietly re-dial and try again.
//
// We only retry when the caller has observed *nothing*: no response :status,
// no on_headers callback, no on_chunk bytes. Once any of those have fired,
// the stream is semantically committed — retrying would duplicate SSE
// events or give the reducer a second set of tool calls.
//
// 3 attempts × small linear backoff gives a hard ceiling of ~300 ms extra
// latency on pathological cases. One attempt is enough for stale-pool; two
// rides out a transient DNS/TLS blip; three is slack.
constexpr int kMaxAttempts = 3;

static bool is_cancelled(const CancelTokenPtr& c) {
    return c && c->is_cancelled();
}

// Backoff between attempts. Attempt 0 → no wait (fastest path for the common
// stale-pool case). Attempt N → 100*N ms. Cancellable: returns false if the
// token trips during the wait so the caller bails without another attempt.
static bool backoff_sleep(int attempt, const CancelTokenPtr& cancel) {
    if (attempt <= 0) return true;
    auto budget = std::chrono::milliseconds(100 * attempt);
    auto end = clock_t_::now() + budget;
    while (clock_t_::now() < end) {
        if (is_cancelled(cancel)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
}

HttpResult
Client::send(const Request& req, Timeouts tos, CancelTokenPtr cancel) {
    Endpoint ep{ req.host, req.port, req.dial_host, req.dial_port };
    HttpError last_err = HttpError::unknown("send: no attempts made");

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (is_cancelled(cancel)) return std::unexpected(HttpError::cancelled());
        if (!backoff_sleep(attempt, cancel))
            return std::unexpected(HttpError::cancelled());

        // First attempt pulls from the pool; subsequent attempts dial fresh —
        // if the pool handed us a corpse once, it'll probably hand us another.
        auto conn_or = (attempt == 0)
            ? acquire_or_dial(impl_->pool, ep, tos, cancel)
            : dial_new(ep, tos, cancel);
        if (!conn_or) { last_err = std::move(conn_or).error(); continue; }
        auto conn = std::move(*conn_or);

        StreamCtx sctx{};
        auto ok = conn->run(req, sctx, tos, cancel);
        if (ok) {
            impl_->pool.release(std::move(conn));
            return Response{ sctx.status, std::move(sctx.headers),
                             std::move(sctx.buffered_body) };
        }
        last_err = std::move(ok).error();

        // If the server actually produced a :status, the request reached
        // application level — the failure is semantic (500, timeout mid-body),
        // not transport, and retrying the POST would duplicate side effects.
        if (sctx.status != 0) break;
        if (is_cancelled(cancel)) break;
    }

    return std::unexpected(std::move(last_err));
}

HttpStreamResult
Client::stream(const Request& req, StreamHandler handler, Timeouts tos,
               CancelTokenPtr cancel) {
    Endpoint ep{ req.host, req.port, req.dial_host, req.dial_port };
    HttpError last_err = HttpError::unknown("stream: no attempts made");
    // Persist across attempts so the synthesised on_headers (if we never got
    // a real one) carries whatever metadata the *last* attempt did collect.
    int     last_status  = 0;
    Headers last_headers;
    bool    any_bytes    = false;   // true once a chunk / header reaches caller

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (is_cancelled(cancel)) { last_err = HttpError::cancelled(); break; }
        if (!backoff_sleep(attempt, cancel)) { last_err = HttpError::cancelled(); break; }

        auto conn_or = (attempt == 0)
            ? acquire_or_dial(impl_->pool, ep, tos, cancel)
            : dial_new(ep, tos, cancel);
        if (!conn_or) { last_err = std::move(conn_or).error(); continue; }
        auto conn = std::move(*conn_or);

        StreamCtx sctx{};
        sctx.handler = &handler;
        auto ok = conn->run(req, sctx, tos, cancel);
        if (sctx.headers_delivered) any_bytes = true;
        last_status  = sctx.status;
        last_headers = std::move(sctx.headers);
        if (ok) {
            impl_->pool.release(std::move(conn));
            return {};
        }
        last_err = std::move(ok).error();

        // Once we've started delivering data to the caller, the stream is
        // committed — retrying would produce duplicate SSE events and a
        // second pass at the reducer's tool_use state machine.
        if (any_bytes) break;
        if (is_cancelled(cancel)) break;
    }

    // If we never got headers, synthesise one so the caller's on_headers
    // contract ("fires at least once") still holds.
    if (!any_bytes && handler.on_headers) {
        handler.on_headers(last_status, last_headers);
    }
    return std::unexpected(std::move(last_err));
}

void Client::prewarm(std::string host, uint16_t port,
                     std::string dial_host, uint16_t dial_port) {
    // Fire-and-forget thread.  Swallows errors — this is opportunistic;
    // the first real request will dial again if prewarm failed.
    std::thread([this,
                 host = std::move(host), port,
                 dial_host = std::move(dial_host), dial_port]() mutable {
        Endpoint ep{ std::move(host), port,
                     std::move(dial_host), dial_port };
        auto r = dial_new(ep, Timeouts{}, /*cancel=*/nullptr);
        if (r) impl_->pool.release(std::move(*r));
    }).detach();
}

Client& default_client() {
    // Deliberately leaked: process lifetime.  Avoids destruction-order
    // races with maya's worker threads that may be mid-request at exit.
    static Client* c = new Client{};
    return *c;
}

namespace {
// Parse a `host[:port]` env-var value into a DialOverride.  Empty / unset
// / ill-formed values silently degrade to "no override" — the user can
// fix the env var and relaunch.  Failing hard would be unfriendly in
// what's already a recovery scenario (air-gapped host, can't reach the
// API directly).
DialOverride parse_dial_env(const char* var_name) {
    const char* v = std::getenv(var_name);
    if (!v || !*v) return {};
    std::string_view s{v};
    auto colon = s.find(':');
    if (colon == std::string_view::npos) {
        // host only → port 443 by default.
        return DialOverride{ std::string{s}, 443 };
    }
    auto host_sv = s.substr(0, colon);
    auto port_sv = s.substr(colon + 1);
    if (host_sv.empty() || port_sv.empty()) return {};
    // strict integer parse: every char of port_sv must be a digit, and
    // the result must fit in uint16_t (≤ 65535).
    unsigned long port_val = 0;
    for (char c : port_sv) {
        if (c < '0' || c > '9') return {};
        port_val = port_val * 10 + static_cast<unsigned long>(c - '0');
        if (port_val > 65535) return {};
    }
    if (port_val == 0) return {};
    return DialOverride{ std::string{host_sv},
                         static_cast<uint16_t>(port_val) };
}
} // namespace

const DialOverride& moha_api_host_override() {
    static const DialOverride cached = parse_dial_env("MOHA_API_HOST");
    return cached;
}

const DialOverride& moha_oauth_host_override() {
    static const DialOverride cached = parse_dial_env("MOHA_OAUTH_HOST");
    return cached;
}

const DialOverride& moha_socks_proxy() {
    static const DialOverride cached = parse_dial_env("MOHA_SOCKS_PROXY");
    return cached;
}

} // namespace moha::http
