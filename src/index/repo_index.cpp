#include "moha/index/repo_index.hpp"

#include "moha/memory/file_card_store.hpp"
#include "moha/memory/memo_store.hpp"
#include "moha/tool/util/fs_helpers.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace moha::index {

namespace fs = std::filesystem;

std::string_view to_string(SymbolKind k) noexcept {
    switch (k) {
        case SymbolKind::Function:  return "fn";
        case SymbolKind::Method:    return "method";
        case SymbolKind::Class:     return "class";
        case SymbolKind::Struct:    return "struct";
        case SymbolKind::Enum:      return "enum";
        case SymbolKind::Union:     return "union";
        case SymbolKind::Namespace: return "namespace";
        case SymbolKind::Typedef:   return "type";
        case SymbolKind::Trait:     return "trait";
        case SymbolKind::Interface: return "interface";
        case SymbolKind::Module:    return "mod";
        case SymbolKind::Const:     return "const";
        case SymbolKind::Macro:     return "macro";
        case SymbolKind::Impl:      return "impl";
    }
    return "?";
}

// ── Per-language extractors ────────────────────────────────────────────
//
// These are intentionally regex-based, not tree-sitter-based. Tree-sitter
// would catch every weird corner case (function-style macros, requires-
// clauses, lambdas-as-defaults), but it's a heavy dependency to add for
// what is fundamentally a 90%-good model context aid. Regex misses the
// 10% that the model can still recover by reading the file directly —
// the index is a *hint*, not a contract.

