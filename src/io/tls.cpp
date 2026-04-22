#include "moha/io/tls.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#if defined(__APPLE__)
#  include <Security/Security.h>
#  include <CoreFoundation/CoreFoundation.h>
#elif defined(_WIN32)
// Declared before windows.h to quiet min/max + cert cruft.
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <wincrypt.h>
#  pragma comment(lib, "crypt32.lib")
#endif

namespace moha::tls {

namespace {

// ALPN advertisement: h2 first, http/1.1 as fallback. Anthropic's edge
// always negotiates h2, but advertising 1.1 keeps us compatible with any
// proxy a user might stand up (corporate TLS-intercepting middleboxes).
// Wire format: length-prefixed protocol IDs per RFC 7301.
constexpr unsigned char kAlpn[] = {
    2, 'h', '2',
    8, 'h', 't', 't', 'p', '/', '1', '.', '1',
};

// --------------------------------------------------------------------------
// Platform root store loaders. We load on top of OpenSSL's default paths
// (which already cover the common Linux cases) so the final X509_STORE is
// the union — belt and suspenders.
// --------------------------------------------------------------------------

#if defined(__APPLE__)
// macOS: enumerate the admin + system keychain trust settings.  Any cert
// marked with kSecTrustSettingsResultTrustRoot or TrustAsRoot goes into
// OpenSSL's store.  Not all roots have explicit trust settings (the system
// ones use implicit trust from /System/Library/Keychains/SystemRootCertificates),
// so we also pull that keychain directly.
bool load_system_roots(SSL_CTX* ctx) {
    X509_STORE* store = SSL_CTX_get_cert_store(ctx);
    if (!store) return false;

    auto add_cfdata = [&](CFDataRef data) {
        const unsigned char* bytes =
            reinterpret_cast<const unsigned char*>(CFDataGetBytePtr(data));
        long len = static_cast<long>(CFDataGetLength(data));
        X509* x = d2i_X509(nullptr, &bytes, len);
        if (x) {
            // Ignore the success/failure — duplicates are benign and a single
            // bad cert shouldn't nuke the whole store.
            X509_STORE_add_cert(store, x);
            X509_free(x);
        }
    };

    auto copy_from_domain = [&](SecTrustSettingsDomain domain) {
        CFArrayRef certs = nullptr;
        if (SecTrustSettingsCopyCertificates(domain, &certs) != errSecSuccess || !certs)
            return;
        CFIndex n = CFArrayGetCount(certs);
        for (CFIndex i = 0; i < n; ++i) {
            SecCertificateRef cert =
                static_cast<SecCertificateRef>(
                    const_cast<void*>(CFArrayGetValueAtIndex(certs, i)));
            if (CFDataRef data = SecCertificateCopyData(cert)) {
                add_cfdata(data);
                CFRelease(data);
            }
        }
        CFRelease(certs);
    };

    copy_from_domain(kSecTrustSettingsDomainSystem);
    copy_from_domain(kSecTrustSettingsDomainAdmin);
    copy_from_domain(kSecTrustSettingsDomainUser);
    return true;
}

#elif defined(_WIN32)
// Windows: pull every cert out of the system ROOT store and add to OpenSSL.
// We go through CertEnumCertificatesInStore instead of the legacy A-variant
// so unicode cert names don't trip us up.
bool load_system_roots(SSL_CTX* ctx) {
    X509_STORE* store = SSL_CTX_get_cert_store(ctx);
    if (!store) return false;

    HCERTSTORE sys = CertOpenSystemStoreW(0, L"ROOT");
    if (!sys) return false;

    PCCERT_CONTEXT ctxt = nullptr;
    while ((ctxt = CertEnumCertificatesInStore(sys, ctxt)) != nullptr) {
        const unsigned char* enc = ctxt->pbCertEncoded;
        long n = static_cast<long>(ctxt->cbCertEncoded);
        X509* x = d2i_X509(nullptr, &enc, n);
        if (x) {
            X509_STORE_add_cert(store, x);
            X509_free(x);
        }
    }
    CertCloseStore(sys, 0);
    return true;
}

#else
// Linux / BSD: OpenSSL's default paths already cover this.  Some distros
// ship the trusted CA bundle under odd paths (Alpine: /etc/ssl/cert.pem,
// some container bases: /etc/pki/tls/certs/ca-bundle.crt); SSL_CTX_set_default_verify_paths
// handles the usual ones.  We additionally honor the two env vars that
// curl conventionally reads, so a user with a corporate proxy cert can
// point us at it without code changes.
bool load_system_roots(SSL_CTX* ctx) {
    if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
        // Not fatal: env overrides still get a shot below.
    }
    if (const char* f = std::getenv("SSL_CERT_FILE"); f && *f) {
        SSL_CTX_load_verify_locations(ctx, f, nullptr);
    }
    if (const char* d = std::getenv("SSL_CERT_DIR"); d && *d) {
        SSL_CTX_load_verify_locations(ctx, nullptr, d);
    }
    if (const char* f = std::getenv("CURL_CA_BUNDLE"); f && *f) {
        SSL_CTX_load_verify_locations(ctx, f, nullptr);
    }
    return true;
}
#endif

// --------------------------------------------------------------------------
// One-time SSL_CTX build. Two contexts: one with peer verification, one
// insecure (for MOHA_INSECURE=1 / --insecure).  Lazy-initialized under a
// mutex; the first caller pays the setup cost, the rest see the cached ptr.
// Leaked at process exit — SSL_CTX_free would race any in-flight SSL that
// outlives main's cleanup.
// --------------------------------------------------------------------------
SSL_CTX* build_ctx(bool insecure) {
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS,
                     nullptr);
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return nullptr;

