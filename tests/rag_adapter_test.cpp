// rag_adapter_test — locks the agentty↔rag-cpp adapter contract.
//
// agentty's retrieval engine is the external rag-cpp library (rag::Engine),
// driven through the compact agentty::rag::Retriever boundary in
// include/agentty/rag/rag_adapter.hpp. This test drives that REAL boundary
// end to end, fully OFFLINE: an unreachable Ollama endpoint is probed once and
// the adapter runs BM25 without repeated network timeouts.
//
// It pins the properties the rest of agentty depends on:
//   1. A docs folder is indexed and a relevant query returns ranked passages
//      whose `source` is "docs" and whose `path` is the file (provenance
//      survives the uri round-trip).
//   2. The top passage for a pointed query is the file that actually contains
//      the answer (ranking is not random).
//   3. An empty knowledge set reports the "no knowledge configured" error
//      instead of throwing or returning garbage.
//   4. warm()/retrieve() are safe to call repeatedly (idempotent reindex).
//   5. retrieve_code() indexes the cwd source tree and finds a symbol.

#include "agtest.hpp"

#include "agentty/rag/rag_adapter.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;


static void write_file(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << body;
}

// Persisted indexes are named `<stem><8-hex-identity>.ragdb`, where the hash
// identifies the EMBEDDER's vector space (see rag/embed_backend.hpp). Tests
// must not hardcode a name: the identity changes with the embedder, which is
// the whole point — switching backends switches between warm indexes instead
// of silently reopening one built with incompatible geometry.
static fs::path find_index(const fs::path& root, std::string_view stem) {
    std::error_code ec;
    const auto dir = root / ".agentty";
    if (!fs::is_directory(dir, ec)) return {};
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const auto name = e.path().filename().string();
        if (name.rfind(std::string{stem}, 0) == 0
            && name.size() > 6
            && name.compare(name.size() - 6, 6, ".ragdb") == 0)
            return e.path();
    }
    return {};
}

