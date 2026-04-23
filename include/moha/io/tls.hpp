#pragma once
// moha::tls — OpenSSL glue factored out so the http layer doesn't have to
// know about SSL_CTX setup. A single SSL_CTX lives for the process lifetime:
// it holds the verified root store, session cache (TLS 1.3 session tickets),
// and ALPN advertisement. Every new connection grabs an SSL object off it.
//
// System-root verification follows Zed's rustls-platform-verifier model:
//   Linux   — OpenSSL's default paths (/etc/ssl/certs), already wired.
//   macOS   — extract trust roots from SecTrustSettings via Security.framework.
//   Windows — enumerate CertOpenSystemStoreW("ROOT"), convert each to X509,
//             push into the SSL_CTX store.

#include <memory>
#include <string>
#include <string_view>

// Forward-declare OpenSSL types so this header doesn't drag openssl/*.h into
// every translation unit that only needs the handle type.
struct ssl_ctx_st;
struct ssl_st;

namespace moha::tls {

using SSL_CTX = ssl_ctx_st;
using SSL     = ssl_st;

// Opaque handle with process lifetime. First call does the one-time init
// (OPENSSL_init_ssl, build SSL_CTX, load system roots, install ALPN).
// Thread-safe; internal locks mean later callers on other threads just
// see the already-built context.
[[nodiscard]] SSL_CTX* shared_context(bool insecure = false);

// Create a client SSL bound to the given fd + SNI hostname, tied to the
// shared context. Ready for the caller to drive SSL_connect. Returns nullptr
// on allocation failure.
[[nodiscard]] SSL* wrap_client(int fd, std::string_view sni_host);

// Destroy an SSL (frees the nghttp2/openssl plumbing cleanly). The underlying
// fd is owned by the caller and must be closed separately.
void free_ssl(SSL* ssl) noexcept;

// Render the last OpenSSL error queue for a given SSL into a human-readable
// string. Drains the queue. Used by the http layer for error propagation.
[[nodiscard]] std::string last_error(SSL* ssl = nullptr);

} // namespace moha::tls
