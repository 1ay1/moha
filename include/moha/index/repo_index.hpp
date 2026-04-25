#pragma once
// moha::index — process-wide symbol index for the workspace.
//
// One walk of the tree on first query, regex-based per-language symbol
// extraction (no tree-sitter dependency), mtime-tracked refresh. The
// model uses this through three thin tools — `outline`, `repo_map`,
// `signatures` — and the system prompt embeds a compact map at session
// start so the agent has a table-of-contents without burning tool turns
// on `list_dir` tours of every directory.
//
// Coverage: C/C++, Python, JS/TS (incl. JSX/TSX), Go, Rust, Java, Ruby.
// Anything else is silently skipped — it falls back to `read`/`grep`
// which still work fine.
//
// The index is intentionally lossy: we capture the *names* and *line
// numbers* of declarations, not their bodies. That's the right
// compression: the model can ask "where is X?" or "what's in this
// file?" for a few hundred tokens instead of paging the file in.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace moha::index {

enum class SymbolKind : std::uint8_t {
    Function,    // free function (any language)
    Method,      // member function — emitted only when the parser can tell
    Class,       // class
    Struct,      // struct (C++/C/Go/Rust)
    Enum,        // enum / enum class
    Union,       // C/C++ union
    Namespace,   // C++ namespace, JS module, Python module
    Typedef,     // typedef / using / type alias
    Trait,       // Rust trait
    Interface,   // TS interface, Go interface, Java interface
    Module,      // Rust mod, Ruby module
    Const,       // top-level const/let
    Macro,       // C/C++ #define
    Impl,        // Rust impl block
};

[[nodiscard]] std::string_view to_string(SymbolKind k) noexcept;

struct Symbol {
    std::string name;
    SymbolKind  kind;
    int         line = 0;          // 1-based line where the declaration starts
    std::string signature;         // trimmed source line; empty for noise we
                                   // could parse but didn't (Macros, large
                                   // function bodies — avoid bloat)
};

struct FileIndex {
    std::filesystem::path                 path;       // workspace-relative
    std::filesystem::file_time_type       mtime{};
    std::vector<Symbol>                   symbols;
    // Top-of-file description: the first prose-shaped comment block,
    // capped at ~120 chars. Empty if the file has no leading comment
    // or the comment is just a license/header-guard incantation.
    std::string                           description;
    // Centrality score: sum of cross-file inbound mentions to the
    // symbols this file *defines*. Computed in `rebuild_importance_`
    // after every refresh. A score of 0 means "nothing else in the
    // workspace references anything this file defines" (typically a
    // top-level main, vendored code, or a leaf test file).
    int                                   score = 0;
};

class RepoIndex {
public:
    // Walk `root` (defaults to the workspace cwd), respecting the standard
    // skip-list (.git, node_modules, build, target, vendor, etc.). Re-uses
    // cached entries whose mtime is unchanged. Thread-safe.
    void refresh(const std::filesystem::path& root);

    // One file. If the file isn't in the cache (or its mtime moved), parse
    // it now. Returns empty if the path isn't a code file or doesn't exist.
    [[nodiscard]] std::vector<Symbol>
    outline(const std::filesystem::path& path);

    // All cached files. Deterministic (sorted by path).
    [[nodiscard]] std::vector<FileIndex> all_files() const;

    // Compact human/model-readable repo map, capped at `max_bytes`. Format:
    //   src/runtime/
    //     update.cpp [Step do_things, Msg classify, ...]
    //     view.cpp   [void render(...), ...]
    // Files are listed flat under each directory; symbols are joined with
    // commas and truncated with "…" once the per-file budget is hit.
    [[nodiscard]] std::string
    compact_map(std::size_t max_bytes = 4096) const;

    // Symbols matching `pattern` (case-insensitive substring). Returns up
    // to `limit` (path, symbol) pairs.
    [[nodiscard]] std::vector<std::pair<std::filesystem::path, Symbol>>
    find_symbols(std::string_view pattern, std::size_t limit = 50) const;

    // True iff at least one refresh has populated the cache.
    [[nodiscard]] bool ready() const noexcept;

    // Workspace root the cache was last built against. Empty if never.
    [[nodiscard]] std::filesystem::path workspace() const;

    // Per-symbol cross-file mention count — populated by
    // `rebuild_importance_`. Returned as a const reference so
    // tests / debug-dump code can introspect without a copy. Empty
    // before the first refresh.
    [[nodiscard]] const std::unordered_map<std::string, int>&
    symbol_scores() const noexcept;

private:
    mutable std::mutex                                            mu_;
    std::filesystem::path                                          root_;
    std::unordered_map<std::string, FileIndex>                     by_path_;  // key = path.string()
    std::unordered_map<std::string, int>                           symbol_score_;  // name → cross-file mentions
    std::chrono::steady_clock::time_point                          last_refresh_{};

    // Compute every symbol's cross-file mention count and per-file
    // score after a refresh. Runs under `mu_` already held.
    // Algorithm: build the universe of defined symbol names (set), then
    // tokenise each file's content once and bump the count for every
    // matching identifier that ISN'T defined in the same file (so a
    // file's local helpers don't pump up their own importance). File
    // score = sum of per-symbol scores for everything it defines, with
    // a 2× multiplier for files modified within the last hour so the
    // map reflects what the user is actively touching.
    void rebuild_importance_();
};

// Process-wide singleton — the runtime + tools + transport all share one
// view of the index. Never null; constructed on first call.
[[nodiscard]] RepoIndex& shared();

// One-shot extractor. Public so the `outline` tool can run it on a file
// that's outside the cached workspace (e.g. an absolute path the user
// pasted). Cheap — a single read + a few regex scans.
[[nodiscard]] std::vector<Symbol>
extract_symbols(const std::filesystem::path& path);

} // namespace moha::index