namespace {

// Trim leading/trailing whitespace and cap a signature line so the index
// stays compact. 200 chars is enough for the full template-spaghetti
// signature without bloating the per-file budget.
std::string trim_sig(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
        s.remove_suffix(1);
    if (s.size() > 200) {
        std::string out{s.substr(0, 197)};
        out += "...";
        return out;
    }
    return std::string{s};
}

bool is_ident_char(char c) noexcept {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

void emit(std::vector<Symbol>& out, std::string name, SymbolKind k,
          int line, std::string_view full_line) {
    if (name.empty()) return;
    // Skip ALL_CAPS macro-style names from "function" pattern matches —
    // they're noise (header guards, etc.) and easy to misclassify.
    if (k == SymbolKind::Function && name.size() > 3) {
        bool all_caps = true;
        for (char c : name)
            if (!(std::isupper(static_cast<unsigned char>(c)) || c == '_'
                  || std::isdigit(static_cast<unsigned char>(c)))) {
                all_caps = false; break;
            }
        if (all_caps) return;
    }
    out.push_back({std::move(name), k, line, trim_sig(full_line)});
}

// ── C / C++ ────────────────────────────────────────────────────────────
//
// We scan line-by-line. This misses multi-line declarations (templates
// split across lines, macro-wrapped class names) but catches the vast
// majority of named declarations and never mis-fires on bodies of
// functions because every pattern starts at column-0-ish and requires
// the keyword.
void extract_cpp(std::string_view content, std::vector<Symbol>& out) {
    static const std::regex re_kw_named(
        R"(^\s*(?:template\s*<[^>]*>\s*)?(?:inline\s+|static\s+|constexpr\s+|consteval\s+|extern\s+|export\s+)*(class|struct|union|enum(?:\s+class)?|namespace)\s+(\w+))");
    static const std::regex re_typedef(
        R"(^\s*typedef\b.+\b(\w+)\s*;)");
    static const std::regex re_using(
        R"(^\s*using\s+(\w+)\s*=)");
    static const std::regex re_define(
        R"(^\s*#\s*define\s+(\w+))");
    // Function-like: optional template <...>, modifiers, return type,
    // identifier or qualified-id, '(' on the same line. The first non-
    // type token followed by '(' is the function name. Captures only the
    // last segment of qualified ids (Foo::bar -> bar).
    static const std::regex re_fn(
        R"(^\s*(?:template\s*<[^>]*>\s*)?(?:[\w:&*<>,\s~]+\s+|~)([A-Za-z_]\w*)\s*\([^;{]*[\){])");

    int line = 0;
    std::size_t cursor = 0;
    while (cursor < content.size()) {
        ++line;
        auto nl = content.find('\n', cursor);
        std::string_view ln = (nl == std::string_view::npos)
            ? content.substr(cursor)
            : content.substr(cursor, nl - cursor);
        std::string ln_str{ln};
        cursor = (nl == std::string_view::npos) ? content.size() : nl + 1;

        // Skip blank, comment-only, and preprocessor non-defines.
        if (ln_str.empty()) continue;
        std::string_view trimmed = ln_str;
        while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t'))
            trimmed.remove_prefix(1);
        if (trimmed.starts_with("//") || trimmed.starts_with("/*") || trimmed.starts_with("*"))
            continue;

        std::cmatch m;
        if (std::regex_search(ln_str.c_str(), m, re_kw_named)) {
            std::string kw = m[1].str();
            std::string name = m[2].str();
            SymbolKind k = SymbolKind::Class;
            if      (kw == "struct")    k = SymbolKind::Struct;
            else if (kw == "union")     k = SymbolKind::Union;
            else if (kw.starts_with("enum")) k = SymbolKind::Enum;
            else if (kw == "namespace") k = SymbolKind::Namespace;
            emit(out, std::move(name), k, line, ln);
            continue;
        }
        if (std::regex_search(ln_str.c_str(), m, re_using)) {
            emit(out, m[1].str(), SymbolKind::Typedef, line, ln);
            continue;
        }
        if (std::regex_search(ln_str.c_str(), m, re_typedef)) {
            emit(out, m[1].str(), SymbolKind::Typedef, line, ln);
            continue;
        }
        if (std::regex_search(ln_str.c_str(), m, re_define)) {
            // Leave signature empty — macros are noisy in the per-file map.
            emit(out, m[1].str(), SymbolKind::Macro, line, "");
            continue;
        }
        // Function pattern is last and most expensive; skip if line obviously
        // can't match (no '(' or no identifier-start char).
        if (ln.find('(') == std::string_view::npos) continue;
        if (std::regex_search(ln_str.c_str(), m, re_fn)) {
            std::string nm = m[1].str();
            // Filter common false-positives: control-flow keywords show as
            // identifiers under our pattern when they take an arg.
            static constexpr std::array<std::string_view, 8> kw{
                "if", "for", "while", "switch", "return", "sizeof",
                "alignof", "decltype",
            };
            if (std::ranges::find(kw, nm) != kw.end()) continue;
            emit(out, std::move(nm), SymbolKind::Function, line, ln);
        }
    }
}

// ── Python ─────────────────────────────────────────────────────────────
void extract_python(std::string_view content, std::vector<Symbol>& out) {
    static const std::regex re_def(R"(^\s*(async\s+)?def\s+(\w+)\s*\()");
    static const std::regex re_class(R"(^\s*class\s+(\w+)\b)");
    int line = 0;
    std::size_t cursor = 0;
    while (cursor < content.size()) {
        ++line;
        auto nl = content.find('\n', cursor);
        std::string_view ln = (nl == std::string_view::npos)
            ? content.substr(cursor) : content.substr(cursor, nl - cursor);
        std::string ln_str{ln};
        cursor = (nl == std::string_view::npos) ? content.size() : nl + 1;
        std::cmatch m;
        if (std::regex_search(ln_str.c_str(), m, re_def))
            emit(out, m[2].str(), SymbolKind::Function, line, ln);
        else if (std::regex_search(ln_str.c_str(), m, re_class))
            emit(out, m[1].str(), SymbolKind::Class, line, ln);
    }
}

// ── JavaScript / TypeScript ────────────────────────────────────────────
void extract_js(std::string_view content, std::vector<Symbol>& out) {
    static const std::regex re_fn(
        R"(^\s*(?:export\s+)?(?:async\s+)?function\s*\*?\s*(\w+)\s*\()");
    static const std::regex re_class(
        R"(^\s*(?:export\s+(?:default\s+)?)?(?:abstract\s+)?class\s+(\w+)\b)");
    static const std::regex re_iface(
        R"(^\s*(?:export\s+)?interface\s+(\w+)\b)");
    static const std::regex re_type(
        R"(^\s*(?:export\s+)?type\s+(\w+)\s*=)");
    static const std::regex re_arrow(
        R"(^\s*(?:export\s+)?(?:const|let|var)\s+(\w+)\s*[:=][^=]*=>)");
    static const std::regex re_const(
        R"(^\s*(?:export\s+)?const\s+(\w+)\s*[:=])");
    int line = 0;
    std::size_t cursor = 0;
    while (cursor < content.size()) {
        ++line;
        auto nl = content.find('\n', cursor);
        std::string_view ln = (nl == std::string_view::npos)
            ? content.substr(cursor) : content.substr(cursor, nl - cursor);
        std::string ln_str{ln};
        cursor = (nl == std::string_view::npos) ? content.size() : nl + 1;
        std::cmatch m;
        if      (std::regex_search(ln_str.c_str(), m, re_fn))    emit(out, m[1].str(), SymbolKind::Function,  line, ln);
        else if (std::regex_search(ln_str.c_str(), m, re_class)) emit(out, m[1].str(), SymbolKind::Class,     line, ln);
        else if (std::regex_search(ln_str.c_str(), m, re_iface)) emit(out, m[1].str(), SymbolKind::Interface, line, ln);
        else if (std::regex_search(ln_str.c_str(), m, re_type))  emit(out, m[1].str(), SymbolKind::Typedef,   line, ln);
        else if (std::regex_search(ln_str.c_str(), m, re_arrow)) emit(out, m[1].str(), SymbolKind::Function,  line, ln);
        else if (std::regex_search(ln_str.c_str(), m, re_const)) emit(out, m[1].str(), SymbolKind::Const,     line, ln);
    }
}

// ── Go ─────────────────────────────────────────────────────────────────
void extract_go(std::string_view content, std::vector<Symbol>& out) {
    static const std::regex re_fn(
        R"(^\s*func\s+(?:\([^)]*\)\s+)?(\w+)\s*\()");
    static const std::regex re_type(
        R"(^\s*type\s+(\w+)\s+(struct|interface|\w))");
    int line = 0;
    std::size_t cursor = 0;
    while (cursor < content.size()) {
        ++line;
        auto nl = content.find('\n', cursor);
        std::string_view ln = (nl == std::string_view::npos)
            ? content.substr(cursor) : content.substr(cursor, nl - cursor);
        std::string ln_str{ln};
        cursor = (nl == std::string_view::npos) ? content.size() : nl + 1;
        std::cmatch m;
        if (std::regex_search(ln_str.c_str(), m, re_fn))
            emit(out, m[1].str(), SymbolKind::Function, line, ln);
        else if (std::regex_search(ln_str.c_str(), m, re_type)) {
            std::string kw = m[2].str();
            SymbolKind k = SymbolKind::Typedef;
            if      (kw == "struct")    k = SymbolKind::Struct;
            else if (kw == "interface") k = SymbolKind::Interface;
            emit(out, m[1].str(), k, line, ln);
        }
    }
}

// ── Rust ───────────────────────────────────────────────────────────────
void extract_rust(std::string_view content, std::vector<Symbol>& out) {
    static const std::regex re_fn(
        R"(^\s*(?:pub(?:\([^)]*\))?\s+)?(?:async\s+|const\s+|unsafe\s+|extern\s+(?:"[^"]*"\s+)?)*fn\s+(\w+))");
    static const std::regex re_struct(
        R"(^\s*(?:pub(?:\([^)]*\))?\s+)?struct\s+(\w+))");
    static const std::regex re_enum(
        R"(^\s*(?:pub(?:\([^)]*\))?\s+)?enum\s+(\w+))");
    static const std::regex re_trait(
        R"(^\s*(?:pub(?:\([^)]*\))?\s+)?(?:unsafe\s+)?trait\s+(\w+))");
    static const std::regex re_mod(
        R"(^\s*(?:pub(?:\([^)]*\))?\s+)?mod\s+(\w+))");
    static const std::regex re_type(
        R"(^\s*(?:pub(?:\([^)]*\))?\s+)?type\s+(\w+)\s*=)");
    static const std::regex re_impl(
        R"(^\s*impl(?:\s*<[^>]*>)?\s+(?:[^{]+\s+for\s+)?(\w+))");
    static const std::regex re_const(
        R"(^\s*(?:pub(?:\([^)]*\))?\s+)?(?:const|static)\s+(\w+)\s*:)");
    int line = 0;
    std::size_t cursor = 0;
    while (cursor < content.size()) {
        ++line;
        auto nl = content.find('\n', cursor);
        std::string_view ln = (nl == std::string_view::npos)
            ? content.substr(cursor) : content.substr(cursor, nl - cursor);
        std::string ln_str{ln};
        cursor = (nl == std::string_view::npos) ? content.size() : nl + 1;
        std::cmatch m;
        if      (std::regex_search(ln_str.c_str(), m, re_fn))     emit(out, m[1].str(), SymbolKind::Function, line, ln);
        else if (std::regex_search(ln_str.c_str(), m, re_struct)) emit(out, m[1].str(), SymbolKind::Struct,   line, ln);
        else if (std::regex_search(ln_str.c_str(), m, re_enum))   emit(out, m[1].str(), SymbolKind::Enum,     line, ln);
        else if (std::regex_search(ln_str.c_str(), m, re_trait))  emit(out, m[1].str(), SymbolKind::Trait,    line, ln);
        else if (std::regex_search(ln_str.c_str(), m, re_mod))    emit(out, m[1].str(), SymbolKind::Module,   line, ln);
        else if (std::regex_search(ln_str.c_str(), m, re_type))   emit(out, m[1].str(), SymbolKind::Typedef,  line, ln);
        else if (std::regex_search(ln_str.c_str(), m, re_impl))   emit(out, m[1].str(), SymbolKind::Impl,     line, ln);
        else if (std::regex_search(ln_str.c_str(), m, re_const))  emit(out, m[1].str(), SymbolKind::Const,    line, ln);
    }
}

// Pick extractor by extension. Returns nullptr for unknown / non-code
// files. We deliberately don't try to sniff content for shebangs etc. —
// the cost/value isn't there.
using Extractor = void(*)(std::string_view, std::vector<Symbol>&);
[[nodiscard]] Extractor pick_extractor(const fs::path& p) {
    auto e = p.extension().string();
    std::ranges::transform(e, e.begin(),
        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    if (e == ".cpp" || e == ".cc" || e == ".cxx" || e == ".c++"
     || e == ".hpp" || e == ".hh" || e == ".hxx" || e == ".h++"
     || e == ".c"   || e == ".h"  || e == ".m"   || e == ".mm")
        return extract_cpp;
    if (e == ".py" || e == ".pyi") return extract_python;
    if (e == ".js" || e == ".jsx" || e == ".ts" || e == ".tsx"
     || e == ".mjs" || e == ".cjs")
        return extract_js;
    if (e == ".go") return extract_go;
    if (e == ".rs") return extract_rust;
    return nullptr;
}

constexpr std::size_t kMaxFileBytes = 2 * 1024 * 1024;  // skip giant generated files

// Names that show up everywhere in any C++/Rust/Python codebase but
// carry zero "I-define-this-thing" signal — counting cross-file
// mentions of these would just rank the standard library highest.
// Kept as a sorted constexpr-able set so the lookup is O(log n) (the
// std::ranges::binary_search call below). Add to it when a real
// codebase's map ranking gets distorted by a noisy name; removing
// items is fine if they turn out to be load-bearing in some repo.
constexpr std::array<std::string_view, 56> kNoiseSymbols = {
    "Self", "T", "U", "V", "args", "begin", "buf", "c", "cbegin",
    "cend", "char", "const_iterator", "data", "destroy", "do", "e",
    "empty", "end", "false", "find", "from", "get", "i", "id",
    "init", "input", "is", "it", "iter", "iterator", "j", "k", "kind",
    "len", "list", "main", "n", "name", "next", "node", "of", "p",
    "pair", "ptr", "push_back", "result", "self", "set", "size",
    "string", "this", "to", "true", "tuple", "value", "vector",
};

[[nodiscard]] bool is_noise_symbol(std::string_view name) noexcept {
    return std::ranges::binary_search(kNoiseSymbols, name);
}

// Leading-underscore / double-underscore identifiers are private by
// convention in Python and detail-level in C/C++. We hide them from
// the compact map so the table-of-contents reads as the *public* API
// surface; they're still in the per-file outline if a tool drills in.
[[nodiscard]] bool is_private_symbol(std::string_view name) noexcept {
    return !name.empty() && name.front() == '_';
}

// Extract a one-line description from the top of a file. Walks the
// first ~12 lines, joins consecutive comment lines into a single
// paragraph, strips comment markers, and caps at ~120 chars. Returns
// empty if the leading block is just a license boilerplate (long, no
// useful prose) or a header guard with no comment at all.
std::string extract_description(std::string_view content) {
    std::string out;
    std::size_t cursor = 0;
    int line_no = 0;
    bool in_block = false;
    while (cursor < content.size() && line_no < 16) {
        auto nl = content.find('\n', cursor);
        std::string_view ln = (nl == std::string_view::npos)
            ? content.substr(cursor) : content.substr(cursor, nl - cursor);
        cursor = (nl == std::string_view::npos) ? content.size() : nl + 1;
        ++line_no;

        // Trim leading whitespace.
        std::string_view trimmed = ln;
        while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t'))
            trimmed.remove_prefix(1);
        if (trimmed.empty()) {
            if (!out.empty()) break;        // blank after some prose ends the block
            continue;                       // blank before any prose: keep scanning
        }

        std::string piece;
        if (in_block) {
            // Inside a /* ... */ block: strip leading "* " and watch for end.
            auto end = trimmed.find("*/");
            if (end != std::string_view::npos) {
                piece = std::string{trimmed.substr(0, end)};
                in_block = false;
            } else {
                piece = std::string{trimmed};
            }
            // Strip the conventional " * " leader.
            if (piece.starts_with("* ")) piece.erase(0, 2);
            else if (piece.starts_with("*")) piece.erase(0, 1);
        } else if (trimmed.starts_with("//!") || trimmed.starts_with("///")) {
            piece = std::string{trimmed.substr(3)};
        } else if (trimmed.starts_with("//")) {
            piece = std::string{trimmed.substr(2)};
        } else if (trimmed.starts_with("#!")) {
            // Skip shebang.
            continue;
        } else if (trimmed.starts_with("# ")) {
            piece = std::string{trimmed.substr(2)};
        } else if (trimmed.starts_with("/*")) {
            in_block = true;
            piece = std::string{trimmed.substr(2)};
            if (auto end = piece.find("*/"); end != std::string::npos) {
                piece.erase(end);
                in_block = false;
            }
        } else {
            // Hit code without finding any prose — abandon.
            break;
        }
        // Trim leading space we may have left on `piece`.
        std::size_t lead = 0;
        while (lead < piece.size() && (piece[lead] == ' ' || piece[lead] == '\t')) ++lead;
        piece.erase(0, lead);
        if (piece.empty()) continue;
        if (!out.empty()) out += ' ';
        out += piece;
        if (out.size() > 200) break;
    }
    // Strip license-boilerplate noise (Copyright / SPDX / Licensed).
    auto lower = [](std::string s) {
        std::ranges::transform(s, s.begin(),
            [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        return s;
    };
    if (out.empty()) return out;
    auto lo = lower(out.substr(0, 60));
    if (lo.find("copyright") != std::string::npos
     || lo.find("spdx-license") != std::string::npos
     || lo.find("licensed under") != std::string::npos
     || lo.find("all rights reserved") != std::string::npos)
        return {};
    if (out.size() > 120) { out.resize(117); out += "..."; }
    return out;
}

[[nodiscard]] inline bool is_ident_start(char c) noexcept {
    unsigned char u = static_cast<unsigned char>(c);
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || u == '_';
}
[[nodiscard]] inline bool is_ident_cont(char c) noexcept {
    unsigned char u = static_cast<unsigned char>(c);
    return is_ident_start(c) || (u >= '0' && u <= '9');
}

} // namespace

std::vector<Symbol> extract_symbols(const fs::path& path) {
    auto ex = pick_extractor(path);
    if (!ex) return {};
    std::error_code ec;
    auto sz = fs::file_size(path, ec);
    if (ec || sz == 0 || sz > kMaxFileBytes) return {};
    auto content = tools::util::read_file(path);
    if (content.empty()) return {};
    std::vector<Symbol> out;
    ex(content, out);
    return out;
}

// ── RepoIndex ──────────────────────────────────────────────────────────

void RepoIndex::refresh(const fs::path& root) {
    std::lock_guard lk(mu_);
    root_ = root;
    std::error_code ec;
    std::unordered_map<std::string, FileIndex> next;
    for (auto it = fs::recursive_directory_iterator(root,
                fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        const auto& entry = *it;
        auto fn = entry.path().filename().string();
        if (entry.is_directory(ec)) {
            if (tools::util::should_skip_dir(fn)) it.disable_recursion_pending();
            continue;
        }
        if (!entry.is_regular_file(ec)) continue;
        if (fn.starts_with(".")) continue;
        auto extractor = pick_extractor(entry.path());
        if (!extractor) continue;
        std::error_code mec;
        auto mtime = fs::last_write_time(entry.path(), mec);
        if (mec) continue;
        std::string key = entry.path().string();
        if (auto cached = by_path_.find(key);
            cached != by_path_.end() && cached->second.mtime == mtime) {
            // Score gets recomputed below; keep cached symbols/description.
            cached->second.score = 0;
            next.emplace(std::move(key), std::move(cached->second));
            continue;
        }
        // Single read per file, used for both the symbol scan and the
        // description harvest. Two passes over the same byte buffer
        // beat two file opens by a wide margin on a cold cache.
        std::error_code sec;
        auto sz = fs::file_size(entry.path(), sec);
        if (sec || sz == 0 || sz > kMaxFileBytes) continue;
        auto content = tools::util::read_file(entry.path());
        if (content.empty()) continue;
        FileIndex fi;
        fi.path  = entry.path();
        fi.mtime = mtime;
        extractor(content, fi.symbols);
        fi.description = extract_description(content);
        next.emplace(std::move(key), std::move(fi));
    }
    by_path_ = std::move(next);
    rebuild_importance_();
    last_refresh_ = std::chrono::steady_clock::now();
}

void RepoIndex::rebuild_importance_() {
    // ── Pass 1: build the universe of defined symbol names ─────────────
    // Each name maps to the path that defines it (first wins on
    // collision — close enough for centrality counting; full duplicate
    // resolution is `find_definition`'s job, not the map's).
    std::unordered_set<std::string> universe;
    std::unordered_map<std::string, std::string> defined_in;
    universe.reserve(by_path_.size() * 16);
    defined_in.reserve(by_path_.size() * 16);
    for (const auto& [path, fi] : by_path_) {
        for (const auto& s : fi.symbols) {
            // Drop noise / private / very-short names from the
            // *importance graph* — they'd dominate the rankings without
            // adding signal. They still survive in the per-file
            // outline; they just don't pull weight in the centrality.
            if (s.name.size() < 3) continue;
            if (is_noise_symbol(s.name)) continue;
            if (is_private_symbol(s.name)) continue;
            universe.insert(s.name);
            defined_in.try_emplace(s.name, path);
        }
    }

    // ── Pass 2: tokenise each file's content, count cross-file mentions ─
    // O(total bytes) tokenisation × O(1) hash lookup per identifier.
    // For a 5k-file repo at ~5 KB avg = 25 MB of bytes, this finishes
    // in ~80 ms on a modern CPU and dominates the second-or-so refresh.
    symbol_score_.clear();
    symbol_score_.reserve(universe.size());
    for (const auto& [path, fi] : by_path_) {
        // Re-read content. We could cache it from refresh() but the
        // cost is one mmap-backed read per file and the saved working
        // set is significant (typical repo content can be > 50 MB
        // total, dwarfing the index itself). Trade RAM for one extra
        // sequential file scan.
        auto content = tools::util::read_file(fi.path);
        if (content.empty()) continue;

        // Track which identifiers already counted *in this file* so a
        // symbol mentioned 50× in one file only contributes 1 to its
        // inbound count — keeps a documentation-heavy file from
        // pumping up `Foo` just because the user wrote `Foo` 50 times
        // in comments.
        std::unordered_set<std::string_view> seen_here;
        std::size_t i = 0, n = content.size();
        while (i < n) {
            // Skip non-identifier-start bytes — including digits, which
            // can never *start* an identifier. The is_ident_cont check
            // below admits digits inside identifiers correctly.
            while (i < n && !is_ident_start(content[i])) ++i;
            std::size_t start = i;
            while (i < n && is_ident_cont(content[i])) ++i;
            if (i == start) continue;
            std::string_view tok{content.data() + start, i - start};
            if (tok.size() < 3) continue;        // skip 1-2 char locals
            if (universe.find(std::string{tok}) == universe.end()) continue;
            if (auto def = defined_in.find(std::string{tok});
                def != defined_in.end() && def->second == path) continue;  // self-mention
            if (!seen_here.insert(tok).second) continue;                    // dup in this file
            ++symbol_score_[std::string{tok}];
        }
    }

    // ── Pass 3: roll symbol scores up to per-file scores ───────────────
    // Plus a recency boost: files modified within the last hour get a
    // 2× multiplier so the agent's map naturally surfaces what the
    // user is actively working on. The boost decays linearly over a
    // 24-hour window so day-old work still ranks above untouched code.
    using clock = std::chrono::system_clock;
    auto now_sys = clock::now();
    auto sys_ftime = [&](fs::file_time_type t) -> clock::time_point {
        // Coarse conversion good enough for an age comparison; avoids
        // C++20 file_clock::to_sys cross-platform pitfalls.
        return clock::time_point{
            std::chrono::duration_cast<clock::duration>(t.time_since_epoch())
        };
    };
    for (auto& [path, fi] : by_path_) {
        int s = 0;
        for (const auto& sym : fi.symbols) {
            if (auto it = symbol_score_.find(sym.name); it != symbol_score_.end())
                s += it->second;
        }
        // Recency boost.
        auto age = now_sys - sys_ftime(fi.mtime);
        auto age_h = std::chrono::duration_cast<std::chrono::hours>(age).count();
        if (age_h <= 1)       s = s * 2 + 5;       // hot edit
        else if (age_h <= 24) s = s + std::max(1, s / 4);
        fi.score = s;
    }
}

const std::unordered_map<std::string, int>&
RepoIndex::symbol_scores() const noexcept {
    return symbol_score_;
}

std::vector<Symbol> RepoIndex::outline(const fs::path& path) {
    std::error_code ec;
    auto mtime = fs::last_write_time(path, ec);
    if (ec) return {};
    {
        std::lock_guard lk(mu_);
        if (auto it = by_path_.find(path.string());
            it != by_path_.end() && it->second.mtime == mtime)
            return it->second.symbols;
    }
    auto sym = extract_symbols(path);
    {
        std::lock_guard lk(mu_);
        FileIndex fi{path, mtime, sym};
        by_path_[path.string()] = std::move(fi);
    }
    return sym;
}

std::vector<FileIndex> RepoIndex::all_files() const {
    std::lock_guard lk(mu_);
    std::vector<FileIndex> out;
    out.reserve(by_path_.size());
    for (const auto& [_, fi] : by_path_) out.push_back(fi);
    std::ranges::sort(out, [](const FileIndex& a, const FileIndex& b) {
        return a.path < b.path;
    });
    return out;
}

bool RepoIndex::ready() const noexcept {
    std::lock_guard lk(mu_);
    return !by_path_.empty();
}

fs::path RepoIndex::workspace() const {
    std::lock_guard lk(mu_);
    return root_;
}

std::string RepoIndex::compact_map(std::size_t max_bytes) const {
    auto files = all_files();
    if (files.empty()) return "";
    fs::path root;
    {
        std::lock_guard lk(mu_);
        root = root_;
    }

    // ── Local helpers ─────────────────────────────────────────────────
    auto rel_of = [&](const fs::path& p) {
        return fs::relative(p, root).generic_string();
    };
    // Pick a per-symbol display string. Functions get a (parens) suffix
    // so the model knows they're callable; classes/structs/enums get a
    // single-char tag prefix; raw names for everything else.
    auto symbol_token = [](const Symbol& s) -> std::string {
        switch (s.kind) {
            case SymbolKind::Function:
            case SymbolKind::Method:    return s.name + "()";
            case SymbolKind::Class:     return "class " + s.name;
            case SymbolKind::Struct:    return "struct " + s.name;
            case SymbolKind::Enum:      return "enum " + s.name;
            case SymbolKind::Trait:     return "trait " + s.name;
            case SymbolKind::Interface: return "interface " + s.name;
            case SymbolKind::Namespace: return "ns " + s.name;
            case SymbolKind::Typedef:   return s.name;
            case SymbolKind::Module:    return "mod " + s.name;
            case SymbolKind::Const:     return s.name;
            case SymbolKind::Macro:     return s.name;
            case SymbolKind::Impl:      return "impl " + s.name;
            case SymbolKind::Union:     return "union " + s.name;
        }
        return s.name;
    };
    // Layer-3 cross-index: which files have past investigations
    // discussed? Computed once for this map render; the lookup is
    // O(1) per file. Each file's row gets a "(seen-in: <topic>)"
    // badge when present so the model knows there's prior knowledge.
    auto file_to_memos = memory::shared().file_to_memo_ids();

    // Render a file row with up to `budget` bytes of inline symbol list.
    // Returns (rendered_line, bytes_consumed). Skips noise + private
    // symbols here too so the compact view reads as the public surface.
    auto render_file = [&](const FileIndex& fi, std::size_t budget,
                           bool include_description) -> std::string {
        std::string row = "  " + fi.path.filename().string();
        std::string syms;
        std::size_t emitted = 0;
        std::size_t over    = 0;
        for (const auto& s : fi.symbols) {
            if (s.kind == SymbolKind::Macro) continue;
            if (is_noise_symbol(s.name)) continue;
            if (is_private_symbol(s.name)) continue;
            std::string tok = symbol_token(s);
            if (!syms.empty()) syms += ", ";
            if (syms.size() + tok.size() > budget) { over++; continue; }
            syms += std::move(tok);
            ++emitted;
        }
        if (!syms.empty()) row += "  [" + syms;
        if (over > 0) row += ", +" + std::to_string(over) + " more";
        if (!syms.empty()) row += "]";
        // Layer 3a: surface the LLM-distilled file card if we have one.
        // This is the "model knows what each file does" payoff — one
        // paragraph per file, cached on disk, regenerated on mtime
        // change. Falls back to the regex-extracted file description
        // (the leading prose comment) when no card has been generated
        // yet for this file.
        if (auto card = memory::shared_cards().get(fi.path);
            card && !card->summary.empty()) {
            row += "\n        // " + card->summary;
        } else if (include_description && !fi.description.empty()) {
            row += "\n        // " + fi.description;
        }
        // Memo cross-index annotation. We try a couple of relative-path
        // forms because the memo store stores workspace-relative paths
        // and the file index uses absolute paths under `root`.
        if (!file_to_memos.empty()) {
            std::error_code ec;
            std::string rel = std::filesystem::relative(fi.path, root, ec)
                              .generic_string();
            auto it = file_to_memos.find(rel);
            if (it != file_to_memos.end() && !it->second.empty()) {
                std::string topics;
                std::size_t n = 0;
                for (const auto& mid : it->second) {
                    if (auto m = memory::shared().by_id(mid)) {
                        if (!topics.empty()) topics += "; ";
                        std::string q = m->query;
                        if (q.size() > 50) q = q.substr(0, 47) + "...";
                        topics += "\"" + q + "\"";
                        if (++n >= 2) break;
                    }
                }
                if (!topics.empty())
                    row += "  (memo: " + topics + ")";
            }
        }
        row += "\n";
        return row;
    };

    // ── Section 1: top-by-importance with per-file detail ────────────
    // Take the highest-scoring N files (by sum of inbound mentions to
    // their defined symbols, recency-boosted). They get a generous
    // per-file byte budget so signatures + descriptions are visible.
    // This is the model's "API cheatsheet": when it asks "what's the
    // shape of this codebase?", the answer leads with what *matters*.
    constexpr std::size_t kTopN              = 18;
    constexpr std::size_t kTopBudgetPerFile  = 360;
    constexpr std::size_t kTreeBudgetPerFile = 220;

    // Sort a copy by score desc; on tie, by path so output is stable.
    auto by_score = files;
    std::ranges::sort(by_score,
        [](const FileIndex& a, const FileIndex& b) {
            if (a.score != b.score) return a.score > b.score;
            return a.path < b.path;
        });

    std::ostringstream out;
    std::size_t budget = max_bytes;
    auto try_write = [&](std::string s) -> bool {
        if (s.size() > budget) return false;
        out << s;
        budget -= s.size();
        return true;
    };

    out << "## Most-referenced files\n";
    out << "## (ranked by cross-file mentions of their symbols + "
        << "recency boost)\n";
    budget -= std::min(budget, std::size_t{120});
    std::size_t shown_top = 0;
    for (const auto& fi : by_score) {
        if (shown_top >= kTopN) break;
        if (fi.score <= 0) break;             // nothing referenced; skip to tree
        std::string header = "[" + std::to_string(fi.score) + "] "
                           + rel_of(fi.path) + "\n";
        if (header.size() + 32 > budget) break;
        out << header;
        budget -= header.size();
        std::string detail = render_file(fi, kTopBudgetPerFile,
                                         /*include_description=*/true);
        // Strip the "  " filename prefix that render_file added — in
        // this section the path is already on its own header line, so
        // the symbol row gets a different indent.
        if (detail.starts_with("  " + fi.path.filename().string())) {
            auto after_name = 2 + fi.path.filename().string().size();
            std::string rest = detail.substr(after_name);
            if (!try_write("    " + rest)) break;
        } else {
            if (!try_write(std::move(detail))) break;
        }
        ++shown_top;
    }
    if (shown_top == 0) {
        out << "  (no inbound references yet — run a few tools first)\n";
    }

    // ── Section 2: directory tree of every file (abbreviated) ────────
    out << "\n## Workspace tree\n";
    budget -= std::min(budget, std::size_t{20});

    // Re-sort the original `files` by path (already sorted by all_files,
    // but be explicit so future caller changes don't trip us up).
    std::ranges::sort(files,
        [](const FileIndex& a, const FileIndex& b) { return a.path < b.path; });

    fs::path current_dir;
    bool first_dir = true;
    std::size_t omitted = 0;
    for (const auto& fi : files) {
        // Allow at least 80 bytes so the truncation footer fits.
        if (budget < 100) { ++omitted; continue; }
        std::string rel = rel_of(fi.path);
        fs::path parent = fs::path{rel}.parent_path();
        if (parent != current_dir) {
            current_dir = parent;
            std::string hdr = (first_dir ? "" : "\n")
                            + (parent.empty() ? std::string{"."}
                                              : parent.generic_string())
                            + "/\n";
            if (!try_write(std::move(hdr))) { ++omitted; continue; }
            first_dir = false;
        }
        // Per-file budget: smaller in the tree. Long files just show a
        // truncated symbol list — the user should query `outline(path)`
        // for full detail.
        std::string row = render_file(fi, kTreeBudgetPerFile,
                                      /*include_description=*/false);
        if (!try_write(std::move(row))) { ++omitted; continue; }
    }
    if (omitted > 0) {
        out << "[... " << omitted << " files omitted from tree to fit "
            << "budget; call `outline(path)` for any specific file or "
            << "`signatures(name)` to grep symbols]\n";
    }

    return out.str();
}

std::vector<std::pair<fs::path, Symbol>>
RepoIndex::find_symbols(std::string_view pattern, std::size_t limit) const {
    std::string needle{pattern};
    std::ranges::transform(needle, needle.begin(),
        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    auto files = all_files();
    std::vector<std::pair<fs::path, Symbol>> out;
    for (const auto& fi : files) {
        for (const auto& s : fi.symbols) {
            std::string lower = s.name;
            std::ranges::transform(lower, lower.begin(),
                [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
            if (lower.find(needle) != std::string::npos) {
                out.emplace_back(fi.path, s);
                if (out.size() >= limit) return out;
            }
        }
    }
    return out;
}

RepoIndex& shared() {
    static RepoIndex g;
    return g;
}

} // namespace moha::index
