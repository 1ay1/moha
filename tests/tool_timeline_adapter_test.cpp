// Exhaustive data-level coverage for native tool timeline cards.
// Every lifecycle state must render useful body text rather than a blank card.

#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "agtest.hpp"

#include "agentty/domain/conversation.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/view/panels.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_body_preview.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_helpers.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_args.hpp"
#include "maya/widget/tool_body_preview.hpp"

namespace A = agentty;
namespace U = agentty::ui;
using Kind = maya::ToolBodyPreview::Kind;



static A::ToolUse make_tool(std::string name, A::ToolUse::Status status,
                            nlohmann::json args = nlohmann::json::object()) {
    static int sequence = 0;
    A::ToolUse tc;
    tc.id = A::ToolCallId{"card-" + std::to_string(++sequence)};
    tc.name = A::ToolName{std::move(name)};
    tc.args = std::move(args);
    tc.status = std::move(status);
    return tc;
}

static bool visible(const maya::ToolBodyPreview::Config& body) {
    switch (body.kind) {
    case Kind::None:     return false;
    case Kind::EditDiff: return !body.hunks.empty();
    case Kind::TodoList: return !body.todos.empty();
    default:             return !body.text.empty();
    }
}

TEST_CASE("tool timeline adapter") {
    static constexpr std::array<const char*, 33> native_tools = {
        "read", "edit", "write", "move", "remove", "shell",
        "process_start", "process_poll", "process_stop",
        "grep", "glob", "list_dir", "repo_map", "todo",
        "web_fetch", "web_search", "find_definition", "search_structural",
        "diagnostics", "test", "git_status", "git_diff", "git_log",
        "git_show", "git_blame", "git_commit", "remember", "forget",
        "wipe_memory", "task", "skill", "search_docs", "search_code"
    };

    for (const char* name : native_tools) {
        auto expect_header_only = [&](A::ToolUse tc, const char* state) {
            const auto body = U::tool_body_preview_config(tc);
            check(!visible(body) && !U::tool_display_name(name).empty(),
                  std::string{name} + " " + state
                      + " stays zero-row with a useful event header");
        };
        auto expect_visible = [&](A::ToolUse tc, const char* state) {
            const auto body = U::tool_body_preview_config(tc);
            check(visible(body), std::string{name} + " " + state + " has a body");
        };

        expect_header_only(make_tool(name, A::ToolUse::Pending{}), "pending");
        expect_header_only(make_tool(name, A::ToolUse::Approved{}), "approved");
        expect_header_only(make_tool(name, A::ToolUse::Running{}), "silent running");

        auto live = make_tool(name, A::ToolUse::Running{});
        std::get<A::ToolUse::Running>(live.status).progress_text = "live progress";
        const auto live_body = U::tool_body_preview_config(live);
        check(visible(live_body) && live_body.text == "live progress"
                  && live_body.is_streaming,
              std::string{name} + " exposes live progress");

        expect_header_only(make_tool(name, A::ToolUse::Done{}), "empty success");
        expect_visible(make_tool(name, A::ToolUse::Done{{}, {}, "result text"}),
                       "success output");

        const auto empty_failure = U::tool_body_preview_config(
            make_tool(name, A::ToolUse::Failed{}));
        check(!visible(empty_failure),
              std::string{name} + " empty failure stays header-only");

        const auto failure = U::tool_body_preview_config(
            make_tool(name, A::ToolUse::Failed{{}, {}, "specific error"}));
        check(visible(failure) && failure.text.find("specific error") != std::string::npos,
              std::string{name} + " preserves failure output");

        expect_header_only(make_tool(name, A::ToolUse::Rejected{}), "rejected");
        check(!U::tool_display_name(name).empty(),
              std::string{name} + " has a display name");
    }

    // Future native and MCP tools inherit the same fallback without needing
    // to be added to the dispatcher's name list.
    auto external = make_tool("mcp__example__long_operation", A::ToolUse::Running{},
                              {{"display_description", "Long MCP operation"}});
    auto external_body = U::tool_body_preview_config(external);
    check(!visible(external_body)
              && U::tool_timeline_detail(external) == "Long MCP operation",
          "unknown silent MCP tool uses its stable event header");
    check(U::tool_display_name("mcp_search_tools") == "MCP Tool Search"
              && U::tool_timeline_detail(make_tool("mcp_search_tools",
                  A::ToolUse::Running{}, {{"query", "browser automation"}}))
                  == "browser automation",
          "MCP catalog broker has a useful label and query");
    check(U::tool_display_name("mcp_call") == "MCP Call"
              && U::tool_timeline_detail(make_tool("mcp_call",
                  A::ToolUse::Running{}, {{"name", "mcp__browser__click"}}))
                  == "mcp__browser__click",
          "MCP call broker identifies its target");

    auto invisible_live = make_tool("mcp__example__silent", A::ToolUse::Running{},
                                    {{"display_description", "Silent MCP wait"}});
    std::get<A::ToolUse::Running>(invisible_live.status).progress_text =
        "\x1b[2K\r\n\t";
    const auto invisible_live_body = U::tool_body_preview_config(invisible_live);
    check(!visible(invisible_live_body)
              && U::tool_timeline_detail(invisible_live) == "Silent MCP wait",
          "ANSI-only progress falls back to the stable event header");

    const auto whitespace_done = U::tool_body_preview_config(make_tool(
        "mcp__example__silent", A::ToolUse::Done{{}, {}, " \n\t"}));
    check(!visible(whitespace_done),
          "whitespace-only success stays header-only");

    const auto ansi_failure = U::tool_body_preview_config(make_tool(
        "mcp__example__silent", A::ToolUse::Failed{{}, {}, "\x1b[31m\x1b[0m"}));
    check(!visible(ansi_failure),
          "ANSI-only failure stays header-only");

    // Structured renderers still win when they have meaningful content.
    auto read = make_tool("read", A::ToolUse::Done{{}, {}, "one\ntwo"},
                          {{"path", "sample.cpp"}});
    check(U::tool_body_preview_config(read).kind == Kind::FileRead,
          "read keeps structured file body");

    // Real line numbers: the pagination footer is stripped from the body
    // and mined for the TRUE first line (authoritative over the args echo).
    {
        auto tc = make_tool("read",
            A::ToolUse::Done{{}, {},
                "alpha\nbeta\n\n[showing lines 40-41 of 90; 49 more \xe2\x80\x94 "
                "pass offset=42 for the next chunk]"},
            {{"path", "sample.cpp"}});
        const auto body = U::tool_body_preview_config(tc);
        check(body.kind == Kind::FileRead && body.start_line == 40,
              "read footer anchors gutter at true start line");
        check(body.text.find("[showing lines") == std::string::npos,
              "read footer stripped from numbered body");
    }
    // Symbol reads: the SUCCESS header is stripped and its `(lines A\u2013B)`
    // range anchors the gutter even though no offset arg exists.
    {
        auto tc = make_tool("read",
            A::ToolUse::Done{{}, {},
                "SUCCESS: `foo` defined at f.cpp:12 (lines 12\xe2\x80\x93 14).\n\n"
                "int foo() {\n  return 1;\n}\n"},
            {{"path", "f.cpp"}, {"symbol", "foo"}});
        const auto body = U::tool_body_preview_config(tc);
        check(body.kind == Kind::FileRead && body.start_line == 12,
              "symbol read anchors gutter at definition line");
        check(body.text.rfind("int foo", 0) == 0,
              "symbol header stripped from numbered body");
        check(U::tool_timeline_detail(tc).find("foo()") != std::string::npos,
              "read header names the symbol read");
    }

    // grep markdown is re-synthesized into path:line:text for the
    // structured GrepMatches table, with line numbers derived from the
    // `### L<s>-<e>` block tags.
    {
        auto tc = make_tool("grep",
            A::ToolUse::Done{{}, {},
                "Found 2 matches across 1 file.\n\n"
                "## Matches in src/a.cpp\n\n"
                "### fn foo \xe2\x80\xba L41-43\n```\n"
                "int x = 1;\nint y = 2;\nint z = 3;\n```\n\n"
                "Showing all 2 matches."},
            {{"pattern", "int"}});
        const auto body = U::tool_body_preview_config(tc);
        check(body.kind == Kind::GrepMatches,
              "grep renders the structured match table");
        check(body.text.find("src/a.cpp:41:int x = 1;") != std::string::npos
                  && body.text.find("src/a.cpp:43:int z = 3;") != std::string::npos,
              "grep rows carry true derived line numbers");
        check(body.text.find("Showing all") == std::string::npos
                  && body.text.find("Found 2") == std::string::npos,
              "grep headline/pagination chatter dropped from body");
    }
    // Unparsable grep output (no fences) stays readable as a CodeBlock.
    {
        auto tc = make_tool("grep",
            A::ToolUse::Done{{}, {}, "No matches for 'zzz'. Try: loosen"},
            {{"pattern", "zzz"}});
        check(U::tool_body_preview_config(tc).kind == Kind::CodeBlock,
              "fenceless grep output falls back to code block");
    }

    // process_* share bash's terminal-output body (tail window, structured
    // extraction) instead of the generic head-anchored fallback.
    {
        auto tc = make_tool("process_poll",
            A::ToolUse::Done{{}, {}, "server listening on :8080\n"});
        check(U::tool_body_preview_config(tc).kind == Kind::BashOutput,
              "process_poll output renders as terminal output");
    }

    // replace dry-run preview reshapes into a colored GitDiff card.
    {
        auto tc = make_tool("replace",
            A::ToolUse::Done{{}, {},
                "DRY RUN \xe2\x80\x94 would replace 1 occurrence of `foo` \xe2\x86\x92 `bar` "
                "across 1 file.\n\n"
                "src/a.cpp:12  (1 hit)\n"
                "  - int foo = 1;\n"
                "  + int bar = 1;\n\n"
                "Re-run with apply:true to write these changes."},
            {{"find", "foo"}, {"replacement", "bar"}});
        const auto body = U::tool_body_preview_config(tc);
        check(body.kind == Kind::GitDiff, "replace preview renders as a diff");
        check(body.text.find("-int foo = 1;") != std::string::npos
                  && body.text.find("+int bar = 1;") != std::string::npos,
              "replace preview keeps the -/+ lines line-leading for GitDiff");
        check(body.text.find("DRY RUN") == std::string::npos,
              "replace summary line dropped from the diff body");
    }
    // rewrite_structural preview: per-line L<n>: anchors become @@ L<n> @@.
    {
        auto tc = make_tool("rewrite_structural",
            A::ToolUse::Done{{}, {},
                "DRY RUN \xe2\x80\x94 would rewrite 1 occurrence across 1 file.\n"
                "src/b.cpp:\n"
                "  L42: - assertEquals(a, b)\n"
                "  L42: + assertEqual(b, a)\n"},
            {{"pattern", "assertEquals($A, $B)"}});
        const auto body = U::tool_body_preview_config(tc);
        check(body.kind == Kind::GitDiff,
              "rewrite_structural preview renders as a diff");
        check(body.text.find("@@ L42 @@") != std::string::npos,
              "rewrite_structural line number becomes a hunk anchor");
        check(body.text.find("-assertEquals(a, b)") != std::string::npos
                  && body.text.find("+assertEqual(b, a)") != std::string::npos,
              "rewrite_structural -/+ lines are line-leading");
    }
    // A 'no matches' replace message has no -/+ preview → stays a code block.
    {
        auto tc = make_tool("replace",
            A::ToolUse::Done{{}, {},
                "no structural matches \xe2\x80\x94 nothing to rewrite."},
            {{"find", "zzz"}, {"replacement", "q"}});
        check(U::tool_body_preview_config(tc).kind == Kind::CodeBlock,
              "replace with no preview falls back to code block");
    }

    auto todo = make_tool("todo", A::ToolUse::Running{},
        {{"todos", {{{"content", "Audit cards"}, {"status", "in_progress"}}}}});
    check(U::tool_body_preview_config(todo).kind == Kind::TodoList,
          "todo keeps structured checklist");

    auto failed_todo = make_tool("todo", A::ToolUse::Failed{{}, {}, "todo failed"},
        {{"todos", {{{"content", "Audit cards"}, {"status", "in_progress"}}}}});
    check(U::tool_body_preview_config(failed_todo).kind == Kind::Failure,
          "failed todo shows error instead of stale checklist");

    // Representative detail mappings prove formerly anonymous tools explain
    // their operation in the event header while the fallback body explains
    // lifecycle state.
    check(U::tool_timeline_detail(make_tool("move", A::ToolUse::Running{},
              {{"source", "a"}, {"destination", "b"}})).find("a") != std::string::npos,
          "move detail includes source");
    check(U::tool_timeline_detail(make_tool("search_structural", A::ToolUse::Running{},
              {{"pattern", "Widget($$$)"}, {"path", "src"}})).find("Widget") != std::string::npos,
          "structural detail includes pattern");
    check(U::tool_timeline_detail(make_tool("search_code", A::ToolUse::Running{},
              {{"query", "retry handling"}})) == "retry handling",
          "semantic search detail includes query");
    check(!U::tool_timeline_detail(make_tool("remove", A::ToolUse::Pending{},
              {{"path", "tmp"}, {"recursive", "not-a-boolean"}})).empty(),
          "malformed remove boolean cannot break rendering");
    check(U::tool_timeline_detail(make_tool("wipe_memory", A::ToolUse::Pending{},
              {{"scope", "project"}, {"confirm", nullptr}})).find("preview")
              != std::string::npos,
          "malformed wipe confirmation cannot break rendering");
    check(!U::tool_timeline_detail(make_tool("search_code", A::ToolUse::Pending{},
              {{"query", nlohmann::json::array({"wrong type"})}})).empty(),
          "malformed string argument cannot break rendering");
    check(!U::tool_timeline_detail(make_tool("git_blame", A::ToolUse::Pending{},
              {{"path", "file.cpp"}, {"start_line", "wrong type"}})).empty(),
          "malformed integer argument cannot break rendering");

    // Failure summary: a Failed tool surfaces a concise reason in its header
    // detail so it doesn't look identical to a success. Structured
    // "[kind] detail" errors show the KIND; bash exit trailers show "exit
    // code N"; a bare error shows its first line. Rejected tools stay plain.
    check(U::tool_timeline_detail(make_tool("edit", A::ToolUse::Failed{{}, {},
              "[not found] old_text did not match"},
              {{"path", "x.cpp"}})).find("not found") != std::string::npos,
          "failed edit surfaces the error kind in the header");
    check(U::tool_timeline_detail(make_tool("shell", A::ToolUse::Failed{{}, {},
              "boom\n[exit code 1]"},
              {{"command", "false"}})).find("exit code 1") != std::string::npos,
          "failed bash surfaces the exit code in the header");
    check(U::tool_timeline_detail(make_tool("read", A::ToolUse::Failed{{}, {},
              "permission denied"},
              {{"path", "x"}})).find("permission denied") != std::string::npos,
          "failed read surfaces an unstructured reason");
    {
        // A Failed tool with no output still gets a 'failed' marker rather
        // than looking successful.
        const auto d = U::tool_timeline_detail(
            make_tool("grep", A::ToolUse::Failed{}, {{"pattern", "x"}}));
        check(d.find("failed") != std::string::npos,
              "failed tool with empty output still marked failed");
    }
    {
        // A successful tool never gets a failure suffix.
        const auto d = U::tool_timeline_detail(make_tool("read",
            A::ToolUse::Done{{}, {}, "ok"}, {{"path", "x.cpp"}}));
        check(d.find("failed") == std::string::npos
                  && d.find("[") == std::string::npos,
              "successful tool has no failure suffix");
    }
    check(U::tool_timeline_detail(make_tool("read", A::ToolUse::Rejected{},
              {{"path", "x.cpp"}})).find("failed") == std::string::npos,
          "rejected tool is not labelled failed");

    // Line/entry counts must reflect the FILE/DIR, not the decorated output.
    // read: the authoritative count is the "of N" the tool reports in its
    // "[showing lines A-B of N]" footer — not a re-count of output rows
    // (which includes the display_description prepend + the footer itself).
    {
        // A big-file read: 3 visible rows of body but the footer says 812.
        std::string out = "describe this\n1: line one\n2: line two\n3: line "
                          "three\n[showing lines 1-3 of 812; pass offset=4]";
        const auto d = U::tool_timeline_detail(make_tool("read",
            A::ToolUse::Done{{}, {}, out}, {{"path", "big.cpp"},
            {"display_description", "describe this"}}));
        check(d.find("812 lines") != std::string::npos,
              "read reports the file's true line count, not decorated rows");
        check(d.find("5 lines") == std::string::npos,
              "read does not count the description + footer rows");
    }
    {
        // Outline fallback shape: "path — N definitions across M lines:".
        std::string out = "map it\nfoo.cpp \xe2\x80\x94 4 definitions across "
                          "296 lines:\n\nL1 a\nL2 b";
        const auto d = U::tool_timeline_detail(make_tool("read",
            A::ToolUse::Done{{}, {}, out}, {{"path", "foo.cpp"}}));
        check(d.find("296 lines") != std::string::npos,
              "read outline reports the 'across N lines' count");
    }
    {
        // Small file read whole (no footer): fall back to content lines.
        std::string out = "a\nb\nc";
        const auto d = U::tool_timeline_detail(make_tool("read",
            A::ToolUse::Done{{}, {}, out}, {{"path", "tiny.txt"}}));
        check(d.find("3 lines") != std::string::npos,
              "small whole-file read falls back to a content line count");
    }
    {
        // list_dir: description prepend + a truncation notice must NOT be
        // counted as entries. 3 real rows here.
        std::string out = "listing\nfile a.txt\ndir  sub/\nfile b.txt\n"
                          "[>1000 entries, truncated]";
        const auto d = U::tool_timeline_detail(make_tool("list_dir",
            A::ToolUse::Done{{}, {}, out}, {{"path", "."},
            {"display_description", "listing"}}));
        check(d.find("3 entries") != std::string::npos,
              "list_dir counts real entries, excluding desc + notice lines");
    }

    // Edited-file visibility: the filename must show DURING streaming, before
    // the throttled args reparse catches up. A Pending edit whose parsed args
    // are still empty but whose raw stream already carries "path" must render
    // the path, not a bare "…".
    {
        auto tc = make_tool("edit", A::ToolUse::Pending{});
        tc.args = nlohmann::json::object();   // parse hasn't caught up
        tc.args_streaming =
            R"({"display_description":"fix","path":"src/foo.cpp","edits":[)";
        const auto d = U::tool_timeline_detail(tc);
        check(d.find("src/foo.cpp") != std::string::npos,
              "edit shows the streaming path before args reparse");
        check(d != "\xe2\x80\xa6", "edit is not a bare ellipsis while streaming");
    }
    {
        // Truncated mid-path: show what streamed so far rather than nothing.
        auto tc = make_tool("write", A::ToolUse::Pending{});
        tc.args_streaming = R"({"path":"src/partial)";
        check(U::tool_timeline_detail(tc).find("src/partial") != std::string::npos,
              "write shows a truncated streaming path");
    }
    {
        // Parsed args win once present (authoritative), stream ignored.
        auto tc = make_tool("read", A::ToolUse::Done{{}, {}, "a\nb"},
                            {{"path", "real.cpp"}});
        tc.args_streaming = R"({"path":"stale.cpp"})";
        check(U::tool_timeline_detail(tc).find("real.cpp") != std::string::npos,
              "parsed path is authoritative over the streaming buffer");
    }
    {
        // move: source/destination visible from the stream too.
        auto tc = make_tool("move", A::ToolUse::Pending{});
        tc.args_streaming = R"({"source":"a.txt","destination":"b.txt"})";
        const auto d = U::tool_timeline_detail(tc);
        check(d.find("a.txt") != std::string::npos
                  && d.find("b.txt") != std::string::npos,
              "move shows streaming source and destination");
    }
    // pick_streaming_string escape + key-vs-value handling.
    check(U::pick_streaming_string(R"({"path":"a\/b\nc"})", "path") == "a/b\nc",
          "pick_streaming_string decodes escapes");
    check(U::pick_streaming_string(R"({"x":"path","path":"real"})", "path")
              == "real",
          "pick_streaming_string finds the KEY, not a same-named value");
    check(U::pick_streaming_string(R"({"other":"x"})", "path").empty(),
          "pick_streaming_string returns empty when key absent");

    // Every tool's header arg is streaming-live, not just the file path: a
    // command/pattern/query shows the moment its bytes arrive, before the
    // parsed-args reparse. (Same mechanism, applied uniformly via safe().)
    {
        auto tc = make_tool("shell", A::ToolUse::Pending{});
        tc.args_streaming = R"({"command":"npm run build)";  // truncated
        check(U::tool_timeline_detail(tc).find("npm run build") != std::string::npos,
              "bash command shows while streaming");
    }
    {
        auto tc = make_tool("grep", A::ToolUse::Pending{});
        tc.args_streaming = R"({"pattern":"TODO","path":"src)";
        check(U::tool_timeline_detail(tc).find("TODO") != std::string::npos,
              "grep pattern shows while streaming");
    }
    {
        auto tc = make_tool("search_code", A::ToolUse::Pending{});
        tc.args_streaming = R"({"query":"retry backoff)";
        check(U::tool_timeline_detail(tc).find("retry backoff") != std::string::npos,
              "search query shows while streaming");
    }

    const nlohmann::json malformed_edit_args = {{"edits", {
        {{"old_text", nlohmann::json::array()}, {"new_text", nullptr}}
    }}};
    for (auto tc : {
            make_tool("edit", A::ToolUse::Pending{}, malformed_edit_args),
            make_tool("edit", A::ToolUse::Running{}, malformed_edit_args),
            make_tool("edit", A::ToolUse::Done{{}, {}, "done"}, malformed_edit_args)}) {
        const auto body = U::tool_body_preview_config(tc);
        check(visible(body) || !U::tool_timeline_detail(tc).empty(),
              "malformed nested edit fields remain renderable");
    }

    const nlohmann::json malformed_todo_args = {{"todos", {
        {{"content", nlohmann::json::array()}, {"status", nullptr}}
    }}};
    auto malformed_todo = make_tool("todo", A::ToolUse::Running{}, malformed_todo_args);
    check((visible(U::tool_body_preview_config(malformed_todo))
               || !U::tool_timeline_detail(malformed_todo).empty()),
          "malformed nested todo fields remain renderable");

    auto huge_read = make_tool("read", A::ToolUse::Done{{}, {}, "line"},
        {{"path", "file.cpp"},
         {"offset", std::numeric_limits<std::uint64_t>::max()}});
    check(U::tool_body_preview_config(huge_read).start_line == 1,
          "out-of-range read offset safely uses default line");

    // Exercise the real overlay adapter, not only the generic Picker. Its old
    // 60-column floor overflowed phone/SSH terminals before flex could help.
    A::Model viewer_model;
    A::tool_output::Entry viewer_entry;
    viewer_entry.name = "shell";
    viewer_entry.title = "Diagnostics";
    viewer_entry.detail = "cmake --build a-very-long-target-name";
    viewer_entry.trailing = "running · 41.2s · 1 MB";
    viewer_entry.output = "line one\nline two\nline three";
    viewer_entry.is_live = true;
    viewer_entry.call = make_tool("shell", A::ToolUse::Running{});
    viewer_model.ui.panel = A::ui::panel::ToolOutput{{{viewer_entry}, 0, false}};
    int list_height = -1;
    for (int width = 8; width <= 120; ++width) {
        auto viewer = U::tool_output_panel(viewer_model);
        auto constrained = maya::dsl::vstack()
            .width(maya::Dimension::fixed(width))(viewer);
        auto measured = maya::measure_element(constrained, width);
        check(measured.width.value <= width,
              "tool viewer list fits width " + std::to_string(width));
        if (list_height < 0) list_height = measured.height.value;
        check(measured.height.value == list_height,
              "tool viewer list never wraps at width " + std::to_string(width));
    }
    viewer_model.ui.panel.get<A::ui::panel::ToolOutput>()->viewing = true;
    int body_height = -1;
    for (int width = 8; width <= 120; ++width) {
        auto viewer = U::tool_output_panel(viewer_model);
        auto constrained = maya::dsl::vstack()
            .width(maya::Dimension::fixed(width))(viewer);
        auto measured = maya::measure_element(constrained, width);
        check(measured.width.value <= width,
              "tool viewer body fits width " + std::to_string(width));
        if (body_height < 0) body_height = measured.height.value;
        check(measured.height.value == body_height,
              "tool viewer body never wraps at width " + std::to_string(width));
    }

    std::vector<A::ToolUse> structured_calls;
    structured_calls.push_back(make_tool("read",
        A::ToolUse::Done{{}, {}, "first line\nsecond line\nthird line"},
        {{"path", "very-long-file-name.cpp"}}));
    structured_calls.push_back(make_tool("write", A::ToolUse::Done{},
        {{"path", "generated.cpp"},
         {"content", "a very long generated source line that must clip\nnext"}}));
    structured_calls.push_back(make_tool("edit", A::ToolUse::Done{{}, {}, "done"},
        {{"path", "edited.cpp"},
         {"edits", {{{"old_text", "old long value"},
                      {"new_text", "new long replacement value"}}}}}));
    structured_calls.push_back(make_tool("git_diff",
        A::ToolUse::Done{{}, {}, "@@ -1 +1 @@\n-old value\n+new value"}));
    structured_calls.push_back(make_tool("todo", A::ToolUse::Done{},
        {{"todos", {{{"content", "A long todo item that cannot wrap"},
                     {"status", "completed"}}}}}));

    static constexpr std::array<int, 7> viewer_widths = {8, 12, 20, 32, 48, 80, 120};
    for (auto& call : structured_calls) {
        A::tool_output::Entry entry;
        entry.name = call.name.value;
        entry.title = U::tool_display_name(entry.name);
        entry.detail = "structured output";
        entry.trailing = "ok · 1.0s";
        entry.output = call.output();
        entry.call = call;
        A::Model structured_model;
        structured_model.ui.panel =
            A::ui::panel::ToolOutput{{{std::move(entry)}, 0, true}};
        int expected_height = -1;
        for (int width : viewer_widths) {
            auto viewer = U::tool_output_panel(structured_model);
            auto constrained = maya::dsl::vstack()
                .width(maya::Dimension::fixed(width))(viewer);
            auto measured = maya::measure_element(constrained, width);
            if (expected_height < 0) expected_height = measured.height.value;
            check(measured.width.value <= width
                      && measured.height.value == expected_height,
                  call.name.value + " structured body fits width "
                      + std::to_string(width));
        }
    }

    for (bool live : {false, true}) {
        A::tool_output::Entry empty;
        empty.name = "mcp__example__silent";
        empty.title = "Silent MCP Operation";
        empty.detail = "waiting for remote service";
        empty.trailing = live ? "running" : "ok";
        empty.is_live = live;
        empty.call = live
            ? make_tool(empty.name, A::ToolUse::Running{})
            : make_tool(empty.name, A::ToolUse::Done{});
        A::Model empty_model;
        empty_model.ui.panel =
            A::ui::panel::ToolOutput{{{std::move(empty)}, 0, true}};
        int expected_height = -1;
        for (int width : viewer_widths) {
            auto viewer = U::tool_output_panel(empty_model);
            auto constrained = maya::dsl::vstack()
                .width(maya::Dimension::fixed(width))(viewer);
            auto measured = maya::measure_element(constrained, width);
            if (expected_height < 0) expected_height = measured.height.value;
            check(measured.width.value <= width
                      && measured.height.value == expected_height,
                  std::string{live ? "live" : "settled"}
                      + " empty body fits width " + std::to_string(width));
        }
    }

    // Long bodies add a range-indicator footer at normal heights. Sweep short
    // non-TTY terminals and ensure compact chrome keeps the complete border in
    // bounds, including the old inherited four-row-floor failure at 10/11.
    const char* old_lines_raw = std::getenv("LINES");
    const std::string old_lines = old_lines_raw ? old_lines_raw : "";
    A::tool_output::Entry long_entry;
    long_entry.name = "shell";
    long_entry.title = "Bash";
    long_entry.detail = "long output";
    long_entry.trailing = "ok · 20 rows";
    for (int i = 0; i < 20; ++i)
        long_entry.output += "output row " + std::to_string(i) + "\n";
    long_entry.call = make_tool("shell",
        A::ToolUse::Done{{}, {}, long_entry.output});
    A::Model short_terminal_model;
    short_terminal_model.ui.panel =
        A::ui::panel::ToolOutput{{{std::move(long_entry)}, 0, true}};
    static constexpr std::array<int, 4> short_heights = {8, 10, 11, 12};
    for (int height : short_heights) {
        const auto height_text = std::to_string(height);
#ifdef _WIN32
        _putenv_s("LINES", height_text.c_str());
#else
        setenv("LINES", height_text.c_str(), 1);
#endif
        auto short_viewer = U::tool_output_panel(short_terminal_model);
        auto short_constrained = maya::dsl::vstack()
            .width(maya::Dimension::fixed(40))(short_viewer);
        const auto short_size = maya::measure_element(short_constrained, 40);
        check(short_size.height.value <= height,
              "long body fits a " + height_text + "-row terminal");
    }

    std::vector<A::tool_output::Entry> short_list_entries;
    for (int i = 0; i < 4; ++i) {
        A::tool_output::Entry entry;
        entry.name = "shell";
        entry.title = "Diagnostics";
        entry.detail = "operation " + std::to_string(i);
        entry.trailing = "ok";
        entry.call = make_tool("shell", A::ToolUse::Done{});
        short_list_entries.push_back(std::move(entry));
    }
    A::Model short_list_model;
    short_list_model.ui.panel =
        A::ui::panel::ToolOutput{{std::move(short_list_entries), 0, false}};
    for (int height : short_heights) {
        const auto height_text = std::to_string(height);
#ifdef _WIN32
        _putenv_s("LINES", height_text.c_str());
#else
        setenv("LINES", height_text.c_str(), 1);
#endif
        auto short_list = U::tool_output_panel(short_list_model);
        auto constrained = maya::dsl::vstack()
            .width(maya::Dimension::fixed(40))(short_list);
        const auto size = maya::measure_element(constrained, 40);
        check(size.height.value <= height,
              "tool output list fits a " + height_text + "-row terminal");
    }
