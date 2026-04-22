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
#  pragma comment(lib, "ws2_32.lib")
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
// Endpoint — pool key.  Identity is (host,port); two requests to the same
// logical API collapse into the same pool slot.
// -----------------------------------------------------------------------
struct Endpoint {
    std::string host;
    uint16_t    port = 443;
    bool operator==(const Endpoint& o) const {
        return port == o.port && host == o.host;
    }
};
struct EndpointHash {
    size_t operator()(const Endpoint& e) const noexcept {
        // djb2 over host plus port — good enough for the handful of
        // endpoints moha ever talks to.
        size_t h = 5381;
        for (char c : e.host) h = ((h << 5) + h) + static_cast<unsigned char>(c);
        return h ^ (static_cast<size_t>(e.port) * 0x9E3779B97F4A7C15ull);
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
    std::expected<void, std::string> run(
        const Request& req,
        StreamCtx& sctx,
        Timeouts tos,
        CancelTokenPtr cancel);

    // Drive the session until an event flushes all pending frames in one
    // direction, used by the ctor to complete SETTINGS exchange.
    std::expected<void, std::string> pump_initial(Timeouts tos);

private:
    socket_t         fd_      = kBadSocket;
    tls::SSL*        ssl_     = nullptr;
    nghttp2_session* session_ = nullptr;
    Endpoint         endpoint_;
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
std::expected<socket_t, std::string>
dial_tcp(const Endpoint& ep, Timeouts tos, CancelToken* cancel) {
    ensure_net_init();
    addrinfo hints{}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    char port_buf[12]; std::snprintf(port_buf, sizeof(port_buf), "%u", ep.port);
    int gai = ::getaddrinfo(ep.host.c_str(), port_buf, &hints, &res);
    if (gai != 0 || !res) {
        std::string err = "getaddrinfo: ";
        err += gai_strerror(gai);
        return std::unexpected(std::move(err));
    }

    auto deadline = clock_t_::now() + tos.connect;
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
#else
        BOOL one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&one), sizeof(one));
        ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE,
                     reinterpret_cast<const char*>(&one), sizeof(one));
#endif
        sock_set_nonblock(fd);
        int r = ::connect(fd, p->ai_addr, static_cast<int>(p->ai_addrlen));
        if (r == 0) { ::freeaddrinfo(res); return fd; }
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
                return std::unexpected("cancelled during connect");
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
            return fd;
        }
    }
    ::freeaddrinfo(res);
    return std::unexpected(last_err.empty() ? "connect: no address worked" : last_err);
}

// -----------------------------------------------------------------------
// TLS handshake loop.  SSL is already attached to fd; we drive SSL_connect
// and translate WANT_READ/WANT_WRITE into poll() waits, honoring the same
// connect deadline.
// -----------------------------------------------------------------------
std::expected<void, std::string>
tls_handshake(socket_t fd, tls::SSL* ssl, Timeouts tos, CancelToken* cancel) {
    auto deadline = clock_t_::now() + tos.connect;
    while (true) {
        int r = SSL_connect(ssl);
        if (r == 1) return {};
        int e = SSL_get_error(ssl, r);
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) {
            return std::unexpected("tls: " + tls::last_error(ssl));
        }
        if (cancel && cancel->is_cancelled())
            return std::unexpected("cancelled during tls handshake");
        int rem = remaining_ms(deadline, 200);
        if (rem <= 0) return std::unexpected("tls: handshake timed out");
        pollfd pfd{ fd, (e == SSL_ERROR_WANT_READ) ? (short)POLLIN : (short)POLLOUT, 0 };
        int pr = sock_poll(&pfd, 1, rem);
        if (pr < 0) {
            if (sock_intr(sock_last_err())) continue;
            return std::unexpected("tls: poll errno=" + std::to_string(sock_last_err()));
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            return std::unexpected("tls: poll hangup");
    }
}

// Pump outgoing frames: pull bytes off nghttp2 and write through TLS.
// Returns WANT_* if TLS couldn't write the whole mem_send() chunk.
// `wrote_any` reports whether progress was made this call.
enum class PumpOut { Idle, WantRead, WantWrite, Error };
static PumpOut pump_send(tls::SSL* ssl, nghttp2_session* session,
                         std::string* err) {
    while (true) {
        const uint8_t* data = nullptr;
        ssize_t n = nghttp2_session_mem_send(session, &data);
        if (n < 0) {
            if (err) *err = std::string{"nghttp2_session_mem_send: "}
                          + nghttp2_strerror(static_cast<int>(n));
            return PumpOut::Error;
        }
        if (n == 0) return PumpOut::Idle;
        size_t off = 0;
        while (off < static_cast<size_t>(n)) {
            size_t put = 0;
            int r = SSL_write(ssl, data + off, static_cast<int>(n - off));
            if (r > 0) { put = static_cast<size_t>(r); off += put; continue; }
            int e = SSL_get_error(ssl, r);
            if (e == SSL_ERROR_WANT_WRITE) return PumpOut::WantWrite;
            if (e == SSL_ERROR_WANT_READ)  return PumpOut::WantRead;
            if (err) *err = "SSL_write: " + tls::last_error(ssl);
            return PumpOut::Error;
        }
    }
}