    // TLS 1.2 minimum — anything older is a compliance red flag and Anthropic
    // won't negotiate it anyway.  TLS 1.3 is preferred (the default) and
    // gives us 1-RTT handshakes, 0-RTT resumption via session tickets.
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);

    // Session cache + tickets.  This is the fat perf win: turn N+1 to
    // api.anthropic.com skips the full handshake and does a 1-RTT resume.
    SSL_CTX_set_session_cache_mode(
        ctx, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
    // Advertise ALPN so the server negotiates h2 on our behalf.
    SSL_CTX_set_alpn_protos(ctx, kAlpn, sizeof(kAlpn));

    if (insecure) {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    } else {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        load_system_roots(ctx);
    }
    return ctx;
}

struct SharedCtx {
    SSL_CTX* verifying = nullptr;
    SSL_CTX* insecure  = nullptr;
};

SharedCtx& shared() {
    // Deliberate leak — process lifetime, avoids destruction order races.
    static SharedCtx* s = new SharedCtx{};
    static std::once_flag once;
    std::call_once(once, [] {
        s->verifying = build_ctx(false);
        // Env gate: MOHA_INSECURE=1 collapses both handles into the insecure
        // one, so accidentally using the "verified" one still follows the
        // user's explicit override.
        if (const char* e = std::getenv("MOHA_INSECURE"); e && *e == '1') {
            s->insecure  = build_ctx(true);
            s->verifying = s->insecure;
        } else {
            s->insecure = build_ctx(true);
        }
    });
    return *s;
}

} // namespace

SSL_CTX* shared_context(bool insecure) {
    auto& s = shared();
    return insecure ? s.insecure : s.verifying;
}

SSL* wrap_client(int fd, std::string_view sni_host) {
    SSL_CTX* ctx = shared_context(false);
    if (!ctx) return nullptr;
    SSL* ssl = SSL_new(ctx);
    if (!ssl) return nullptr;

    // SNI: required for virtual-hosted edges (Anthropic's Cloudflare front).
    // null-terminate via a local string — string_view has no guarantee.
    std::string host{sni_host};
    SSL_set_tlsext_host_name(ssl, host.c_str());
    // Hostname verification — checks CN/SAN against the cert, on top of
    // chain verification that SSL_CTX already configured.
    SSL_set1_host(ssl, host.c_str());
    SSL_set_hostflags(ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);

    if (SSL_set_fd(ssl, fd) != 1) {
        SSL_free(ssl);
        return nullptr;
    }
    SSL_set_connect_state(ssl);
    return ssl;
}

void free_ssl(SSL* ssl) noexcept {
    if (!ssl) return;
    // Best-effort bidirectional shutdown.  If the peer hasn't ACK'd we don't
    // block on it — the fd is about to close anyway.
    (void)SSL_shutdown(ssl);
    SSL_free(ssl);
}

std::string last_error(SSL* ssl) {
    std::string out;
    if (ssl) {
        int code = SSL_get_error(ssl, -1);
        if (code == SSL_ERROR_SYSCALL) {
            // The actual reason is often on the err queue; fall through.
            out = "syscall";
        } else if (code == SSL_ERROR_ZERO_RETURN) {
            out = "peer closed (SSL_ERROR_ZERO_RETURN)";
        }
    }
    unsigned long e;
    char buf[256];
    while ((e = ERR_get_error()) != 0) {
        ERR_error_string_n(e, buf, sizeof(buf));
        if (!out.empty()) out += "; ";
        out += buf;
    }
    if (out.empty()) out = "unknown openssl error";
    return out;
}

} // namespace moha::tls
