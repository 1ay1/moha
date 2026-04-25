#include "moha/auth/auth.hpp"

// OAuth config lives with the provider it belongs to; `using` lets the
// existing OAuthConfig:: references below stay short.
#include "moha/provider/anthropic/oauth.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <concepts>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/sha.h>

#include "moha/io/http.hpp"

#ifdef _WIN32
#  include <io.h>
#  include <windows.h>
#  include <shellapi.h>
#else
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace moha::auth {

using OAuthConfig = moha::provider::anthropic::OAuthConfig;

namespace fs = std::filesystem;
using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Credentials accessors — derived views over the variant.
// std::visit + a generic lambda gives exhaustive matching: adding a new
// alternative will fail to compile here until handled.
// ---------------------------------------------------------------------------

static std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool is_valid(const Credentials& c) noexcept {
    return std::visit([](const auto& v) noexcept -> bool {
        using T = std::decay_t<decltype(v)>;
        if      constexpr (std::same_as<T, cred::None>)   return false;
        else if constexpr (std::same_as<T, cred::ApiKey>) return !v.key.empty();
        else /* cred::OAuth */                            return !v.access_token.empty();
    }, c);
}

bool is_expired(const Credentials& c) noexcept {
    return std::visit([](const auto& v) noexcept -> bool {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::same_as<T, cred::OAuth>) {
            if (v.expires_at_ms == 0) return false;
            return now_ms() >= v.expires_at_ms;
        } else {
            return false;   // None / ApiKey never expire
        }
    }, c);
}

std::string header_value(const Credentials& c) {
    return std::visit([](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if      constexpr (std::same_as<T, cred::None>)   return {};
        else if constexpr (std::same_as<T, cred::ApiKey>) return v.key;
        else /* cred::OAuth */ return std::string{"Bearer "} + v.access_token;
    }, c);
}

Style style(const Credentials& c) noexcept {
    return std::holds_alternative<cred::OAuth>(c) ? Style::Bearer : Style::ApiKey;
}

std::string_view persist_tag(const Credentials& c) noexcept {
    return std::visit([](const auto& v) noexcept -> std::string_view {
        using T = std::decay_t<decltype(v)>;
        if      constexpr (std::same_as<T, cred::None>)   return "none";
        else if constexpr (std::same_as<T, cred::ApiKey>) return "api_key";
        else /* cred::OAuth */                            return "oauth";
    }, c);
}

