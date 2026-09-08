// agentty::tools::skills — implementation. See skills.hpp for the
// progressive-disclosure rationale and the discovery-root table.

#include "agentty/util/user_root.hpp"
#include "agentty/tool/skills.hpp"
#include "agentty/util/home_dir.hpp"

#include "agentty/scope/scope.hpp"
#include "agentty/tool/util/fs_helpers.hpp"
#include "agentty/util/dbglog.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <system_error>

namespace agentty::tools::skills {

namespace fs = std::filesystem;

namespace {

// Resolution counter behind debug_cap_resolutions() (see skills.hpp).
// Atomic because all() is mutex-guarded but the counter is read from a
// test thread without that lock.
std::atomic<std::size_t>& cap_resolutions() noexcept {
    static std::atomic<std::size_t> n{0};
    return n;
}

// ── Catalog cap: default + env override ────────────────────────
//
// kMaxSkills (header) is the default tier-1 catalog cap;
// AGENTTY_MAX_SKILLS overrides it — for small-context models (a
// 100-skill library would eat the system prompt) or libraries that
// legitimately exceed the default.
//
// Clamped to [8, 4096]: below 8 the catalog loses the trade that
// motivated the cap (the listing is worth its context), above 4096 it
// no longer protects the system prompt at all. A malformed value keeps
// the default (matches bridge.cpp's env parsing) with a dbglog
// breadcrumb.
//
// RESOLVED ONCE PER DISCOVERY PASS, then threaded through — not read
// per call. Two reasons, in order of weight:
//
//   1. The breadcrumb. This is consulted inside scan_root's walk, once
//      per DIRECTORY ENTRY. Re-reading there logged the malformed-value
//      message once per entry: measured 120 lines for a single pass over
//      40 skills. all() runs every turn and dbglog emits at ERROR level,
//      which also enters the crash-time flight recorder — so one typo'd
//      env var floods the recorder with a repeated message and displaces
//      the diagnostics a crash dump exists to preserve.
//
//   2. Stability. A pass that re-read the env could observe two
//      different caps for its own scan bound and its own catalog slice
//      if the environment changed underneath it. One resolution per pass
//      makes that unrepresentable rather than merely unlikely.
//
// Still re-read per PASS (not latched in a function-local static): a
// subagent, a --workspace switch, or a test may legitimately run with a
// different value in the same process, and a process-lifetime latch
// would silently serve the first caller's cap to everyone after.
//
// BOTH bound sites take the resolved value — scan_root's candidate
// collection and the catalog slice in all() — so a raised cap lifts the
// scan-side WORK bound too; capping only the slice would truncate
// discovery of large libraries (the bug scan_root's comment documents).
[[nodiscard]] std::size_t resolve_catalog_cap() noexcept {
    cap_resolutions().fetch_add(1, std::memory_order_relaxed);
    if (const char* e = std::getenv("AGENTTY_MAX_SKILLS"); e && e[0]) {
        const char* end = e + std::strlen(e);
        std::size_t v   = 0;
        const auto [ptr, ec] = std::from_chars(e, end, v);
        if (ec == std::errc{} && ptr == end) {
            if (v < 8)    return std::size_t{8};
            if (v > 4096) return std::size_t{4096};
            return v;
        }
        agentty::util::dbglog("skills.catalog_cap.env",
                              "AGENTTY_MAX_SKILLS: not a positive integer — using default");
    }
    return kMaxSkills;
}

[[nodiscard]] fs::path home_dir() noexcept {
    return agentty::util::home_dir_or_empty();
}

// Trim ASCII whitespace from both ends.
[[nodiscard]] std::string trim(std::string s) {
    auto issp = [](char c) { return c==' '||c=='\t'||c=='\r'||c=='\n'; };
    while (!s.empty() && issp(s.front())) s.erase(s.begin());
    while (!s.empty() && issp(s.back()))  s.pop_back();
    return s;
}

// Read a file with a hard byte cap. Empty on missing / unreadable / oversize.
[[nodiscard]] std::string read_capped(const fs::path& p, std::size_t cap) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec) || ec) return {};
    auto sz = fs::file_size(p, ec);
    if (ec || sz == 0 || sz > cap) return {};
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::string out(static_cast<std::size_t>(sz), '\0');
    f.read(out.data(), static_cast<std::streamsize>(sz));
    out.resize(static_cast<std::size_t>(f.gcount()));
    return out;
}

