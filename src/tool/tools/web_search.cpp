#include "moha/tool/spec.hpp"
#include "moha/tool/tools.hpp"
#include "moha/tool/util/arg_reader.hpp"
#include "moha/tool/util/tool_args.hpp"
#include "moha/io/http.hpp"

#include <chrono>
#include <cctype>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

namespace moha::tools {

using json = nlohmann::json;

namespace {

struct WebSearchArgs {
    std::string query;
    int count;
    std::string display_description;
};

std::expected<WebSearchArgs, ToolError> parse_web_search_args(const json& j) {
    util::ArgReader ar(j);
    auto q_opt = ar.require_str("query");
    if (!q_opt)
        return std::unexpected(ToolError::invalid_args("query required"));
    return WebSearchArgs{
        *std::move(q_opt),
        ar.integer("count", 10),
        ar.str("display_description", ""),
    };
}

// RFC 3986 percent-encode of the unreserved / not-allowed characters that
// appear in a typical query string. Matches the subset curl_easy_escape used.
std::string url_escape(std::string_view s) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        const bool unreserved =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

ExecResult run_web_search(const WebSearchArgs& a) {
    http::Request req;
    req.method = http::HttpMethod::Get;
    req.host   = "html.duckduckgo.com";
    req.port   = 443;
    req.path   = "/html/?q=" + url_escape(a.query);
    req.headers.push_back({"user-agent",
        std::string{moha::kUserAgent} + " (terminal agent)"});

    http::Timeouts tos{
        .connect = std::chrono::milliseconds(10'000),
        .total   = std::chrono::milliseconds(15'000),
    };
    auto r = http::default_client().send(req, tos);
    if (!r) return std::unexpected(ToolError::network("search failed: " + r.error().render()));
    const std::string& body = r->body;

    std::ostringstream out;
    int found = 0;

    // Extract result blocks: class="result__a" … class="result__snippet"
    // DDG's HTML Lite template is fragile — if they change a class name
    // we'll return empty results. Callers see "no results" and can fall
    // back to the bash tool with curl/jq for a richer search.
    size_t pos = 0;
    while (pos < body.size() && found < a.count) {
        auto title_start = body.find("class=\"result__a\"", pos);
        if (title_start == std::string::npos) break;

        auto href_start = body.rfind("href=\"", title_start);
        std::string link;
        if (href_start != std::string::npos && href_start > pos) {
            href_start += 6;
            auto href_end = body.find('"', href_start);
            if (href_end != std::string::npos)
                link = body.substr(href_start, href_end - href_start);
        }

        auto tag_end = body.find('>', title_start);
        if (tag_end == std::string::npos) break;
        auto text_end = body.find('<', tag_end + 1);
        std::string title;
        if (text_end != std::string::npos)
            title = body.substr(tag_end + 1, text_end - tag_end - 1);

        auto snippet_start = body.find("class=\"result__snippet\"", text_end);
        std::string snippet;
        if (snippet_start != std::string::npos) {
            auto stag = body.find('>', snippet_start);
            if (stag != std::string::npos) {
                auto send = body.find("</", stag);
                if (send != std::string::npos) {
                    snippet = body.substr(stag + 1, send - stag - 1);
                    std::string clean;
                    bool in_tag = false;
                    for (char c : snippet) {
                        if (c == '<') in_tag = true;
                        else if (c == '>') in_tag = false;
                        else if (!in_tag) clean += c;
                    }
                    snippet = clean;
                }
            }
            pos = snippet_start + 10;
        } else {
            pos = text_end ? text_end + 1 : body.size();
        }

        auto strip_entities = [](std::string& s) {
            size_t p = 0;
            while ((p = s.find("&amp;", p)) != std::string::npos) s.replace(p, 5, "&");
            p = 0;
            while ((p = s.find("&lt;", p)) != std::string::npos) s.replace(p, 4, "<");
            p = 0;
            while ((p = s.find("&gt;", p)) != std::string::npos) s.replace(p, 4, ">");
            p = 0;
            while ((p = s.find("&quot;", p)) != std::string::npos) s.replace(p, 6, "\"");
            p = 0;
            while ((p = s.find("&#x27;", p)) != std::string::npos) s.replace(p, 6, "'");
            p = 0;
            while ((p = s.find("&nbsp;", p)) != std::string::npos) s.replace(p, 6, " ");
        };

        strip_entities(title);
        strip_entities(snippet);

        if (!title.empty()) {
            out << found + 1 << ". " << title << "\n";
            if (!link.empty()) out << "   " << link << "\n";
            if (!snippet.empty()) out << "   " << snippet << "\n";
            out << "\n";
            found++;
        }
    }

    if (found == 0) return ToolOutput{"no results found for: " + a.query, std::nullopt};
    std::string s = out.str();
    if (!a.display_description.empty())
        s = a.display_description + "\n\n" + s;
    return ToolOutput{std::move(s), std::nullopt};
}

} // namespace

ToolDef tool_web_search() {
    ToolDef t;
    constexpr const auto& kSpec = spec::require<"web_search">();
    t.name = ToolName{std::string{kSpec.name}};
    t.description = "Search the web using DuckDuckGo. Returns search result snippets. "
                    "Use for looking up documentation, error messages, API references.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"query"}},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"query", {{"type","string"}, {"description","Search query"}}},
            {"count", {{"type","integer"}, {"description","Max results (default: 10)"}}},
        }},
    };
    t.effects = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<WebSearchArgs>(parse_web_search_args, run_web_search);
    return t;
}

} // namespace moha::tools