std::string OAuthError::render() const {
    std::string_view kind_str;
    switch (kind) {
        case OAuthErrorKind::Network:      kind_str = "network";      break;
        case OAuthErrorKind::BadResponse:  kind_str = "bad response"; break;
        case OAuthErrorKind::ApiError:     kind_str = "api error";    break;
        case OAuthErrorKind::MissingToken: kind_str = "missing token";break;
    }
    return "[" + std::string{kind_str} + "] " + detail;
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

fs::path config_dir() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    fs::path base;
    if (xdg && *xdg) {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        if (!home || !*home) home = std::getenv("USERPROFILE");
        base = (home && *home) ? fs::path(home) / ".config" : fs::current_path() / ".config";
    }
    fs::path p = base / "moha";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

fs::path credentials_path() { return config_dir() / "credentials.json"; }

// ---------------------------------------------------------------------------
// Pre-warm: open TCP+TLS+h2 to api.anthropic.com while the user is still
// typing, so the first real request skips the handshake. The http::Client's
// pool keeps the connection until used or until the 90 s idle TTL elapses.
// ---------------------------------------------------------------------------
void prewarm_anthropic() {
    static std::atomic<bool> started{false};
    bool expected = false;
    if (!started.compare_exchange_strong(expected, true)) return;
    http::default_client().prewarm("api.anthropic.com", 443);
}

// ---------------------------------------------------------------------------
// Load/save/clear credentials
// ---------------------------------------------------------------------------

static void restrict_perms(const fs::path& p) {
#ifdef _WIN32
    (void)p; // best-effort — Windows ACLs are out of scope here
#else
    ::chmod(p.c_str(), S_IRUSR | S_IWUSR);
#endif
}

// POSIX: create the file with mode 0600 from the start so there is no window
// where it exists world-readable between open() and chmod().
// Windows: fall back to std::ofstream (ACLs are out of scope here).
static bool write_private(const fs::path& p, const std::string& content) {
#ifdef _WIN32
    std::ofstream ofs(p, std::ios::trunc);
    if (!ofs) return false;
    ofs.write(content.data(), (std::streamsize)content.size());
    return static_cast<bool>(ofs);
#else
    int fd = ::open(p.c_str(),
                    O_WRONLY | O_CREAT | O_TRUNC,
                    S_IRUSR | S_IWUSR);
    if (fd < 0) return false;
    const char* buf = content.data();
    size_t remaining = content.size();
    while (remaining > 0) {
        ssize_t n = ::write(fd, buf, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return false;
        }
        buf += n;
        remaining -= (size_t)n;
    }
    return ::close(fd) == 0;
#endif
}

std::optional<Credentials> load_credentials() {
    std::ifstream ifs(credentials_path());
    if (!ifs) return std::nullopt;
    try {
        json j; ifs >> j;
        auto m = j.value("method", "");
        if (m == "oauth") {
            cred::OAuth o;
            o.access_token  = j.value("access_token", "");
            o.refresh_token = j.value("refresh_token", "");
            o.expires_at_ms = j.value("expires_at", std::int64_t{0});
            if (o.access_token.empty()) return std::nullopt;
            return Credentials{std::move(o)};
        }
        if (m == "api_key") {
            cred::ApiKey k;
            k.key = j.value("access_token", "");    // legacy field name
            if (k.key.empty()) return std::nullopt;
            return Credentials{std::move(k)};
        }
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

bool save_credentials(const Credentials& c) {
    json j;
    j["method"] = std::string{persist_tag(c)};
    std::visit([&](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::same_as<T, cred::ApiKey>) {
            j["access_token"]  = v.key;
            j["refresh_token"] = "";
            j["expires_at"]    = std::int64_t{0};
        } else if constexpr (std::same_as<T, cred::OAuth>) {
            j["access_token"]  = v.access_token;
            j["refresh_token"] = v.refresh_token;
            j["expires_at"]    = v.expires_at_ms;
        } else {
            j["access_token"]  = "";
            j["refresh_token"] = "";
            j["expires_at"]    = std::int64_t{0};
        }
    }, c);
    fs::path p = credentials_path();
    if (!write_private(p, j.dump(2))) return false;
    restrict_perms(p);
    return true;
}

bool clear_credentials() {
    std::error_code ec;
    fs::remove(credentials_path(), ec);
    return !ec;
}

// ---------------------------------------------------------------------------
// PKCE helpers
// ---------------------------------------------------------------------------

std::string base64url_no_pad(const unsigned char* data, size_t len) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= len) {
        uint32_t v = (data[i] << 16) | (data[i+1] << 8) | data[i+2];
        out.push_back(tbl[(v >> 18) & 0x3f]);
        out.push_back(tbl[(v >> 12) & 0x3f]);
        out.push_back(tbl[(v >> 6) & 0x3f]);
        out.push_back(tbl[v & 0x3f]);
        i += 3;
    }
    if (i < len) {
        uint32_t v = data[i] << 16;
        if (i + 1 < len) v |= data[i+1] << 8;
        out.push_back(tbl[(v >> 18) & 0x3f]);
        out.push_back(tbl[(v >> 12) & 0x3f]);
        if (i + 1 < len) out.push_back(tbl[(v >> 6) & 0x3f]);
    }
    return out;
}

std::string random_urlsafe(size_t n) {
    static const char charset[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::random_device rd;
    std::mt19937_64 rng(((uint64_t)rd() << 32) ^ rd());
    std::uniform_int_distribution<int> dist(0, sizeof(charset) - 2);
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) out.push_back(charset[dist(rng)]);
    return out;
}

std::string sha256_hex(const std::string& s) {
    unsigned char md[SHA256_DIGEST_LENGTH];
    ::SHA256(reinterpret_cast<const unsigned char*>(s.data()), s.size(), md);
    std::ostringstream oss;
    oss << std::hex;
    for (unsigned char c : md) oss << (c < 16 ? "0" : "") << (int)c;
    return oss.str();
}

std::string code_challenge_s256(const std::string& verifier) {
    unsigned char md[SHA256_DIGEST_LENGTH];
    ::SHA256(reinterpret_cast<const unsigned char*>(verifier.data()),
             verifier.size(), md);
    return base64url_no_pad(md, SHA256_DIGEST_LENGTH);
}

// ---------------------------------------------------------------------------
// HTTP helpers (form-encoded POST against the OAuth endpoint)
// ---------------------------------------------------------------------------

