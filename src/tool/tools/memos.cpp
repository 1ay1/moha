// `remember` / `forget` / `memos` — explicit hooks into the persistent
// workspace memory the agent accumulates.
//
// `investigate` writes memos automatically; these tools let the model
// (or a curious user via the wire) curate that memory directly:
//
//   remember(topic, content, files?)   — append a fact the agent
//        wants to keep handy across turns. Useful when knowledge is
//        learned via a normal read/grep/edit loop, not a dedicated
//        investigate run.
//
//   forget(id_or_query)                — drop a memo that's stale,
//        wrong, or outdated.
//
//   memos()                            — list every memo with its id,
//        topic, age, and which files it references. The model uses
//        this to discover what's already known before deciding to
//        investigate / read / grep again.

#include "moha/memory/memo_store.hpp"
#include "moha/tool/spec.hpp"
#include "moha/tool/tools.hpp"
#include "moha/tool/util/arg_reader.hpp"
#include "moha/tool/util/tool_args.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace moha::tools {

using json = nlohmann::json;

namespace {

// ── remember ───────────────────────────────────────────────────────
struct RememberArgs {
    std::string topic;
    std::string content;
    std::vector<std::string> files;
    std::string display_description;
};

[[nodiscard]] std::expected<RememberArgs, ToolError>
parse_remember_args(const json& j) {
    util::ArgReader ar(j);
    auto t = ar.require_str("topic");
    if (!t) return std::unexpected(ToolError::invalid_args(
        "topic required: a one-line description of what this memo is about"));
    auto c = ar.require_str("content");
    if (!c) return std::unexpected(ToolError::invalid_args(
        "content required: the body of the fact to remember"));
    std::vector<std::string> files;
    if (const json* fs = ar.raw("files"); fs && fs->is_array()) {
        for (const auto& v : *fs) {
            if (v.is_string()) files.push_back(v.get<std::string>());
        }
    }
    return RememberArgs{
        *std::move(t), *std::move(c),
        std::move(files),
        ar.str("display_description", ""),
    };
}

ExecResult run_remember(const RememberArgs& a) {
    auto& store = memory::shared();
    if (!store.ready())
        return std::unexpected(ToolError::io(
            "memo store not bound to a workspace"));
    memory::Memo m;
    m.query     = a.topic;
    m.synthesis = a.content;
    m.created_at = std::chrono::system_clock::now();
    m.file_refs  = a.files;
    store.add(std::move(m));
    std::ostringstream out;
    out << "\xe2\x9c\x93 remembered: \"" << a.topic << "\"\n"
        << "  " << a.content.size() << " chars";
    if (!a.files.empty()) {
        out << "  ·  " << a.files.size() << " file ref"
            << (a.files.size() == 1 ? "" : "s");
    }
    out << "  ·  " << store.size() << " memo"
        << (store.size() == 1 ? "" : "s") << " total\n";
    out << "This memo will be in the system prompt for every future turn "
           "in this workspace until forgotten.";
    std::string body = out.str();
    if (!a.display_description.empty())
        body = a.display_description + "\n\n" + body;
    return ToolOutput{std::move(body), std::nullopt};
}

ToolDef tool_remember_impl() {
    ToolDef t;
    constexpr const auto& kSpec = spec::require<"remember">();
    t.name = ToolName{std::string{kSpec.name}};
    t.description =
        "Save a fact about this codebase to long-term memory. Persisted "
        "to <workspace>/.moha/memos.json and INJECTED INTO THE SYSTEM "
        "PROMPT for every subsequent turn (cached on Anthropic's side, "
        "so the cost amortises after the first turn). Use when you've "
        "learned something durable that you'd otherwise have to "
        "rediscover next session — architecture decisions, where things "
        "live, gotchas, contracts between modules. NOT for transient "
        "in-conversation context. Format the `content` as concise "
        "markdown; you can include code spans, paths, line numbers.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"topic","content"}},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"topic", {{"type","string"},
                {"description","One-line topic — the question this memo answers, e.g. \"how does the OAuth refresh flow work?\"."}}},
            {"content", {{"type","string"},
                {"description","Markdown body. Names, line numbers, key relationships, gotchas."}}},
            {"files", {{"type","array"}, {"items", {{"type","string"}}},
                {"description","Optional: workspace-relative paths whose mtime governs whether this memo is still fresh."}}},
        }},
    };
    t.effects = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<RememberArgs>(parse_remember_args, run_remember);
    return t;
}

// ── forget ─────────────────────────────────────────────────────────
struct ForgetArgs {
    std::string target;     // id or substring of query
    std::string display_description;
};

[[nodiscard]] std::expected<ForgetArgs, ToolError>
parse_forget_args(const json& j) {
    util::ArgReader ar(j);
    auto t = ar.require_str("target");
    if (!t) return std::unexpected(ToolError::invalid_args(
        "target required: a memo id (from `memos()`) or a substring of its topic"));
    return ForgetArgs{
        *std::move(t),
        ar.str("display_description", ""),
    };
}

ExecResult run_forget(const ForgetArgs& a) {
    // Walk the memo list, find matches, remove. Since MemoStore doesn't
    // expose a remove API, do the work inline by clearing + re-adding.
    // Simpler: extend MemoStore with a remove() method.
    auto& store = memory::shared();
    if (!store.ready())
        return std::unexpected(ToolError::io(
            "memo store not bound to a workspace"));
    std::size_t removed = store.forget(a.target);
    std::ostringstream out;
    if (removed == 0) {
        out << "no memo matched '" << a.target
            << "' — call `memos()` to see what's stored";
    } else {
        out << "\xe2\x9c\x97 forgot " << removed << " memo"
            << (removed == 1 ? "" : "s") << " matching '"
            << a.target << "'  ·  " << store.size() << " remaining";
    }
    std::string body = out.str();
    if (!a.display_description.empty())
        body = a.display_description + "\n\n" + body;
    return ToolOutput{std::move(body), std::nullopt};
}

