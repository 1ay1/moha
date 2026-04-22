#include "moha/tool/tools.hpp"
#include "moha/io/auth.hpp"

#include <sstream>
#include <string>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace moha::tools {

using json = nlohmann::json;

namespace {
// Shared with web_search via extern linkage — both tools cap response size
// at 200 KB so a misbehaving server can't blow the model's context.
constexpr size_t kMaxFetchBytes = 200'000;
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
    t.execute = [](const json& args) -> ExecResult {
        std::string url = args.value("url", "");
        std::string method = args.value("method", "GET");
        if (url.empty())
            return std::unexpected(ToolError{"url required"});
        if (!url.starts_with("http://") && !url.starts_with("https://"))
            return std::unexpected(ToolError{"url must start with http:// or https://"});

        CURL* curl = curl_easy_init();
        if (!curl) return std::unexpected(ToolError{"failed to initialize HTTP client"});

        std::string body;
        struct curl_slist* hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, "User-Agent: moha/0.1");

        if (args.contains("headers") && args["headers"].is_object()) {
            for (auto& [k, v] : args["headers"].items()) {
                std::string hdr = k + ": " + v.get<std::string>();
                hdrs = curl_slist_append(hdrs, hdr.c_str());
            }
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, web_fetch_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
        auth::apply_tls_options(curl);

        if (method == "HEAD")
            curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
        else if (method == "POST")
            curl_easy_setopt(curl, CURLOPT_POST, 1L);

        CURLcode rc = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        char* ct = nullptr;
        curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct);
        std::string content_type = ct ? ct : "";

        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);

        if (rc != CURLE_OK)
            return std::unexpected(ToolError{std::string{"fetch failed: "} + curl_easy_strerror(rc)});

        std::ostringstream out;
        out << "HTTP " << http_code;
        if (!content_type.empty()) out << " (" << content_type << ")";
        out << "\n\n" << body;
        if (body.size() >= kMaxFetchBytes) out << "\n[body truncated at 200KB]";
        return ToolOutput{out.str(), std::nullopt};
    };
    return t;
}

} // namespace moha::tools