#ifdef _WIN32
    _putenv_s("LINES", old_lines.c_str());
#else
    if (old_lines_raw) setenv("LINES", old_lines.c_str(), 1);
    else unsetenv("LINES");
#endif
}

// A settled `edit` renders its ```diff fence as a GitDiff review card. Two
// pathological cases must be handled so the card stays a clean review surface:
//   1. The edit tool truncates its OUTPUT to the model's char budget and
//      appends a "[... N chars elided — output exceeded tool's budget; refine
//      your request ...]" marker. That marker addresses the MODEL; it must
//      never render inside the user's diff card.
//   2. A huge deletion/rewrite must not dump hundreds of red rows unbounded
//      — it windows to the head with maya's elision (show_all=false).
TEST_CASE("edit diff card strips model budget marker + caps huge diffs") {
    // (1) Budget marker leaking into the fence must be stripped.
    {
        std::string out;
        out += "```diff\n";
        out += "@@ -1,3 +1,3 @@\n";
        out += "-old line\n";
        out += "+new line\n";
        out += " context\n";
        out += "\n[... 4096 chars elided — output exceeded tool's budget; "
               "refine your request to see more ...]\n";
        out += "```";
        auto tc = make_tool("edit", A::ToolUse::Done{{}, {}, out},
                            {{"path", "x.cpp"}});
        const auto body = U::tool_body_preview_config(tc);
        check(body.kind == Kind::GitDiff, "settled edit is a GitDiff card");
        check(body.text.find("chars elided") == std::string::npos
                  && body.text.find("refine your request") == std::string::npos,
              "the model-facing budget marker is stripped from the diff card");
        check(body.text.find("new line") != std::string::npos,
              "the actual diff content is preserved");
    }

    // (2) A normal small diff renders in full (show_all).
    {
        std::string out = "```diff\n@@ -1 +1 @@\n-a\n+b\n```";
        auto tc = make_tool("edit", A::ToolUse::Done{{}, {}, out},
                            {{"path", "x.cpp"}});
        const auto body = U::tool_body_preview_config(tc);
        check(body.kind == Kind::GitDiff && body.show_all,
              "a small diff still renders in full");
    }

    // (3) A very tall diff is windowed to the head, not dumped whole.
    {
        std::string out = "```diff\n@@ -1,400 +1,0 @@\n";
        for (int i = 0; i < 400; ++i)
            out += "-deleted line " + std::to_string(i) + "\n";
        out += "```";
        auto tc = make_tool("edit", A::ToolUse::Done{{}, {}, out},
                            {{"path", "big.cpp"}});
        const auto body = U::tool_body_preview_config(tc);
        check(body.kind == Kind::GitDiff, "huge edit is still a GitDiff card");
        check(!body.show_all,
              "a >200-line diff windows via elision instead of show_all");
        check(body.code_head > 0 && body.code_head <= 200,
              "the windowed diff keeps a bounded head");
    }
}

TEST_CASE("tool_display_name always capitalises") {
    using agentty::ui::tool_display_name;
    // Mapped built-ins keep their curated brand labels.
    check(tool_display_name("shell") == "Shell", "shell -> Shell");
    check(tool_display_name("read")  == "Read",  "read -> Read");
    check(tool_display_name("git_diff") == "Git Diff", "git_diff -> Git Diff");

    // UNMAPPED names (MCP tools, a raw model name) must still Title-Case — no
    // card ever renders a bare lowercase token. This is the fix for `Bash` /
    // any tool showing uncapitalised.
    check(tool_display_name("bash") == "Bash", "bash -> Bash (fallback)");
    check(tool_display_name("Bash") == "Bash", "Bash stays Bash");
    check(tool_display_name("github_create_issue") == "Github Create Issue",
          "snake_case MCP tool -> Title Case words");
    check(tool_display_name("do-thing") == "Do Thing", "dash-separated -> words");
    check(!tool_display_name("x").empty() && tool_display_name("x") == "X",
          "single char capitalises");
}