// Parse `key: value` from a YAML frontmatter line. LENIENT by design
// (spec guidance): only the FIRST colon splits, so a description like
// "Use when: handling PDFs" — technically invalid YAML that other
// clients' parsers accept — parses fine here. Returns false if the
// line isn't a simple scalar mapping.
[[nodiscard]] bool parse_kv(const std::string& line,
                            std::string& key, std::string& val) {
    auto colon = line.find(':');
    if (colon == std::string::npos) return false;
    key = trim(line.substr(0, colon));
    val = trim(line.substr(colon + 1));
    // Strip matching surrounding quotes on the value.
    if (val.size() >= 2 &&
        ((val.front() == '"' && val.back() == '"') ||
         (val.front() == '\'' && val.back() == '\''))) {
        val = val.substr(1, val.size() - 2);
    }
    return !key.empty();
}

[[nodiscard]] bool is_truthy(const std::string& v) noexcept {
    return v == "true" || v == "1" || v == "yes";
}

// Leading-space count (frontmatter nesting depth detector).
[[nodiscard]] std::size_t indent_of(const std::string& line) noexcept {
    std::size_t i = 0;
    while (i < line.size() && line[i] == ' ') ++i;
    return i;
}

// Split a SKILL.md into frontmatter fields + body. Frontmatter is the
// block between the first two `---` lines. `slug` is the directory
// name, used as the name fallback (lenient: a name/dir mismatch loads
// anyway, with the frontmatter name winning).
//
// The mini-parser covers the YAML subset real skills use:
//   • scalar `key: value` (first-colon split — unquoted colons fine)
//   • block scalars `key: |` / `key: >` (folded/literal multi-line
//     descriptions — common in Claude Code-authored skills)
//   • one-level nested mapping under `metadata:`
[[nodiscard]] Skill parse_skill(const std::string& raw, const std::string& slug,
                                const std::string& source) {
    Skill s;
    s.name = slug;   // fallback; frontmatter `name:` may override below
    s.slug = slug;   // spec-derived identity, kept for lint
    s.source = source;

    std::istringstream in(raw);
    std::string line;
    std::streampos body_start = 0;
    bool fm_done = false;
    std::string first;
    if (std::getline(in, first) && trim(first) == "---") {
        bool in_metadata = false;          // inside the `metadata:` nested map
        std::string* block_target = nullptr;  // collecting a block scalar into
        bool block_fold = false;           // `>` folds newlines to spaces
        bool block_first = true;
        while (std::getline(in, line)) {
            if (trim(line) == "---") { fm_done = true; body_start = in.tellg(); break; }

            const std::size_t ind = indent_of(line);

            // Continuation lines of an active block scalar: any indented
            // non-empty line. A top-level (unindented) line ends it.
            if (block_target) {
                if (ind > 0 || trim(line).empty()) {
                    auto t = trim(line);
                    if (!t.empty()) {
                        if (!block_first) *block_target += block_fold ? " " : "\n";
                        *block_target += t;
                        block_first = false;
                    }
                    continue;
                }
                block_target = nullptr;   // fall through: parse this line
            }

            // Nested lines under `metadata:`.
            if (in_metadata && ind > 0) {
                std::string k, v;
                if (parse_kv(line, k, v)) s.metadata.emplace_back(k, v);
                continue;
            }
            in_metadata = false;

            std::string k, v;
            if (!parse_kv(line, k, v)) continue;
            if (k == "metadata" && v.empty()) { in_metadata = true; continue; }

            // Block-scalar opener: `description: |` / `description: >-` etc.
            std::string* field = nullptr;
            if      (k == "name")                      { if (!v.empty()) s.name = v; continue; }
            else if (k == "description")               field = &s.description;
            else if (k == "compatibility")             field = &s.compatibility;
            else if (k == "allowed-tools")             field = &s.allowed_tools;
            else if (k == "license")                   field = &s.license;
            else if (k == "disable-model-invocation")  { s.user_only = is_truthy(v); continue; }
            else continue;   // unknown keys: tolerated, unused

            if (v == "|" || v == "|-" || v == "|+" ||
                v == ">" || v == ">-" || v == ">+") {
                block_target = field;
                block_fold   = (v[0] == '>');
                block_first  = true;
                field->clear();
            } else {
                *field = v;
            }
        }
    }

    if (fm_done && body_start != std::streampos(-1)) {
        s.body = trim(raw.substr(static_cast<std::size_t>(body_start)));
    } else {
        // No frontmatter — treat the whole file as the body.
        s.body = trim(raw);
    }
    // Lenient fallback (spec: a description is essential for disclosure,
    // but we can synthesise one): first non-blank body line.
    if (s.description.empty()) {
        std::istringstream b(s.body);
        std::string l;
        while (std::getline(b, l)) {
            auto t = trim(l);
            if (!t.empty()) { s.description = t; break; }
        }
    }
    return s;
}

