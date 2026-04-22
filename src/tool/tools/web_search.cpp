#include "moha/tool/tools.hpp"
#include "moha/io/auth.hpp"

#include <sstream>
#include <string>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace moha::tools {

using json = nlohmann::json;

// Shared with web_fetch.cpp — appends to userdata up to a 200 KB cap.
size_t web_fetch_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata);

ToolDef tool_web_search() {
    ToolDef t;
    t.name = ToolName{std::string{"web_search"}};
    t.description = "Search the web using DuckDuckGo. Returns search result snippets. "
                    "Use for looking up documentation, error messages, API references.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"query"}},
        {"properties", {
            {"query", {{"type","string"}, {"description","Search query"}}},
            {"count", {{"type","integer"}, {"description","Max results (default: 10)"}}},
        }},
    };
    t.needs_permission = [](Profile p){ return p != Profile::Write; };
    t.execute = [](const json& args) -> ExecResult {
        std::string query = args.value("query", "");
        if (query.empty())
            return std::unexpected(ToolError{"query required"});

        CURL* curl = curl_easy_init();
        if (!curl) return std::unexpected(ToolError{"failed to initialize HTTP client"});

        char* encoded = curl_easy_escape(curl, query.c_str(), (int)query.size());
        std::string url = std::string{"https://html.duckduckgo.com/html/?q="} + encoded;
        curl_free(encoded);

        std::string body;
        struct curl_slist* hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, "User-Agent: moha/0.1 (terminal agent)");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, web_fetch_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
        auth::apply_tls_options(curl);

        CURLcode rc = curl_easy_perform(curl);
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);

        if (rc != CURLE_OK)
            return std::unexpected(ToolError{std::string{"search failed: "} + curl_easy_strerror(rc)});

        int max_results = args.value("count", 10);
        std::ostringstream out;
        int found = 0;

        // Extract result blocks: class="result__a" … class="result__snippet"
        // DDG's HTML Lite template is fragile — if they change a class name
        // we'll return empty results. Callers see "no results" and can fall
        // back to the bash tool with curl/jq for a richer search.
        size_t pos = 0;
        while (pos < body.size() && found < max_results) {
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

        if (found == 0) return ToolOutput{"no results found for: " + query, std::nullopt};
        return ToolOutput{out.str(), std::nullopt};
    };
    return t;
}

} // namespace moha::tools
