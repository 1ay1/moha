#include "moha/tool/tools.hpp"
#include "moha/tool/util/arg_reader.hpp"
#include "moha/tool/util/tool_args.hpp"
#include "moha/io/http.hpp"

#include <algorithm>
#include <chrono>
#include <expected>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

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
    std::string display_description;
};

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
        ar.str("display_description", ""),
    };
}

// Cap the response body so a chatty server can't blow the model's context.
constexpr size_t kMaxFetchBytes = 200'000;

// Parse https://host[:port]/path. http:// is rejected at the arg-validation
// layer above (TLS-only), but we still need to handle the `:port` suffix and
// query strings cleanly.
struct ParsedUrl {
    std::string host;
    uint16_t port = 443;
    std::string path = "/";
};

std::expected<ParsedUrl, std::string> parse_url(std::string_view url) {
    constexpr std::string_view k = "https://";
    if (!url.starts_with(k)) return std::unexpected(std::string{"missing https:// scheme"});
    url.remove_prefix(k.size());
    auto slash = url.find('/');
    auto authority = url.substr(0, slash);
    ParsedUrl out;
    out.path = (slash == std::string_view::npos) ? "/" : std::string{url.substr(slash)};
    if (auto colon = authority.find(':'); colon != std::string_view::npos) {
        out.host.assign(authority.substr(0, colon));
        try {
            out.port = static_cast<uint16_t>(std::stoi(std::string{authority.substr(colon + 1)}));
        } catch (...) { return std::unexpected(std::string{"bad port"}); }
    } else {
        out.host.assign(authority);
    }
    if (out.host.empty()) return std::unexpected(std::string{"empty host"});
    return out;
}

ExecResult run_web_fetch(const WebFetchArgs& a) {
    auto u = parse_url(a.url);
    if (!u) return std::unexpected(
        ToolError::invalid_args("could not parse url: " + a.url + " (" + u.error() + ")"));

    http::Request req;
    req.method = a.method == HttpMethod::Head ? "HEAD"
               : a.method == HttpMethod::Post ? "POST" : "GET";
    req.host = u->host;
    req.port = u->port;
    req.path = u->path;
    req.headers.push_back({"user-agent", "moha/0.1"});
    for (const auto& [k, v] : a.headers) {
        std::string lower; lower.reserve(k.size());
        for (char c : k) lower.push_back(static_cast<char>(std::tolower(c)));
        req.headers.push_back({std::move(lower), v});
    }

    http::Timeouts tos{
        .connect = std::chrono::milliseconds(10'000),
        .total   = std::chrono::milliseconds(30'000),
    };
    auto r = http::default_client().send(req, tos);
    if (!r) return std::unexpected(ToolError::network("fetch failed: " + r.error()));

    std::string content_type;
    for (const auto& h : r->headers)
        if (h.name == "content-type") { content_type = h.value; break; }

    std::string body = std::move(r->body);
    bool truncated = false;
    if (body.size() > kMaxFetchBytes) {
        body.resize(kMaxFetchBytes);
        truncated = true;
    }

    std::ostringstream out;
    out << "HTTP " << r->status;
    if (!content_type.empty()) out << " (" << content_type << ")";
    out << "\n\n" << body;
    if (truncated) out << "\n[body truncated at 200KB]";
    std::string s = out.str();
    if (!a.display_description.empty())
        s = a.display_description + "\n\n" + s;
    return ToolOutput{std::move(s), std::nullopt};
}

} // namespace

ToolDef tool_web_fetch() {
    ToolDef t;
    t.name = ToolName{std::string{"web_fetch"}};
    t.description = "Fetch the contents of a URL. Supports HTTPS. Returns the response "
                    "body, status code, and content type. Use for documentation, APIs, etc.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"url"}},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"url",     {{"type","string"}, {"description","The URL to fetch (https only)"}}},
            {"method",  {{"type","string"}, {"description","HTTP method (default: GET)"}}},
            {"headers", {{"type","object"}, {"description","Additional headers as key-value pairs"}}},
        }},
    };
    t.needs_permission = [](Profile p){ return p != Profile::Write; };
    t.execute = util::adapt<WebFetchArgs>(parse_web_fetch_args, run_web_fetch);
    return t;
}

} // namespace moha::tools