// Derive a skill's spec name from its directory path below the root.
//
// `embedded/startup` -> `embedded-startup`. The spec's charset is
// `[a-z0-9-]`, and this function is the ONE place that guarantees it — the
// name it returns is what `skills::find()` keys on and what the user types
// as `/name`, so a character that survives here is a skill that cannot be
// invoked. Replacing only '/' was not enough: a group folder named
// `My Group/Sub_Dir` produced the name "My Group-Sub_Dir", which is
// discoverable and listed but permanently unreachable, because the
// slash-command parser (update/submit.cpp) splits its token on whitespace.
//
// Rules, applied in order:
//   • ASCII upper -> lower (names are case-insensitive in practice; the
//     picker and `find()` compare exactly, so folding here is what makes
//     `Foo/` and `foo/` the same skill rather than two).
//   • [a-z0-9] kept; everything else (separators, spaces, punctuation,
//     any non-ASCII byte) becomes '-'.
//   • runs of '-' collapse, and leading/trailing '-' are trimmed, so
//     `a//b`, `a - b` and `-a-` do not yield doubled or edge hyphens.
//
// Returns empty when nothing survives (a directory named `___`), which the
// caller treats as "not a skill" rather than inventing a name.
[[nodiscard]] std::string slug_from_path(std::string_view rel) {
    std::string out;
    out.reserve(rel.size());
    for (unsigned char c : rel) {
        char mapped;
        if (c >= 'A' && c <= 'Z')                      mapped = static_cast<char>(c - 'A' + 'a');
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
                                                       mapped = static_cast<char>(c);
        else                                           mapped = '-';
        // Collapse runs rather than emitting doubled hyphens.
        if (mapped == '-' && (out.empty() || out.back() == '-')) continue;
        out.push_back(mapped);
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    if (out.size() > kMaxSlugLen) {
        out.resize(kMaxSlugLen);
        while (!out.empty() && out.back() == '-') out.pop_back();
    }
    return out;
}

// Enumerate bundled resource files under the skill dir (tier 3 of
// progressive disclosure). Recursive but bounded: depth ≤ 3, at most
// kMaxResources entries, skipping dotfiles and every SKILL.md (the
// skill's own AND a nested skill's — instructions are never resources).
// Paths
// recorded relative to the skill dir with forward slashes so the model
// can splice them after the absolute dir we hand it.
void enumerate_resources(const fs::path& dir, std::vector<std::string>& out) {
    std::error_code ec;
    auto opts = fs::directory_options::skip_permission_denied;
    fs::recursive_directory_iterator it(dir, opts, ec), end;
    for (; !ec && it != end && out.size() < kMaxResources; it.increment(ec)) {
        if (it.depth() > 2) { it.disable_recursion_pending(); continue; }
        const auto& p = it->path();
        auto fname = p.filename().string();
        if (!fname.empty() && fname.front() == '.') {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        // Every SKILL.md — the skill's own (depth 0) AND a nested
        // skill's (deeper) — is instructions, not a bundled resource.
        if (fname == "SKILL.md") continue;
        auto rel = fs::relative(p, dir, ec);
        if (ec) continue;
        std::string r = rel.generic_string();   // forward slashes everywhere
        out.push_back(std::move(r));
    }
    std::sort(out.begin(), out.end());
}

// Scan one root for skill entries — a `<dir>/SKILL.md` at ANY depth
// below the root — appending to `out` (skipping names already present —
// earlier roots win, see header precedence table). Nested group folders
// are transparent: the slug is the skill directory's path relative to
// the root with segments joined by `-` (`skills/embedded/startup/` →
// `embedded-startup`), so names stay inside the spec's `a-z0-9-` set.
// Entries are visited SHALLOWEST-FIRST (see the sort below) — on a
// joined-name collision the shallower entry wins, and root-level skills
// keep their flat names so existing libraries load unchanged.
// Appends each found SKILL.md's mtime into `sig` so an in-place edit
// (same dir mtime) still invalidates the cache.
void scan_root(const fs::path& root, const std::string& source,
               std::size_t cap, std::vector<Skill>& out, std::string& sig) {
    std::error_code ec;
    if (!fs::is_directory(root, ec) || ec) return;
    auto mt = fs::last_write_time(root, ec);
    // file_time_type's rep is __int128 on Android/bionic — no std::to_string
    // overload matches. long long keeps ~292 years of ns range; a cache-sig
    // wrap is harmless (it only needs to CHANGE when the mtime changes).
    if (!ec)
        sig += source + ":" +
               std::to_string(static_cast<long long>(
                   mt.time_since_epoch().count())) +
               ";";

    // Collect every SKILL.md below the root. Hidden directories are
    // never descended into (`.git`, `.obsidian`, ... are storage, not
    // skill territory), and directory symlinks — which the iterator
    // does not follow — are probed directly so a top-level symlinked
    // skill dir keeps working (it was discoverable in the flat scan).
    //
    // The walk is BOUNDED on both axes. Depth stops at kMaxSkillDepth
    // (group folders organize a library; they do not nest arbitrarily),
    // and the collection stops once enough candidates exist to fill
    // kMaxSkills. Without the second bound the cap applied to the RESULT
    // but not the WORK: a skills root that happened to contain a large
    // tree was traversed in full on every cache miss.
    std::vector<fs::path> mds;
    auto opts = fs::directory_options::skip_permission_denied;
    fs::recursive_directory_iterator it(root, opts, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        // depth() is 0 for entries directly in the root, so a skill dir at
        // depth d has its SKILL.md at d+1. Stop DESCENDING past the cap
        // rather than stopping the walk: siblings at legal depth still
        // need visiting.
        if (it.depth() >= static_cast<int>(kMaxSkillDepth))
            it.disable_recursion_pending();
        if (mds.size() >= cap) break;
        if (it->is_directory(ec)) {
            auto fname = it->path().filename().string();
            if (!fname.empty() && fname.front() == '.') {
                it.disable_recursion_pending();
                continue;
            }
            if (it->is_symlink(ec)) {
                fs::path md = it->path() / "SKILL.md";
                if (fs::is_regular_file(md, ec)) mds.push_back(std::move(md));
            }
            continue;
        }
        if (it->is_regular_file(ec) && it->path().filename() == "SKILL.md")
            mds.push_back(it->path());
    }
    // Visit SHALLOWEST-FIRST, then lexicographically within a depth.
    //
    // The shadow rule below is first-wins, so visit order IS precedence.
    // Plain lexicographic order does not encode depth: '-' (0x2D) sorts
    // before '/' (0x2F), so `a/b/SKILL.md` sorts BEFORE `a-b/SKILL.md`
    // even though the latter is shallower. Both slug to `a-b`, so an
    // existing flat `a-b/` skill was silently displaced the moment an
    // unrelated `a/b/` group folder appeared next to it — a regression for
    // libraries that predate nesting, which is exactly what "flat layouts
    // keep their names unchanged" is supposed to guarantee.
    //
    // Comparing (depth, path) states the intent directly instead of
    // relying on a byte-order coincidence. Depth is measured lexically
    // for the same reason the slug is (see below): a symlinked skill dir
    // belongs at the depth where it was PUT, not where it resolves to.
    auto depth_of = [&](const fs::path& md) {
        const auto rel = md.parent_path().lexically_relative(root);
        return static_cast<std::size_t>(
            std::distance(rel.begin(), rel.end()));
    };
    std::ranges::sort(mds, [&](const fs::path& a, const fs::path& b) {
        const auto da = depth_of(a), db = depth_of(b);
        if (da != db) return da < db;
        return a < b;
    });
    mds.erase(std::unique(mds.begin(), mds.end()), mds.end());

    for (const auto& md : mds) {
        if (out.size() >= cap) break;
        // Slug = path below the root, sanitized to the spec's charset. A
        // SKILL.md sitting directly in the root has no directory of its
        // own — same as the flat scan, it is not a skill (a skill is a
        // DIRECTORY holding SKILL.md). A path that sanitizes to nothing
        // (a group folder named `___`) is likewise not a skill; inventing
        // a name for it would be worse than skipping it.
        //
        // LEXICAL relative, not fs::relative: the latter canonicalizes,
        // which resolves symlinks. A skill dir symlinked into the library
        // (`skills/linked -> /elsewhere/realskill`) then measured from its
        // TARGET, escaping the root as `../../elsewhere/realskill` and
        // naming the skill after an absolute filesystem path rather than
        // after the name the user gave it in their library. The name a
        // skill gets must come from where it was PUT, not from where its
        // bytes happen to live.
        const std::string rel =
            md.parent_path().lexically_relative(root).generic_string();
        if (rel.empty() || rel == "." || rel.starts_with("..")) continue;
        const std::string slug = slug_from_path(rel);
        if (slug.empty()) continue;
        std::error_code mec;
        auto fmt = fs::last_write_time(md, mec);
        if (!mec)
            sig += std::to_string(static_cast<long long>(
                       fmt.time_since_epoch().count())) +
                   ";";
        std::string raw = read_capped(md, kMaxBodyBytes);
        if (raw.empty()) continue;
        Skill s = parse_skill(raw, slug, source);
        if (s.name.empty()) continue;
        // Shadow: earlier roots (project before user, native before
        // interop) — and earlier visit order (shallower before deeper)
        // — win on name collision.
        if (std::ranges::any_of(out, [&](const Skill& e){ return e.name == s.name; }))
            continue;
        std::error_code cec;
        auto abs = fs::weakly_canonical(md.parent_path(), cec);
        s.dir = cec ? fs::absolute(md.parent_path(), cec) : abs;
        enumerate_resources(s.dir, s.resources);
        // Tier-3 access: allowlist the skill dir for READS so the model
        // can fetch bundled scripts/references that live outside the
        // workspace (~/.agentty/skills/...) without tripping the
        // boundary. Read-only — the write/edit gate never consults it.
        util::allow_read_root(s.dir);
        out.push_back(std::move(s));
    }
}

std::vector<Skill>& cache() {
    static std::vector<Skill> c;
    return c;
}

} // namespace

std::size_t debug_cap_resolutions() noexcept {
    return cap_resolutions().load(std::memory_order_relaxed);
}

const std::vector<Skill>& all() {
    static std::mutex mu;
    static std::string cached_sig = "\x01uninit";
    std::lock_guard lk(mu);

    // Build the current signature from every root's + SKILL.md's mtime;
    // rescan only when it changed (cheap stat vs full parse per turn).
    std::string sig;
    std::vector<Skill> fresh;

    // Resolve the cap ONCE for this pass — every root's scan bound and
    // every catalog slice below share this value, so the environment
    // cannot change the answer part-way through a discovery, and a
    // malformed value logs its breadcrumb exactly once.
    const std::size_t cap = resolve_catalog_cap();

    // Precedence order comes from scope::plan (Locus-major, Dialect-minor):
    // project .agentty ▷ .agents ▷ .claude ▷ user .agentty ▷ .agents ▷
    // .claude — exactly the ladder this used to hand-write. scan_root does the
    // shadow (earlier roots win) + mtime-sig + Tier-3 allowlist per root.
    // NOTE: project stays cwd-relative here (unlike memory's project_root()
    // clamp) — skills has always resolved ".agentty/skills" against cwd, so
    // env.project_root = "." preserves that exactly.
    scope::Env env;
    env.home             = home_dir();
    env.user_native_base = ::agentty::util::user_root();
    env.project_root     = fs::path{"."};
    env.project_writable = true;   // discovery reads all dialects; unused here
    const scope::Layout layout{.leaf = "skills"};
    for (const scope::Source& src : scope::plan(layout, env)) {
        scan_root(src.base / layout.leaf, std::string{scope::to_string(src.locus)},
                  cap, fresh, sig);
    }

    if (sig != cached_sig) {
        cache() = std::move(fresh);
        cached_sig = sig;
    }
    return cache();
}

const Skill* find(std::string_view name) {
    for (const auto& s : all())
        if (s.name == name) return &s;
    return nullptr;
}

std::string catalog_block() {
    const auto& skills = all();
    // Hidden beats listed-but-blocked: user_only skills never appear, so
    // the model can't waste a turn trying to activate one.
    std::size_t eligible = 0;
    for (const auto& s : skills) if (!s.user_only) ++eligible;
    if (eligible == 0) return {};

    std::ostringstream m;
    m << "\n\n<skills>\n"
      << "On-demand skills are available. Each is a focused instruction "
         "doc you can load IN FULL with the `skill` tool when its task "
         "comes up — don't guess the contents, load it. Skills may "
         "bundle resource files (scripts/, references/, assets/); the "
         "activation result lists them — `read` the specific file "
         "when the instructions reference it, resolving relative paths "
         "against the skill's directory shown below. NEVER guess a skill "
         "path — skills live in several roots (.agentty/, .agents/, "
         ".claude/, project and user) and only the listed directory is "
         "readable. Listed: name — description (directory).\n";
    for (const auto& s : skills) {
        if (s.user_only) continue;
        m << "- " << s.name;
        if (!s.description.empty()) m << " — " << s.description;
        if (!s.dir.empty()) m << " (" << s.dir.string() << ")";
        m << "\n";
    }
    m << "</skills>";
    return m.str();
}

std::string activation_payload(const Skill& s) {
    std::ostringstream out;
    out << "<skill_content name=\"" << s.name << "\">\n";
    if (!s.description.empty()) out << s.description << "\n\n";
    if (!s.compatibility.empty())
        out << "Compatibility: " << s.compatibility << "\n\n";
    if (!s.license.empty())
        out << "License: " << s.license << "\n\n";
    if (!s.allowed_tools.empty())
        out << "Allowed tools: " << s.allowed_tools
            << " \u2014 prefer these tools while following this skill.\n\n";
    out << s.body << "\n";
    if (!s.dir.empty()) {
        out << "\nSkill directory: " << s.dir.string() << "\n"
            << "Relative paths in this skill resolve against the skill "
               "directory \u2014 use absolute paths in tool calls.\n";
    }
    if (!s.resources.empty()) {
        out << "\n<skill_resources>\n";
        for (const auto& r : s.resources) out << "  " << r << "\n";
        if (s.resources.size() >= kMaxResources)
            out << "  (listing capped \u2014 there may be more files)\n";
        out << "</skill_resources>\n"
            << "Resources are NOT loaded \u2014 `read` the specific file "
               "when the instructions call for it.\n";
    }
    out << "</skill_content>";
    return out.str();
}

namespace {
struct Activations {
    std::mutex mu;
    std::vector<std::string> names;
};
[[nodiscard]] Activations& activations() {
    static Activations a;
    return a;
}
} // namespace

bool note_activated(std::string_view name) {
    auto& a = activations();
    std::lock_guard lk(a.mu);
    for (const auto& n : a.names) if (n == name) return false;
    a.names.emplace_back(name);
    return true;
}

void reset_activations() {
    auto& a = activations();
    std::lock_guard lk(a.mu);
    a.names.clear();
}

std::vector<std::string> lint(const Skill& s) {
    std::vector<std::string> out;
    // name: 1-64 chars, lowercase alnum + hyphens, no edge/double hyphens.
    if (s.name.empty()) out.push_back("name is empty");
    if (s.name.size() > 64) out.push_back("name exceeds 64 characters");
    bool bad_char = false, prev_hyphen = false, dbl = false;
    for (char c : s.name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) bad_char = true;
        if (c == '-' && prev_hyphen) dbl = true;
        prev_hyphen = (c == '-');
    }
    if (bad_char)
        out.push_back("name has invalid characters (allowed: a-z, 0-9, hyphen)");
    if (!s.name.empty() && (s.name.front() == '-' || s.name.back() == '-'))
        out.push_back("name must not start or end with a hyphen");
    if (dbl) out.push_back("name contains consecutive hyphens");
    // name/dir: the frontmatter name must match the spec-derived slug
    // (the '-'-joined path below the discovery root). For a nested
    // skill the leaf directory alone is not the name.
    if (!s.slug.empty() && s.slug != s.name)
        out.push_back("name does not match parent directory '"
                      + s.slug + "'");
    // description: required, ≤ 1024.
    if (s.description.empty()) out.push_back("description is missing");
    if (s.description.size() > 1024)
        out.push_back("description exceeds 1024 characters");
    // compatibility: ≤ 500 when present.
    if (s.compatibility.size() > 500)
        out.push_back("compatibility exceeds 500 characters");
    // body: spec recommends ≤ 500 lines (move detail to references/).
    std::size_t lines = 1 + static_cast<std::size_t>(
        std::count(s.body.begin(), s.body.end(), '\n'));
    if (lines > 500)
        out.push_back("body is " + std::to_string(lines)
                      + " lines (spec recommends ≤ 500 — move detail to "
                        "references/)");
    return out;
}

int cmd_skills() {
    const auto& sk = all();
    if (sk.empty()) {
        std::printf("no skills installed.\n"
                    "add one: <project>/.agentty/skills/<name>/SKILL.md "
                    "(or ~/.agentty/skills/, .agents/, .claude/)\n");
        return 0;
    }
    int dirty = 0;
    for (const auto& s : sk) {
        std::printf("%-28s %-8s %s\n", s.name.c_str(), s.source.c_str(),
                    s.dir.string().c_str());
        if (!s.description.empty())
            std::printf("    %s\n", s.description.c_str());
        if (!s.resources.empty())
            std::printf("    resources: %zu file(s)\n", s.resources.size());
        if (s.user_only)
            std::printf("    [disable-model-invocation — hidden from catalog]\n");
        for (const auto& d : lint(s)) {
            std::printf("    warn: %s\n", d.c_str());
            ++dirty;
        }
    }
    std::printf("%zu skill(s), %d warning(s)\n", sk.size(), dirty);
    return dirty ? 1 : 0;
}

} // namespace agentty::tools::skills
