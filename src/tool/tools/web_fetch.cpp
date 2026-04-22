#include "moha/tool/tools.hpp"
#include "moha/tool/util/arg_reader.hpp"
#include "moha/tool/util/tool_args.hpp"
#include "moha/io/auth.hpp"

#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace moha::tools {

using json = nlohmann::json;

namespace {

enum class HttpMethod { Get, Head, Post };

HttpMethod parse_method(std::string_view m) {
    if (m == "HEAD") return HttpMethod::Head;
    if (m == "POST") return HttpMethod::Post;
    return HttpMethod::Get;
}

struct WebFetchArgs {
    std::string url;
    HttpMethod method;
    std::vector<std::pair<std::string, std::string>> headers;
};

// Shared with web_search via extern linkage — both tools cap response size
// at 200 KB so a misbehaving server can't blow the model's context.
constexpr size_t kMaxFetchBytes = 200'000;

std::expected<WebFetchArgs, ToolError> parse_web_fetch_args(const json& j) {
    util::ArgReader ar(j);
    auto url_opt = ar.require_str("url");
    if (!url_opt)
        return std::unexpected(ToolError::invalid_args("url required"));
    std::string url = *std::move(url_opt);
    if (!url.starts_with("http://") && !url.starts_with("https://"))
        return std::unexpected(ToolError::invalid_args("url must start with http:// or https://"));
    std::vector<std::pair<std::string, std::string>> hdrs;
    if (const json* h = ar.raw("headers"); h && h->is_object()) {
        for (auto& [k, v] : h->items()) {
            if (v.is_string()) hdrs.emplace_back(k, v.get<std::string>());
            else               hdrs.emplace_back(k, v.dump());
        }
    }
    return WebFetchArgs{
        std::move(url),
        parse_method(ar.str("method", "GET")),
        std::move(hdrs),
    };
}

} // namespace

size_t web_fetch_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    size_t total = size * nmemb;
    if (buf->size() + total > kMaxFetchBytes) {
        buf->append(ptr, kMaxFetchBytes - buf->size());
        return 0;
    }
    buf->append(ptr, total);
    return total;
}

namespace {

ExecResult run_web_fetch(const WebFetchArgs& a) {
    CURL* curl = curl_easy_init();
    if (!curl) return std::unexpected(ToolError::network("failed to initialize HTTP client"));

    std::string body;
    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "User-Agent: moha/0.1");
    for (const auto& [k, v] : a.headers) {
        std::string hdr = k + ": " + v;
        hdrs = curl_slist_append(hdrs, hdr.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, a.url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, web_fetch_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    auth::apply_tls_options(curl);

    if (a.method == HttpMethod::Head)      curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    else if (a.method == HttpMethod::Post) curl_easy_setopt(curl, CURLOPT_POST, 1L);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    char* ct = nullptr;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct);
    std::string content_type = ct ? ct : "";

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK)
        return std::unexpected(ToolError::network(std::string{"fetch failed: "} + curl_easy_strerror(rc)));

    std::ostringstream out;
    out << "HTTP " << http_code;
    if (!content_type.empty()) out << " (" << content_type << ")";
    out << "\n\n" << body;
    if (body.size() >= kMaxFetchBytes) out << "\n[body truncated at 200KB]";
    return ToolOutput{out.str(), std::nullopt};
}

} // namespace

ToolDef tool_web_fetch() {
    ToolDef t;
    t.name = ToolName{std::string{"web_fetch"}};
    t.description = "Fetch the contents of a URL. Supports HTTP/HTTPS. Returns the response "
                    "body, status code, and content type. Use for documentation, APIs, etc.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"url"}},
        {"properties", {
            {"url",     {{"type","string"}, {"description","The URL to fetch"}}},
            {"method",  {{"type","string"}, {"description","HTTP method (default: GET)"}}},
            {"headers", {{"type","object"}, {"description","Additional headers as key-value pairs"}}},
        }},
    };
    t.needs_permission = [](Profile p){ return p != Profile::Write; };
    t.execute = util::adapt<WebFetchArgs>(parse_web_fetch_args, run_web_fetch);
    return t;
}

} // namespace moha::tools
