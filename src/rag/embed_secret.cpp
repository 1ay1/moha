// embed_secret.cpp — where the embeddings API key lives.
//
// Not in settings.json. A credential in a plaintext config file is a leak
// waiting for a screen-share, a dotfiles commit, or a bug-report attachment,
// and this one is pasted by users who are explicitly working around a locked
// -down environment — exactly the population that can least afford it.
//
// Two tiers, same shape as the rest of agentty's credential handling:
//   1. the OS keystore (libsecret / macOS Keychain) when it is available and
//      enabled, so the secret never touches agentty's own files;
//   2. otherwise a sealed file under the config dir (auth::crypt::seal —
//      machine-bound AEAD), which is what credentials.json already uses.
//
// Keyed by ENDPOINT, not by "the embeddings key": a user may point different
// projects at different gateways, and the key that authenticates one must not
// leak into a request to another.

#include "agentty/rag/embed_secret.hpp"

#include "agentty/auth/auth.hpp"
#include "agentty/auth/cred_crypt.hpp"
#include "agentty/auth/keystore.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace agentty::rag::embed {

namespace {

namespace fs = std::filesystem;

// One file for every endpoint's key, sealed as a unit.
[[nodiscard]] fs::path secrets_path() {
    return auth::config_dir() / "embed_keys.json";
}

[[nodiscard]] std::string keystore_key(const std::string& endpoint) {
    return "agentty:embed:" + endpoint;
}

[[nodiscard]] nlohmann::json read_all() {
    std::ifstream in(secrets_path());
    if (!in) return nlohmann::json::object();
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string raw = ss.str();
    if (raw.empty()) return nlohmann::json::object();
    // Sealed envelope, or (legacy/hand-written) plaintext JSON.
    if (auto opened = auth::crypt::unseal(raw)) {
        try { return nlohmann::json::parse(*opened); } catch (...) { }
        return nlohmann::json::object();
    }
    try {
        auto j = nlohmann::json::parse(raw);
        if (j.is_object()) return j;
    } catch (...) { }
    return nlohmann::json::object();
}

bool write_all(const nlohmann::json& j) {
    std::error_code ec;
    fs::create_directories(secrets_path().parent_path(), ec);
    // Refuse to persist rather than fall back to plaintext: an unsealable
    // secret is a secret we have no business writing to disk.
    auto sealed = auth::crypt::seal(j.dump());
    if (!sealed) return false;

    const auto tmp = fs::path{secrets_path().string() + ".tmp"};
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << *sealed;
        if (!out) return false;
    }
#if !defined(_WIN32)
    // Owner-only before the rename, so the secret is never briefly world-readable.
    fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ec);
#endif
    fs::rename(tmp, secrets_path(), ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
}

} // namespace

std::string load_key(const std::string& endpoint) {
    if (endpoint.empty()) return {};
    if (auth::keystore::available()) {
        std::string out;
        if (auth::keystore::retrieve(keystore_key(endpoint), out) == auth::keystore::Status::Ok)
            return out;
    }
    const auto all = read_all();
    if (auto it = all.find(endpoint); it != all.end() && it->is_string())
        return it->get<std::string>();
    return {};
}

bool store_key(const std::string& endpoint, const std::string& key) {
    if (endpoint.empty()) return false;
    if (key.empty()) return erase_key(endpoint);

    if (auth::keystore::available()
        && auth::keystore::store(keystore_key(endpoint), key) == auth::keystore::Status::Ok) {
        // Belt and braces: drop any stale sealed copy so there is exactly one
        // home for this secret.
        auto all = read_all();
        if (all.erase(endpoint) > 0) (void)write_all(all);
        return true;
    }

    auto all = read_all();
    all[endpoint] = key;
    return write_all(all);
}

bool erase_key(const std::string& endpoint) {
    if (endpoint.empty()) return false;
    bool any = false;
    if (auth::keystore::available())
        any = auth::keystore::remove(keystore_key(endpoint)) == auth::keystore::Status::Ok;
    auto all = read_all();
    if (all.erase(endpoint) > 0) any = write_all(all) || any;
    return any;
}

std::string endpoint_key(const EmbedConfig& c) {
    if (!needs_api_key(c.backend)) return {};
    std::string out;
    out += id_of(c.backend);
    out += "://";
    out += c.host;
    out += ':';
    out += std::to_string(c.port);
    if (!c.path.empty()) out += c.path;
    return out;
}

} // namespace agentty::rag::embed