ToolDef tool_forget_impl() {
    ToolDef t;
    constexpr const auto& kSpec = spec::require<"forget">();
    t.name = ToolName{std::string{kSpec.name}};
    t.description =
        "Remove a memo from the workspace memory. Use when a memo is "
        "stale, wrong, or contradicts something you've just learned. "
        "Pass either the memo's id (from `memos()`) or a substring of "
        "its topic.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"target"}},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"target", {{"type","string"},
                {"description","Memo id or topic substring (case-insensitive)."}}},
        }},
    };
    t.effects = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<ForgetArgs>(parse_forget_args, run_forget);
    return t;
}

// ── memos ──────────────────────────────────────────────────────────
struct MemosArgs {
    std::string filter;     // substring filter on topic
    std::string display_description;
};

[[nodiscard]] std::expected<MemosArgs, ToolError>
parse_memos_args(const json& j) {
    util::ArgReader ar(j);
    return MemosArgs{
        ar.str("filter", ""),
        ar.str("display_description", ""),
    };
}

ExecResult run_memos(const MemosArgs& a) {
    auto& store = memory::shared();
    if (!store.ready())
        return std::unexpected(ToolError::io(
            "memo store not bound to a workspace"));
    auto all = store.list_memos();
    if (all.empty())
        return ToolOutput{
            "No memos yet for " + store.workspace().generic_string()
            + ". The first `investigate` (or explicit `remember`) "
            "will start populating workspace memory.",
            std::nullopt};

    std::string lower_filter = a.filter;
    std::ranges::transform(lower_filter, lower_filter.begin(),
        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });

    // Sort by recency, descending.
    std::ranges::sort(all, [](const memory::Memo& x, const memory::Memo& y) {
        return x.created_at > y.created_at;
    });

    std::ostringstream out;
    auto now = std::chrono::system_clock::now();
    out << "Workspace memory: " << store.workspace().generic_string() << "\n";
    int shown = 0;
    for (const auto& m : all) {
        if (!lower_filter.empty()) {
            std::string lq = m.query;
            std::ranges::transform(lq, lq.begin(),
                [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
            if (lq.find(lower_filter) == std::string::npos) continue;
        }
        ++shown;
        auto age_s = std::chrono::duration_cast<std::chrono::seconds>(
            now - m.created_at).count();
        std::string age;
        if      (age_s < 60)         age = std::to_string(age_s) + "s";
        else if (age_s < 3600)       age = std::to_string(age_s / 60) + "m";
        else if (age_s < 86400)      age = std::to_string(age_s / 3600) + "h";
        else                         age = std::to_string(age_s / 86400) + "d";
        bool fresh = store.is_fresh(m);
        out << "\n" << (fresh ? "\xe2\x9c\x93" : "\xe2\x9a\xa0") << " "
            << m.id << "  ·  " << age << " ago  ·  "
            << m.synthesis.size() << " chars";
        if (!m.file_refs.empty())
            out << "  ·  " << m.file_refs.size() << " file ref"
                << (m.file_refs.size() == 1 ? "" : "s");
        if (!fresh) out << "  ·  STALE (files changed)";
        out << "\n   Q: " << m.query << "\n";
        if (!m.file_refs.empty()) {
            out << "   files: ";
            std::size_t budget = 80;
            std::size_t i = 0;
            for (const auto& f : m.file_refs) {
                if (i > 0) out << ", ";
                if (f.size() + 2 > budget) {
                    out << "(+" << (m.file_refs.size() - i) << " more)";
                    break;
                }
                out << f;
                budget -= f.size() + 2;
                ++i;
            }
            out << "\n";
        }
    }
    if (shown == 0)
        out << "\n(no memos match filter '" << a.filter << "')";
    else
        out << "\n" << shown << "/" << all.size() << " shown.  "
            << "Call `forget(id)` to drop one.";
    std::string body = out.str();
    if (!a.display_description.empty())
        body = a.display_description + "\n\n" + body;
    return ToolOutput{std::move(body), std::nullopt};
}

ToolDef tool_memos_impl() {
    ToolDef t;
    constexpr const auto& kSpec = spec::require<"memos">();
    t.name = ToolName{std::string{kSpec.name}};
    t.description =
        "List the workspace memos — the persistent knowledge accumulated "
        "from past `investigate` runs and `remember` calls. Use BEFORE "
        "starting a fresh investigation: if a memo already covers the "
        "question, prefer answering from it (or from the "
        "<learned-about-this-workspace> system-prompt block, which "
        "carries the same content). Each memo shows its id, age, byte "
        "size, and file refs. Stale memos (files changed since the "
        "memo was written) get a ⚠ marker.";
    t.input_schema = json{
        {"type","object"},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"filter", {{"type","string"},
                {"description","Optional substring filter on memo topics (case-insensitive)."}}},
        }},
    };
    t.effects = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<MemosArgs>(parse_memos_args, run_memos);
    return t;
}

} // namespace

ToolDef tool_remember() { return tool_remember_impl(); }
ToolDef tool_forget()   { return tool_forget_impl(); }
ToolDef tool_memos()    { return tool_memos_impl(); }

} // namespace moha::tools