namespace {

// RFC 3986 unreserved set passes through; everything else gets %HH-encoded.
// Mirrors curl_easy_escape's behaviour for the same input set.
std::string url_escape(std::string_view s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() + s.size() / 4);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
         || (c >= '0' && c <= '9')
         || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0xF]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

std::string form_urlencode(const std::vector<std::pair<std::string,std::string>>& kv) {
    std::string out;
    for (size_t i = 0; i < kv.size(); ++i) {
        if (i) out += '&';
        out += url_escape(kv[i].first);
        out += '=';
        out += url_escape(kv[i].second);
    }
    return out;
}

// Endpoint parser for the small set of OAuth URLs we hit. We don't pull in a
// general URL parser: every URL we use is https://, no userinfo, no fragment.
struct ParsedUrl {
    std::string host;
    uint16_t    port = 443;
    std::string path;   // includes leading '/' and any query string
};

std::expected<ParsedUrl, std::string> parse_https_url(std::string_view url) {
    constexpr std::string_view scheme = "https://";
    if (url.substr(0, scheme.size()) != scheme)
        return std::unexpected(std::string{"missing https:// scheme"});
    url.remove_prefix(scheme.size());
    auto slash = url.find('/');
    std::string_view authority = url.substr(0, slash);
    ParsedUrl p;
    p.path = std::string{slash == std::string_view::npos ? "/" : url.substr(slash)};
    auto colon = authority.find(':');
    if (colon == std::string_view::npos) {
        p.host = std::string{authority};
    } else {
        p.host = std::string{authority.substr(0, colon)};
        try {
            int port_int = std::stoi(std::string{authority.substr(colon + 1)});
            if (port_int <= 0 || port_int > 65535)
                return std::unexpected(std::string{"port out of range"});
            p.port = static_cast<uint16_t>(port_int);
        } catch (...) { return std::unexpected(std::string{"bad port"}); }
    }
    if (p.host.empty()) return std::unexpected(std::string{"empty host"});
    return p;
}

// Internal HTTP form-POST result. Distinct from the public TokenResult so
// the typed mapping (HTTP error → OAuthError) happens in exactly one place
// (parse_token_json below).
struct FormPostResult {
    int         status = 0;
    std::string body;
    std::string transport_error;   // non-empty iff request never reached server
};

