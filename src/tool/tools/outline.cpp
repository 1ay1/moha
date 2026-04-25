#include "moha/index/repo_index.hpp"
#include "moha/tool/spec.hpp"
#include "moha/tool/tools.hpp"
#include "moha/tool/util/arg_reader.hpp"
#include "moha/tool/util/fs_helpers.hpp"
#include "moha/tool/util/tool_args.hpp"

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

namespace moha::tools {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct OutlineArgs {
    util::NormalizedPath path;
    std::string display_description;
};

std::expected<OutlineArgs, ToolError> parse_outline_args(const json& j) {
    util::ArgReader ar(j);
    auto p = ar.require_str("path");
    if (!p) return std::unexpected(ToolError::invalid_args("path required"));
    return OutlineArgs{
        util::NormalizedPath{std::move(*p)},
        ar.str("display_description", ""),
    };
}

ExecResult run_outline(const OutlineArgs& a) {
    const auto& p = a.path.path();
    std::error_code ec;
    if (!fs::exists(p, ec))
        return std::unexpected(ToolError::not_found("file not found: " + a.path.string()));
    if (!fs::is_regular_file(p, ec))
        return std::unexpected(ToolError::not_a_file("not a regular file: " + a.path.string()));

    auto symbols = index::shared().outline(p);
    if (symbols.empty()) {
        return ToolOutput{
            "No symbols extracted (file may be empty, binary, an "
            "unsupported language, or contain only declarations the "
            "regex extractor can't recognise — fall back to `read` for "
            "the raw contents).",
            std::nullopt};
    }

    std::ostringstream out;
    out << a.path.string() << "  (" << symbols.size() << " symbols)\n";
    // Group consecutive same-kind entries with a single header so the
    // output reads as a true outline rather than a flat dump.
    index::SymbolKind cur_kind{};
    bool first = true;
    for (const auto& s : symbols) {
        if (first || s.kind != cur_kind) {
            out << "\n[" << index::to_string(s.kind) << "]\n";
            cur_kind = s.kind;
            first = false;
        }
        out << "  L" << s.line << "  " << s.name;
        if (!s.signature.empty()) {
            // Only show the trimmed signature when it adds info beyond
            // the bare name (avoids "foo  foo" rows for plain function
            // declarations whose signature line is just `foo()`).
            std::string_view sig{s.signature};
            if (sig.find(s.name) != std::string_view::npos
                && sig.size() > s.name.size() + 2) {
                out << "    " << sig;
            }
        }
        out << "\n";
    }

    std::string body = out.str();
    if (!a.display_description.empty())
        body = a.display_description + "\n\n" + body;
    return ToolOutput{std::move(body), std::nullopt};
}

} // namespace

ToolDef tool_outline() {
    ToolDef t;
    constexpr const auto& kSpec = spec::require<"outline">();
    t.name = ToolName{std::string{kSpec.name}};
    t.description =
        "Return a compact outline of a single file: every function, "
        "class, struct, enum, namespace, etc. with its line number. "
        "Use this BEFORE `read` when you only need to know what's "
        "*in* a file — it's 10-50x cheaper than reading the body. "
        "Supports C/C++, Python, JS/TS/JSX/TSX, Go, Rust.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"path"}},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"path", {{"type","string"}, {"description","File path"}}},
        }},
    };
    t.effects = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<OutlineArgs>(parse_outline_args, run_outline);
    return t;
}

} // namespace moha::tools