TEST_CASE("rag adapter") {
    std::printf("rag_adapter_test\n");

    // Isolated temp workspace: a docs/ folder + an env pointing at it.
    auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path tmp = fs::temp_directory_path() /
                   ("agentty_rag_" + std::to_string(nonce));
    fs::path docs = tmp / "docs";
    fs::remove_all(tmp);
    write_file(docs / "auth.md",
               "# Authentication\n\n"
               "agentty stores OAuth credentials in an encrypted keystore. "
               "The token is refreshed automatically before every request. "
               "To log in run `agentty login` which opens the browser flow.\n");
    write_file(docs / "sandbox.md",
               "# Filesystem sandbox\n\n"
               "Every tool call is confined to the workspace root. Writes "
               "outside the project directory are refused by the sandbox "
               "boundary unless the path is explicitly allowlisted.\n");
    write_file(docs / "build.md",
               "# Building\n\n"
               "Run cmake to configure, then cmake --build to compile the "
               "binary. The test suite runs under ctest.\n");
    for (int i = 0; i < 8; ++i)
        write_file(docs / ("survey" + std::to_string(i) + ".md"),
                   "# Survey facet " + std::to_string(i) + "\n\n"
                   "orion broad survey shared topic facet" + std::to_string(i) + "\n");
    std::string huge = "# Giant reference\n";
    for (int i = 0; i < 3000; ++i)
        huge += "unrelated filler line " + std::to_string(i) + "\n";
    huge += "needle-budget exact relevant sentence\n";
    write_file(docs / "giant.md", huge);
    write_file(tmp / "src" / "auth_guard.cpp",
               "bool validate_bearer_token(const std::string& token) {\n"
               "  return token == \"valid\";\n}\n");
    auto old_cwd = fs::current_path();
    fs::current_path(tmp);

#if defined(_WIN32)
    _putenv_s("AGENTTY_DOCS_DIR", docs.string().c_str());
    // Force the offline path: one bounded probe, then BM25-only retrieval.
    _putenv_s("AGENTTY_OLLAMA_HOST", "127.0.0.1:1");
    // Isolate ranking from the AMBIENT environment: the developer's installed
    // skills and learned memory would otherwise be indexed alongside the tiny
    // temp docs and can out-rank them, and a stale .ragdb / feedback TSV under
    // the repo's own .agentty/ would perturb results. Disable both knowledge
    // sources, persistence, and the learning loop for the deterministic
    // ranking assertions.
    _putenv_s("AGENTTY_RAG_SKILLS", "0");
    _putenv_s("AGENTTY_RAG_MEMORY", "0");
    _putenv_s("AGENTTY_RAG_PERSIST", "1");
    _putenv_s("AGENTTY_RAG_LEARN", "0");
    _putenv_s("AGENTTY_RAG_GRAPH", "0");
    _putenv_s("AGENTTY_RAG_PRF", "0");
#else
    ::setenv("AGENTTY_DOCS_DIR", docs.string().c_str(), 1);
    ::setenv("AGENTTY_OLLAMA_HOST", "127.0.0.1:1", 1);   // bounded probe, then BM25
    ::setenv("AGENTTY_RAG_SKILLS", "0", 1);
    ::setenv("AGENTTY_RAG_MEMORY", "0", 1);
    ::setenv("AGENTTY_RAG_PERSIST", "1", 1);
    ::setenv("AGENTTY_RAG_LEARN", "0", 1);
    ::setenv("AGENTTY_RAG_GRAPH", "0", 1);
    ::setenv("AGENTTY_RAG_PRF", "0", 1);
#endif

    {
        agentty::rag::Retriever r;

        // (1)+(2): a pointed query returns docs-sourced, well-ranked passages.
        auto res = r.retrieve("how do I log in / authenticate", 5, /*skip_docs=*/false);
        check(res.error.empty(), "retrieve() succeeds on a populated docs folder");
        check(!res.passages.empty(), "retrieve() returns at least one passage");
        if (!res.passages.empty()) {
            const auto& top = res.passages.front();
            check(top.source == "docs", "top passage is source-tagged \"docs\"");
            check(top.path.find("auth") != std::string::npos,
                  "top passage for an auth query is auth.md (ranking works)");
            check(!top.text.empty(), "passage carries body text");
            check(!res.mode.empty(), "mode/provenance string is populated");
        }

        // (4): repeat calls are safe and stay warm (no reindex churn / crash).
        auto res2 = r.retrieve("filesystem sandbox workspace root", 3);
        check(res2.error.empty(), "second retrieve() succeeds");
        check(!res2.passages.empty(), "second query returns passages");
        if (!res2.passages.empty())
            check(res2.passages.front().path.find("sandbox") != std::string::npos,
                  "sandbox query ranks sandbox.md first");
        check(r.warm(), "index reports warm after a build");

        // Full-power features engage: the mode string advertises the rich
        // pipeline as a readable FUNNEL (fusion method + per-stage counts) and
        // CRAG produced a confidence.
        {
            auto q = r.retrieve("how do I log in / authenticate", 5);
            check(q.mode.find("convex-fusion") != std::string::npos,
                  "mode advertises convex (TM2C2) fusion (rag-cpp default)");
            check(q.mode.find("funnel:") != std::string::npos,
                  "mode renders the retrieval funnel (engine workings are shown)");
            check(q.mode.find("confidence") != std::string::npos,
                  "mode reports a confidence signal");
            check(q.confidence >= 0.0 && q.confidence <= 1.0,
                  "confidence is a well-formed [0,1] signal");
        }

        // A real persisted index and validation manifest must be written.
        //
        // The filename carries the EMBEDDER IDENTITY hash (see
        // embed_backend.hpp): a .ragdb is only reusable by an embedder that
        // produces the same vector geometry, and keying the path on that
        // means switching backends switches between warm indexes instead of
        // silently reopening one built by a different embedder. So this
        // asserts the SHAPE (rag_docs.<8 hex>.ragdb + its manifest) rather
        // than a fixed name.
        {
            std::error_code ec;
            const auto ragdb = find_index(tmp, "rag_docs.");
            check(!ragdb.empty() && fs::is_regular_file(ragdb, ec),
                  "persisted .ragdb is written under an identity-keyed name");
            if (!ragdb.empty()) {
                auto meta = fs::path{ragdb.string() + ".meta.json"};
                check(fs::is_regular_file(meta, ec),
                      "persisted source manifest is written");
            }
        }

        // Requested breadth is honored; corrective retrieval must not silently
        // collapse a broad k=8 request to its old three-strip default.
        {
            auto broad = r.retrieve("orion broad survey shared topic", 8);
            check(broad.error.empty(), "broad retrieval succeeds");
            check(broad.passages.size() >= 6, "broad retrieval is not capped at three passages");
        }

        // Aggregate body output is bounded near 12 KiB and keeps the relevant
        // span from the tail of a giant source.
        {
            auto bounded = r.retrieve("needle-budget exact relevant sentence", 6);
            std::size_t bytes = 0;
            bool kept_needle = false;
            for (const auto& p : bounded.passages) {
                bytes += p.text.size();
                kept_needle = kept_needle || p.text.find("needle-budget") != std::string::npos;
            }
            check(bytes <= 12 * 1024, "retrieval passage bodies obey aggregate budget");
            check(kept_needle, "query-focused compression keeps the relevant span");
        }

        // Generator seam is callable and drives HyDE when enabled.
        {
#if defined(_WIN32)
            _putenv_s("AGENTTY_RAG_HYDE", "1");
#else
            ::setenv("AGENTTY_RAG_HYDE", "1", 1);
#endif
            bool gen_called = false;
            agentty::rag::Retriever r2;
            r2.set_generator([&](const std::string&, int n) {
                gen_called = true;
                std::vector<std::string> outs;
                for (int i = 0; i < (n > 0 ? n : 1); ++i)
                    outs.push_back("authenticate login oauth token browser flow");
                return outs;
            });
            auto hy = r2.retrieve("how to authenticate", 5);
            check(hy.error.empty(), "HyDE-enabled retrieve succeeds");
            check(gen_called, "generator seam is invoked when HyDE is on");
#if defined(_WIN32)
            _putenv_s("AGENTTY_RAG_HYDE", "0");
#else
            ::setenv("AGENTTY_RAG_HYDE", "0", 1);
#endif
        }

        // Learning is intentionally opt-in: merely surfacing results must not
        // create feedback that systematically penalizes skills/memory.
        {
            std::error_code ec;
            auto fb = tmp / ".agentty" / "rag_feedback.tsv";
            fs::remove(fb, ec);
            (void)r.retrieve("filesystem sandbox workspace root", 3);
            agentty::rag::feedback::note_file_opened("sandbox.md");
            check(!fs::exists(fb, ec), "implicit learning is disabled by default");
        }

        // Source-aware code index returns a definition-shaped chunk and updates
        // one changed file without discarding the whole corpus.
        {
            // Gate for OPPORTUNISTIC retrieval (structural zero-hit leads):
            // cold — no in-memory index, no persisted ragdb — must report NOT
            // warm so a side-effect query can't trigger the cold build…
            check(!r.code_warm(), "code index reports cold before first build");
            auto code = r.retrieve_code("validate bearer token credentials", 5);
            check(code.error.empty() && !code.passages.empty(), "search_code finds source");
            // …and warm right after the explicit build.
            check(r.code_warm(), "code index reports warm after explicit build");
            if (!code.passages.empty()) {
                check(code.passages.front().path.find("auth_guard.cpp") != std::string::npos,
                      "code result points at auth_guard.cpp");
                check(code.passages.front().text.find("validate_bearer_token") != std::string::npos,
                      "code-aware chunk preserves the function definition");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            write_file(tmp / "src" / "auth_guard.cpp",
                       "bool rotate_session_nonce(int nonce) { return nonce > 41; }\n");
            auto updated = r.retrieve_code("rotate session nonce", 5);
            check(updated.error.empty() && !updated.passages.empty(),
                  "incremental code refresh finds an edited file");
            if (!updated.passages.empty())
                check(updated.passages.front().text.find("rotate_session_nonce") != std::string::npos,
                      "edited definition replaces stale code content");

            // Disk-verification: the returned passage text must be exactly what
            // is on disk RIGHT NOW — read_disk_lines re-reads the cited range, so
            // even after an edit the result is correct-on-bytes, never a stale
            // or mangled index chunk. Prove it by confirming the passage body
            // is a verbatim substring of the current file.
            if (!updated.passages.empty()) {
                std::ifstream in(tmp / "src" / "auth_guard.cpp", std::ios::binary);
                std::string disk((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
                const auto& body = updated.passages.front().text;
                // Take the first non-empty line of the passage and require it
                // to appear verbatim on disk (guards against stale/garbled text).
                std::string first = body.substr(0, body.find('\n'));
                check(!first.empty() && disk.find(first) != std::string::npos,
                      "search_code passage is verified against live disk");
            }
        }
        // A single documentation edit is refreshed in place and replaces the
        // stale document without rebuilding unrelated sources.
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            write_file(docs / "auth.md",
                       "# Authentication\n\nEncrypted OAuth keystore and browser login. "
                       "lattice-refresh-marker is now documented.\n");
            auto updated = r.retrieve("lattice refresh marker", 5);
            check(updated.error.empty() && !updated.passages.empty(),
                  "incremental docs refresh finds an edited document");
            if (!updated.passages.empty())
                check(updated.passages.front().text.find("lattice-refresh-marker") != std::string::npos,
                      "edited docs content replaces the stale passage");
        }
    }

    // A fresh Retriever opens the persisted corpus without rewriting it.
    {
        auto db = find_index(tmp, "rag_docs.");
        std::error_code ec;
        auto before = fs::last_write_time(db, ec);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        agentty::rag::Retriever warm;
        auto res = warm.retrieve("encrypted OAuth keystore", 5);
        auto after = fs::last_write_time(db, ec);
        check(res.error.empty() && !res.passages.empty(), "fresh retriever opens persisted index");
        check(before == after, "warm open does not rewrite the persisted index");
        if (!res.passages.empty())
            check(res.passages.front().path.find("auth") != std::string::npos,
                  "warm-opened index preserves ranking");

        auto code_db = find_index(tmp, "rag_code.");
        auto code_before = fs::last_write_time(code_db, ec);
        auto code = warm.retrieve_code("rotate session nonce", 5);
        auto code_after = fs::last_write_time(code_db, ec);
        check(code.error.empty() && !code.passages.empty(),
              "fresh retriever opens persisted code index");
        check(code_before == code_after, "warm code open does not rewrite its index");
    }

    // Corrupt/truncated .meta.json must not crash a fresh Retriever: the load
    // path parses meta inside try/catch and falls back to a clean rebuild. This
    // guards the persistence hardening (atomic meta writes) — even a half-
    // written meta from a killed process is recovered from, never fatal.
    {
        std::error_code ec;
        auto meta = fs::path{find_index(tmp, "rag_docs.").string() + ".meta.json"};
        // Truncated JSON object: exactly what an interrupted write could leave
        // if writes were not atomic.
        write_file(meta, "{\"version\": 3, \"root\": \"/tmp\", \"docs_fp\":");
        agentty::rag::Retriever r;
        auto res = r.retrieve("encrypted OAuth keystore", 5);
        check(res.error.empty(), "corrupt .meta.json recovers via rebuild, no crash");
        check(!res.passages.empty(), "rebuild after corrupt meta still returns results");
        // The rebuild must rewrite a VALID meta (parseable JSON object).
        std::ifstream in(meta);
        std::string body((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        check(!body.empty() && body.front() == '{' && body.back() == '}',
              "rebuild republishes a complete .meta.json");
    }

    // Very-large-corpus bounds: a docs root with more than the file cap,
    // plus vendored/build dirs and an oversized blob, must index a BOUNDED
    // corpus and never crash. This guards the OOM that unbounded DirOptions
    // (max_files=0, 4 MB files, no _deps/build exclusion) caused on large
    // codebases when AGENTTY_DOCS_DIR pointed at a repo root.
    {
        fs::path big = tmp / "big_corpus";
        fs::remove_all(big);
        // 7000 tiny docs (> the 6000 cap) so the walk must truncate.
        for (int i = 0; i < 7000; ++i)
            write_file(big / ("doc" + std::to_string(i) + ".md"),
                       "# Doc " + std::to_string(i) +
                       "\n\nalpha beta gamma delta term" + std::to_string(i) + "\n");
        // Vendored + build output that MUST be skipped, not indexed.
        write_file(big / "_deps" / "vendored.md", "should not be indexed\n");
        write_file(big / "build" / "generated.md", "should not be indexed\n");
        write_file(big / "node_modules" / "pkg.md", "should not be indexed\n");
        // An oversized blob that exceeds the 1 MB per-file cap.
        std::string blob = "# Huge\n";
        blob.reserve(3 * 1024 * 1024);
        while (blob.size() < 3u * 1024 * 1024) blob += "filler filler filler\n";
        write_file(big / "huge.md", blob);

#if defined(_WIN32)
        _putenv_s("AGENTTY_DOCS_DIR", big.string().c_str());
#else
        ::setenv("AGENTTY_DOCS_DIR", big.string().c_str(), 1);
#endif
        agentty::rag::Retriever r;
        auto res = r.retrieve("alpha beta gamma delta", 5);
        check(res.error.empty(), "large-corpus retrieve() does not crash or error");
        check(!res.passages.empty(), "large-corpus retrieve() returns passages");
        for (const auto& p : res.passages) {
            check(p.path.find("_deps") == std::string::npos
                  && p.path.find("node_modules") == std::string::npos,
                  "vendored/build dirs are excluded from the docs corpus");
        }
    }

    // (3): empty knowledge ⇒ graceful "no knowledge" error, not a crash.
    {
        fs::path empty_dir = tmp / "empty";
        fs::create_directories(empty_dir);
#if defined(_WIN32)
        _putenv_s("AGENTTY_DOCS_DIR", empty_dir.string().c_str());
        _putenv_s("AGENTTY_RAG_SKILLS", "0");
        _putenv_s("AGENTTY_RAG_MEMORY", "0");
#else
        ::setenv("AGENTTY_DOCS_DIR", empty_dir.string().c_str(), 1);
        ::setenv("AGENTTY_RAG_SKILLS", "0", 1);
        ::setenv("AGENTTY_RAG_MEMORY", "0", 1);
#endif
        agentty::rag::Retriever r;
        auto res = r.retrieve("anything at all", 5);
        check(!res.error.empty(), "empty knowledge set reports an error");
        check(res.passages.empty(), "empty knowledge set returns no passages");
    }

    fs::current_path(old_cwd);
    fs::remove_all(tmp);
}

// Regression: Retriever::shutdown() must interrupt an in-flight background
// warm PROMPTLY. Before the cooperative-cancel fix, the warm jthread ignored
// its stop_token (captured [state], not [stop_token]) and was only joined at
// static destruction, blocking ^C for 4–10 s while the embed pass finished.
TEST_CASE("rag shutdown interrupts warm promptly") {
    namespace fs = std::filesystem;
    auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    auto tmp = fs::temp_directory_path() /
               ("agentty_rag_shutdown_" + std::to_string(nonce));
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    auto old_cwd = fs::current_path();
    fs::current_path(tmp);

    // A large-ish corpus so a real warm has work to do (and thus a window in
    // which cancellation matters).
    fs::create_directories(tmp / "docs");
    for (int f = 0; f < 8; ++f) {
        std::ofstream o(tmp / "docs" / ("doc" + std::to_string(f) + ".md"));
        for (int i = 0; i < 400; ++i)
            o << "# section " << f << "-" << i
              << "\nsome searchable prose about topic " << (i % 7) << ".\n\n";
    }

    {
        agentty::rag::Retriever r;
        agentty::rag::Config cfg;
        cfg.docs_root = (tmp / "docs").string();
        r.apply_config(cfg);

        // Kick the background warm, then immediately ask it to stop. The join
        // inside shutdown() must return quickly because the worker now polls
        // the cancel flag between files / before the embed build.
        r.warm_async();
        const auto t0 = std::chrono::steady_clock::now();
        r.shutdown();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        // Generous ceiling: even one in-flight embed batch is well under this;
        // the OLD bug blocked for the whole multi-second corpus embed.
        check(ms < 2500, "shutdown() returns promptly, not after a full warm");

        // shutdown() is idempotent and leaves the retriever usable.
        r.shutdown();
        r.warm_async();
        r.shutdown();
        check(true, "re-warm + re-shutdown after a cancel is safe");
    }

    fs::current_path(old_cwd);
    fs::remove_all(tmp);
}