FormPostResult http_post_form(const std::string& url,
    const std::vector<std::pair<std::string,std::string>>& fields) {
    FormPostResult r;
    auto parsed = parse_https_url(url);
    if (!parsed) {
        r.transport_error = "bad url: " + url + " (" + parsed.error() + ")";
        return r;
    }

    http::Request hreq;
    hreq.method = http::HttpMethod::Post;
    hreq.host   = parsed->host;
    hreq.port   = parsed->port;
    hreq.path   = parsed->path;
    hreq.headers = {
        {"content-type", "application/x-www-form-urlencoded"},
        {"accept",       "application/json"},
        {"user-agent",   std::string{moha::kUserAgent}},
    };
    hreq.body = form_urlencode(fields);

    http::Timeouts tos;
    tos.connect = std::chrono::milliseconds(10'000);
    tos.total   = std::chrono::milliseconds(30'000);

    auto resp = http::default_client().send(hreq, tos);
    if (!resp) { r.transport_error = resp.error().render(); return r; }
    r.status = resp->status;
    r.body   = std::move(resp->body);
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// Token exchange / refresh — the only place HTTP/JSON failure shapes get
// mapped to the typed OAuthErrorKind. Downstream callers see only
// TokenResult and dispatch on `kind`.
// ---------------------------------------------------------------------------

static TokenResult parse_token_json(const std::string& body, int http_status) {
    json j;
    try {
        j = json::parse(body);
    } catch (const std::exception& e) {
        return std::unexpected(OAuthError{
            OAuthErrorKind::BadResponse,
            std::string{"json parse failed: "} + e.what()});
    }
    if (http_status >= 400) {
        return std::unexpected(OAuthError{
            OAuthErrorKind::ApiError,
            j.value("error_description",
                j.value("error",
                    std::string{"HTTP "} + std::to_string(http_status)))});
    }
    OAuthToken tok;
    tok.access_token  = j.value("access_token", "");
    tok.refresh_token = j.value("refresh_token", "");
    tok.expires_in_s  = j.value("expires_in", std::int64_t{0});
    if (tok.access_token.empty()) {
        return std::unexpected(OAuthError{
            OAuthErrorKind::MissingToken,
            "200 OK but no access_token in response"});
    }
    return tok;
}

std::string oauth_authorize_url(const PkceVerifier& verifier,
                                const OAuthState&   state) {
    std::string challenge = code_challenge_s256(verifier.value);
    std::ostringstream url;
    url << OAuthConfig::authorize_url
        << "?response_type=code"
        << "&client_id="             << OAuthConfig::client_id
        << "&redirect_uri="          << url_escape(OAuthConfig::redirect_uri)
        << "&scope="                 << url_escape(OAuthConfig::scopes)
        << "&state="                 << state.value
        << "&code_challenge="        << challenge
        << "&code_challenge_method=S256"
        << "&code=true";
    return url.str();
}

TokenResult exchange_code(const OAuthCode& code,
                          const PkceVerifier& verifier,
                          const OAuthState& state) {
    // Claude's callback often returns "<code>#<state>" joined. Split if present.
    std::string actual_code = code.value;
    auto hash = actual_code.find('#');
    if (hash != std::string::npos) actual_code = actual_code.substr(0, hash);

    auto r = http_post_form(OAuthConfig::token_url, {
        {"grant_type",    "authorization_code"},
        {"code",          actual_code},
        {"client_id",     OAuthConfig::client_id},
        {"redirect_uri",  OAuthConfig::redirect_uri},
        {"code_verifier", verifier.value},
        {"state",         state.value},
    });
    if (!r.transport_error.empty())
        return std::unexpected(OAuthError{
            OAuthErrorKind::Network, r.transport_error});
    return parse_token_json(r.body, r.status);
}

TokenResult refresh_access_token(const RefreshToken& refresh_token) {
    auto r = http_post_form(OAuthConfig::token_url, {
        {"grant_type",    "refresh_token"},
        {"client_id",     OAuthConfig::client_id},
        {"refresh_token", refresh_token.value},
    });
    if (!r.transport_error.empty())
        return std::unexpected(OAuthError{
            OAuthErrorKind::Network, r.transport_error});
    return parse_token_json(r.body, r.status);
}

// ---------------------------------------------------------------------------
// Resolve on startup
// ---------------------------------------------------------------------------

Credentials resolve(const std::string& cli_api_key) {
    if (!cli_api_key.empty())
        return Credentials{cred::ApiKey{cli_api_key}};
    if (const char* k = std::getenv("ANTHROPIC_API_KEY"); k && *k)
        return Credentials{cred::ApiKey{std::string{k}}};
    if (const char* t = std::getenv("CLAUDE_CODE_OAUTH_TOKEN"); t && *t)
        return Credentials{cred::OAuth{std::string{t}, "", 0}};

    auto loaded = load_credentials();
    if (!loaded) return Credentials{cred::None{}};

    // Refresh path is only meaningful for the OAuth alternative; the
    // visit ensures we never read OAuth fields off an ApiKey by mistake.
    return std::visit([&](auto v) -> Credentials {
        using T = std::decay_t<decltype(v)>;
        if constexpr (!std::same_as<T, cred::OAuth>) {
            return Credentials{std::move(v)};
        } else {
            const bool expired = v.expires_at_ms != 0
                              && now_ms() >= v.expires_at_ms;
            if (!expired) return Credentials{std::move(v)};
            if (v.refresh_token.empty()) {
                std::fprintf(stderr,
                    "moha: stored OAuth token is expired and no refresh token.\n"
                    "      run 'moha login'.\n");
                return Credentials{cred::None{}};
            }
            std::fprintf(stderr, "moha: refreshing OAuth token... ");
            auto tr = refresh_access_token(RefreshToken{v.refresh_token});
            if (tr) {
                v.access_token = std::move(tr->access_token);
                if (!tr->refresh_token.empty())
                    v.refresh_token = std::move(tr->refresh_token);
                v.expires_at_ms = tr->expires_in_s
                    ? now_ms() + tr->expires_in_s * 1000 : 0;
                Credentials out{std::move(v)};
                save_credentials(out);
                std::fprintf(stderr, "ok\n");
                return out;
            }
            std::fprintf(stderr, "FAILED: %s\n",
                         tr.error().render().c_str());
            std::fprintf(stderr,
                "moha: stored OAuth token is expired and refresh failed.\n"
                "      run 'moha login' to re-authenticate.\n");
            return Credentials{cred::None{}};
        }
    }, *loaded);
}

// ---------------------------------------------------------------------------
// Browser launch
// ---------------------------------------------------------------------------

void open_browser(const std::string& url) {
#ifdef _WIN32
    ::ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    std::string cmd = "open \"" + url + "\" >/dev/null 2>&1 &";
    (void)std::system(cmd.c_str());
#else
    std::string cmd = "xdg-open \"" + url + "\" >/dev/null 2>&1 &";
    (void)std::system(cmd.c_str());
#endif
}

// ---------------------------------------------------------------------------
// Subcommands
// ---------------------------------------------------------------------------

int cmd_login() {
    std::cout << "moha — authenticate with Claude\n\n"
              << "  1) OAuth via claude.ai (Pro/Max subscription)\n"
              << "  2) Paste an Anthropic API key (sk-ant-...)\n"
              << "\nChoice [1/2]: " << std::flush;
    std::string choice;
    std::getline(std::cin, choice);
    for (auto& c : choice) c = (char)std::tolower((unsigned char)c);

    if (choice == "2" || choice == "api" || choice == "key") {
        std::cout << "\nPaste API key: " << std::flush;
        std::string key;
        std::getline(std::cin, key);
        while (!key.empty() && (key.back() == '\r' || key.back() == '\n'
                                || key.back() == ' ')) key.pop_back();
        if (key.empty()) { std::cerr << "No key entered.\n"; return 1; }
        Credentials c{cred::ApiKey{std::move(key)}};
        if (!save_credentials(c)) {
            std::cerr << "Failed to save credentials.\n"; return 1;
        }
        std::cout << "Saved API key to " << credentials_path().string() << "\n";
        return 0;
    }

    // OAuth PKCE flow
    PkceVerifier verifier{random_urlsafe(128)};
    OAuthState   state{random_urlsafe(32)};
    std::string  auth_url = oauth_authorize_url(verifier, state);

    std::cout << "\nOpening browser to authorize moha...\n"
              << auth_url << "\n\n";
    open_browser(auth_url);

    std::cout << "After logging in, paste the code shown on the callback page: "
              << std::flush;
    std::string code_raw;
    std::getline(std::cin, code_raw);
    while (!code_raw.empty() && (code_raw.back() == '\r' || code_raw.back() == '\n'
                                 || code_raw.back() == ' ')) code_raw.pop_back();
    if (code_raw.empty()) { std::cerr << "No code entered.\n"; return 1; }

    auto tr = exchange_code(OAuthCode{std::move(code_raw)}, verifier, state);
    if (!tr) {
        std::cerr << "Token exchange failed: " << tr.error().render() << "\n";
        return 1;
    }
    Credentials c{cred::OAuth{
        std::move(tr->access_token),
        std::move(tr->refresh_token),
        tr->expires_in_s ? now_ms() + tr->expires_in_s * 1000 : 0
    }};
    if (!save_credentials(c)) {
        std::cerr << "Failed to save credentials.\n"; return 1;
    }
    std::cout << "\n\xE2\x9C\x93 Logged in. Saved to " << credentials_path().string() << "\n";
    return 0;
}

int cmd_logout() {
    auto p = credentials_path();
    std::error_code ec;
    if (!fs::exists(p, ec)) {
        std::cout << "No saved credentials.\n"; return 0;
    }
    if (!clear_credentials()) {
        std::cerr << "Failed to remove " << p.string() << "\n"; return 1;
    }
    std::cout << "Removed " << p.string() << "\n";
    return 0;
}

int cmd_status() {
    std::cout << "Credentials file: " << credentials_path().string() << "\n";
    if (const char* k = std::getenv("ANTHROPIC_API_KEY"); k && *k) {
        std::cout << "ANTHROPIC_API_KEY: set (will be used, overrides file)\n";
    }
    if (const char* t = std::getenv("CLAUDE_CODE_OAUTH_TOKEN"); t && *t) {
        std::cout << "CLAUDE_CODE_OAUTH_TOKEN: set (OAuth via env)\n";
    }
    auto loaded = load_credentials();
    if (!loaded) { std::cout << "Saved credentials: (none)\n"; return 0; }
    std::cout << "Saved method: " << persist_tag(*loaded) << "\n";
    if (auto* o = std::get_if<cred::OAuth>(&*loaded)) {
        if (o->expires_at_ms) {
            auto remaining_s = (o->expires_at_ms - now_ms()) / 1000;
            if (remaining_s <= 0) std::cout << "Token: expired\n";
            else std::cout << "Token expires in " << remaining_s << "s\n";
        } else {
            std::cout << "Token: no expiration info\n";
        }
        std::cout << "Refresh token: "
                  << (o->refresh_token.empty() ? "(none)" : "present") << "\n";
    }
    return 0;
}

} // namespace moha::auth