// Pump incoming bytes: read from TLS, feed to nghttp2.  Returns WANT_* when
// TLS has nothing more buffered.
enum class PumpIn { Idle, WantRead, WantWrite, Closed, Error };
static PumpIn pump_recv(tls::SSL* ssl, nghttp2_session* session,
                        std::string* err) {
    uint8_t buf[16 * 1024];
    while (true) {
        int r = SSL_read(ssl, buf, sizeof(buf));
        if (r > 0) {
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

std::expected<void, std::string>
Connection::pump_initial(Timeouts tos) {
    // Drive the first SETTINGS / window-update exchange so the connection
    // is fully usable before we return it from dial_*.  Bounded by the
    // connect deadline passed in.
    auto deadline = clock_t_::now() + tos.connect;
    std::string err;
    while (nghttp2_session_want_write(session_)) {
        auto s = pump_send(ssl_, session_, &err);
        if (s == PumpOut::Error) return std::unexpected(err);
        if (s == PumpOut::Idle) break;
        int rem = remaining_ms(deadline, 200);
        if (rem <= 0) return std::unexpected("h2: initial send timed out");
        pollfd pfd{ fd_, (s == PumpOut::WantRead) ? (short)POLLIN : (short)POLLOUT, 0 };
        int pr = sock_poll(&pfd, 1, rem);
        if (pr < 0 && !sock_intr(sock_last_err()))
            return std::unexpected("h2: poll errno=" + std::to_string(sock_last_err()));
    }
    return {};
}

std::expected<void, std::string>
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
    nvs.push_back(make_nv(":method",    req.method));
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
        return std::unexpected(std::string{"nghttp2_submit_request: "}
                             + nghttp2_strerror(sid));
    sctx.stream_id = sid;

    // --- I/O loop ---
    std::optional<clock_t_::time_point> deadline;
    if (tos.total.count() > 0) deadline = clock_t_::now() + tos.total;

    std::string err;
    while (!sctx.completed) {
        if (cancel && cancel->is_cancelled()) {
            nghttp2_submit_rst_stream(session_, NGHTTP2_FLAG_NONE, sid,
                                      NGHTTP2_CANCEL);
            sctx.error = "cancelled";
        }
        auto out = pump_send(ssl_, session_, &err);
        if (out == PumpOut::Error) return std::unexpected(err);
        auto in = pump_recv(ssl_, session_, &err);
        if (in == PumpIn::Error)   return std::unexpected(err);
        if (in == PumpIn::Closed) {
            // Peer closed the TLS session.  If the stream also closed,
            // we're done; otherwise surface as an error.
            if (sctx.completed) break;
            return std::unexpected("connection closed by peer mid-stream");
        }

        if (sctx.completed) break;

        // Decide the poll mask.  Always want POLLIN so we can catch
        // unsolicited frames (WINDOW_UPDATE, PING) promptly.  Add POLLOUT
        // only if nghttp2 has bytes ready to send.
        short mask = POLLIN;
        if (nghttp2_session_want_write(session_) || out == PumpOut::WantWrite)
            mask |= POLLOUT;
        if (!nghttp2_session_want_read(session_)
            && !nghttp2_session_want_write(session_))
            break;

        // Cap the poll wait so cancellation tokens land quickly and so
        // we honor an overall deadline if one was set.  200 ms matches
        // Zed's own cancellation-latency profile closely enough.
        int rem = remaining_ms(deadline, 200);
        if (rem == 0 && deadline) {
            nghttp2_submit_rst_stream(session_, NGHTTP2_FLAG_NONE, sid,
                                      NGHTTP2_CANCEL);
            return std::unexpected("request timed out");
        }
        pollfd pfd{ fd_, mask, 0 };
        int pr = sock_poll(&pfd, 1, rem);
        if (pr < 0) {
            if (sock_intr(sock_last_err())) continue;
            return std::unexpected("h2: poll errno=" + std::to_string(sock_last_err()));
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            return std::unexpected("h2: socket hangup");
        }
    }

    // Clear the ctx pointer so callbacks for a late peer frame (unlikely)
    // after the stream closes don't touch a dead StreamCtx.
    nghttp2_session_set_user_data(session_, nullptr);

    if (sctx.handler_aborted)
        return std::unexpected(sctx.error.empty()
                               ? std::string{"stream aborted by caller"}
                               : sctx.error);
    if (sctx.reset)
        return std::unexpected(sctx.error.empty()
                               ? std::string{"stream reset"}
                               : sctx.error);
    return {};
}

// -----------------------------------------------------------------------
// Dial a fresh connection: TCP, TLS, nghttp2 preamble + SETTINGS.  Returns
// a ready-to-go Connection.
// -----------------------------------------------------------------------
std::expected<std::unique_ptr<Connection>, std::string>
dial_new(const Endpoint& ep, Timeouts tos, CancelTokenPtr cancel) {
    auto fd_or = dial_tcp(ep, tos, cancel.get());
    if (!fd_or) return std::unexpected(fd_or.error());
    socket_t fd = *fd_or;

    tls::SSL* ssl = tls::wrap_client(fd, ep.host);
    if (!ssl) { sock_close(fd); return std::unexpected("tls: SSL_new failed"); }

    if (auto r = tls_handshake(fd, ssl, tos, cancel.get()); !r) {
        tls::free_ssl(ssl); sock_close(fd);
        return std::unexpected(r.error());
    }

    // Require the peer to have negotiated h2 via ALPN.  Anthropic's edge does;
    // a proxy that strips ALPN back to http/1.1 would need separate support,
    // and loudly erroring here is better than a silent nghttp2 protocol error.
    const unsigned char* alpn = nullptr; unsigned int alpn_len = 0;
    SSL_get0_alpn_selected(ssl, &alpn, &alpn_len);
    if (alpn_len != 2 || !alpn || alpn[0] != 'h' || alpn[1] != '2') {
        tls::free_ssl(ssl); sock_close(fd);
        return std::unexpected("tls: peer did not negotiate h2 (ALPN)");
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
        return std::unexpected(std::string{"nghttp2_session_client_new: "}
                             + nghttp2_strerror(rc));
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
    if (auto r = conn->pump_initial(tos); !r) return std::unexpected(r.error());
    return conn;
}

// -----------------------------------------------------------------------
// Connection pool.  Simple LIFO stack per endpoint with an idle deadline;
// entries older than kIdleTtl are reaped before reuse.
// -----------------------------------------------------------------------
constexpr auto kIdleTtl = std::chrono::seconds(90);

struct PooledConn {
    std::unique_ptr<Connection>          conn;
    clock_t_::time_point                  released_at;
};

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
            if (!p.conn->is_alive()) continue;
            if (now - p.released_at > kIdleTtl) continue;
            return std::move(p.conn);
        }
        return nullptr;
    }

    void release(std::unique_ptr<Connection> c) {
        if (!c || !c->is_alive()) return;
        std::lock_guard<std::mutex> lk(mu_);
        map_[c->endpoint()].push_back({std::move(c), clock_t_::now()});
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
static std::expected<std::unique_ptr<Connection>, std::string>
acquire_or_dial(Pool& pool, const Endpoint& ep, Timeouts tos, CancelTokenPtr cancel) {
    if (auto c = pool.acquire(ep)) return c;
    return dial_new(ep, tos, std::move(cancel));
}

std::expected<Response, std::string>
Client::send(const Request& req, Timeouts tos, CancelTokenPtr cancel) {
    Endpoint ep{ req.host, req.port };
    auto conn_or = acquire_or_dial(impl_->pool, ep, tos, cancel);
    if (!conn_or) return std::unexpected(conn_or.error());
    auto conn = std::move(*conn_or);

    StreamCtx sctx{};
    auto ok = conn->run(req, sctx, tos, std::move(cancel));
    if (ok) impl_->pool.release(std::move(conn));
    if (!ok) return std::unexpected(ok.error());

    return Response{ sctx.status, std::move(sctx.headers),
                     std::move(sctx.buffered_body) };
}

std::expected<void, std::string>
Client::stream(const Request& req, StreamHandler handler, Timeouts tos,
               CancelTokenPtr cancel) {
    Endpoint ep{ req.host, req.port };
    auto conn_or = acquire_or_dial(impl_->pool, ep, tos, cancel);
    if (!conn_or) return std::unexpected(conn_or.error());
    auto conn = std::move(*conn_or);

    StreamCtx sctx{};
    sctx.handler = &handler;
    auto ok = conn->run(req, sctx, tos, std::move(cancel));
    if (ok) impl_->pool.release(std::move(conn));

    // If the caller never got a headers callback (e.g. stream failed before
    // the status line arrived), synthesise one so the caller isn't left
    // waiting for a signal that won't come.
    if (!sctx.headers_delivered && handler.on_headers) {
        handler.on_headers(sctx.status, sctx.headers);
    }

    if (!ok) return std::unexpected(ok.error());
    return {};
}

void Client::prewarm(std::string host, uint16_t port) {
    // Fire-and-forget thread.  Swallows errors — this is opportunistic;
    // the first real request will dial again if prewarm failed.
    std::thread([this, host = std::move(host), port]() mutable {
        Endpoint ep{ std::move(host), port };
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

} // namespace moha::http
