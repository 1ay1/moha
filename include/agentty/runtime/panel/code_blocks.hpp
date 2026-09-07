#pragma once
// Code-block picker — "run the thing the AI just told me to run" without
// the copy-paste-then-clean dance.
//
// Ctrl+G scans the newest assistant reply for fenced ``` blocks and pops
// a compact picker above the composer: one row per block (language +
// first-line preview). From there:
//
//   Enter / 1-9  → RUN the block locally, interactively, on the REAL
//                  terminal (maya suspend + /bin/sh -c + stdout/stderr
//                  tee). sudo password prompts work; output streams
//                  live. When it exits, the Result card opens with the
//                  captured copy: attach to composer / copy / discard.
//   e            → stage the CLEANED body into the composer to edit first.
//   y            → copy the cleaned body to the clipboard.
//
// "Cleaned" means: fences stripped, and — when the block is a pasted
// terminal transcript where EVERY command line carries a `$ ` prompt —
// the prompt markers dropped too. The all-lines rule matters: `#` starts
// a comment in real shell scripts, so blindly stripping `# `/`$ ` from
// individual lines would corrupt legitimate scripts. Only the uniform
// transcript pattern is unambiguous.
//
// Running is restricted to blocks the current platform has an
// interpreter for: POSIX runs sh/bash/zsh/shell/console/terminal (and
// bare fences) through /bin/sh; Windows runs cmd/bat and bare fences
// through cmd.exe and powershell/pwsh/ps1 through PowerShell. A block in
// a language this platform can't run (e.g. `powershell` on Linux, or a
// `python`/`js` snippet anywhere) still offers edit/copy, but Run shows
// a toast instead of executing garbage. See shell_for_language().
//
// This header is UI-state + pure extraction only. Reducer wiring lives in
// update/codeblock.cpp, key dispatch in subscribe.cpp, the view in
// view/pickers.cpp — the same file split as the other pickers.

#include <cstddef>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace agentty {

// One extracted fenced block. `language` is the fence info string
// (lower-cased, first word only — "sh", "python", "" for a bare fence).
// `body` is already cleaned (see above) and ends without a trailing
// newline so composer insertion / clipboard writes don't carry a
// dangling blank line.
struct CodeBlock {
    std::string language;
    std::string body;
    int         line_count = 0;
};

namespace code_block_picker {

// Which interpreter should run a given fence language — and, implicitly,
// whether the block is runnable at all on THIS platform. The set is
// platform-aware: a `powershell` block is runnable on Windows but not on
// POSIX (nothing sane to hand it to), and a `bash` block is runnable on
// POSIX but only opportunistically on Windows (needs a bash on PATH,
// e.g. Git-Bash/WSL — we don't assume it). The picker's Run gate and the
// runner both consult this one enum so they never disagree.
enum class BlockShell {
    None,        // not runnable on this platform
    Posix,       // /bin/sh -c
    Cmd,         // Windows cmd.exe (the default shell wrapper)
    PowerShell,  // powershell -NoProfile -Command
};

// Classify a fence info string (already lower-cased, first word only)
// into the interpreter that should run it on the CURRENT platform.
[[nodiscard]] inline BlockShell shell_for_language(std::string_view lang) noexcept {
    const bool posixish =
        lang == "sh" || lang == "bash" || lang == "zsh" || lang == "shell"
        || lang == "console" || lang == "terminal" || lang == "posix"
        || lang == "shell-session" || lang == "shellsession";
    const bool powershellish =
        lang == "powershell" || lang == "pwsh" || lang == "ps" || lang == "ps1";
    const bool cmdish =
        lang == "cmd" || lang == "bat" || lang == "batch" || lang == "dos"
        || lang == "winbatch" || lang == "cmd.exe";

#if defined(_WIN32)
    // On Windows the *native* shell for a bare fence is cmd.exe. A
    // PowerShell block runs through powershell; an explicit posix block
    // is handed to the default shell wrapper too (cmd.exe) as a best
    // effort — models emitting `bash` on a Windows session usually mean
    // "the shell", and if it's truly bash-only it'll just error, same as
    // any wrong command. That's a run the user explicitly asked for.
    if (powershellish)          return BlockShell::PowerShell;
    if (cmdish)                 return BlockShell::Cmd;
    if (posixish || lang.empty()) return BlockShell::Cmd;
    return BlockShell::None;
#else
    // POSIX: bare fences and posix languages go to /bin/sh. cmd/
    // powershell blocks are Windows-only — nothing to run them with.
    if (posixish || lang.empty()) return BlockShell::Posix;
    (void)powershellish;
    (void)cmdish;
    return BlockShell::None;
#endif
}

// Is this block runnable on the current platform? (Run gate + nudge
// counter both use this — a single source of truth with the runner.)
[[nodiscard]] inline bool is_shell_language(std::string_view lang) noexcept {
    return shell_for_language(lang) != BlockShell::None;
}

// Strip uniform `$ ` / `> ` prompt decoration from a transcript-style
// block. Applied ONLY when every non-empty line starts with one of the
// markers — the safe pattern; mixed lines mean real script content
// (comments, output lines) and are left untouched.
[[nodiscard]] inline std::string strip_prompt_prefixes(std::string body) {
    // First pass: does every non-empty line start with "$ " or "> "?
    bool uniform = true;
    bool any     = false;
    std::size_t pos = 0;
    while (pos <= body.size()) {
        std::size_t eol  = body.find('\n', pos);
        std::size_t len  = (eol == std::string::npos ? body.size() : eol) - pos;
        std::string_view line{body.data() + pos, len};
        if (!line.empty()) {
            any = true;
            if (!(line.size() >= 2
                  && (line[0] == '$' || line[0] == '>')
                  && line[1] == ' ')) {
                uniform = false;
                break;
            }
        }
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    if (!uniform || !any) return body;
    // Second pass: rebuild without the 2-byte marker.
    std::string out;
    out.reserve(body.size());
    pos = 0;
    while (pos <= body.size()) {
        std::size_t eol = body.find('\n', pos);
        std::size_t len = (eol == std::string::npos ? body.size() : eol) - pos;
        std::string_view line{body.data() + pos, len};
        if (!line.empty()) line.remove_prefix(2);
        out.append(line);
        if (eol == std::string::npos) break;
        out.push_back('\n');
        pos = eol + 1;
    }
    return out;
}

// Extract every fenced code block from a markdown text. Tolerates the
// two fence shapes the models actually emit: ``` and ~~~, with optional
// info string. An unterminated fence at EOF still yields a block (the
// stream may have been cut mid-reply; the user can still see + run what
// arrived). Indented fences (up to 3 spaces, the CommonMark limit) are
// recognised so blocks nested under list items aren't missed.
[[nodiscard]] inline std::vector<CodeBlock> extract_code_blocks(std::string_view text) {
    std::vector<CodeBlock> out;
    std::size_t pos = 0;
    bool        in_block = false;
    char        fence_ch = '`';
    std::string lang;
    std::string body;

    auto flush = [&] {
        // Trim trailing newline(s) — the closing fence sits on its own
        // line so the body always ends with one.
        while (!body.empty() && (body.back() == '\n' || body.back() == '\r'))
            body.pop_back();
        if (!body.empty()) {
            CodeBlock b;
            b.language = lang;
            b.body     = strip_prompt_prefixes(std::move(body));
            b.line_count = 1;
            for (char c : b.body) if (c == '\n') ++b.line_count;
            out.push_back(std::move(b));
        }
        body.clear();
        lang.clear();
    };

    while (pos <= text.size()) {
        std::size_t eol = text.find('\n', pos);
        std::size_t len = (eol == std::string_view::npos ? text.size() : eol) - pos;
        std::string_view line = text.substr(pos, len);
        // Strip trailing \r (CRLF-tolerant).
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        // Allow up to 3 leading spaces before the fence (CommonMark).
        std::string_view t = line;
        int indent = 0;
        while (!t.empty() && t.front() == ' ' && indent < 3) { t.remove_prefix(1); ++indent; }

        const bool is_fence = t.size() >= 3
            && (t[0] == '`' || t[0] == '~')
            && t[1] == t[0] && t[2] == t[0];

        if (!in_block) {
            if (is_fence) {
                in_block = true;
                fence_ch = t[0];
                // Info string: first word after the fence run, lower-cased.
                std::size_t i = 3;
                while (i < t.size() && t[i] == fence_ch) ++i;
                while (i < t.size() && t[i] == ' ') ++i;
                std::size_t start = i;
                while (i < t.size() && t[i] != ' ' && t[i] != '\t') ++i;
                lang.assign(t.substr(start, i - start));
                for (char& c : lang)
                    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
            }
        } else {
            // A closing fence must match the opening character and have
            // nothing but fence chars on the line (CommonMark allows
            // trailing spaces; info strings are opening-only).
            bool is_close = is_fence && t[0] == fence_ch;
            if (is_close) {
                std::size_t i = 0;
                while (i < t.size() && t[i] == fence_ch) ++i;
                while (i < t.size() && t[i] == ' ') ++i;
                is_close = (i == t.size());
            }
            if (is_close) {
                in_block = false;
                flush();
            } else {
                body.append(line);
                body.push_back('\n');
            }
        }

        if (eol == std::string_view::npos) break;
        pos = eol + 1;
    }
    // Unterminated fence at EOF — keep what we have.
    if (in_block) flush();
    return out;
}

// Mid-stream-safe extraction: like extract_code_blocks, but DROPS a block
// whose fence hasn't closed yet (the one still being typed at the live edge).
// Running a half-typed command is dangerous — the model may not have finished
// the line — so while a reply is still streaming we only ever offer blocks
// whose closing ``` has already arrived. `complete_out` (optional) receives
// whether the trailing block was still open, so the caller can hint "a block
// is still streaming" vs "no blocks at all".
[[nodiscard]] inline std::vector<CodeBlock>
extract_closed_code_blocks(std::string_view text, bool* had_open = nullptr) {
    // Detect an unterminated trailing fence by re-scanning fence lines: an
    // odd number of fence-delimiter lines (opening without a matching close)
    // means the last opened block never closed.
    auto blocks = extract_code_blocks(text);
    bool open_tail = false;
    {
        std::size_t pos = 0;
        bool in_block = false;
        char fence_ch = '`';
        while (pos <= text.size()) {
            std::size_t eol = text.find('\n', pos);
            std::size_t len =
                (eol == std::string_view::npos ? text.size() : eol) - pos;
            std::string_view line = text.substr(pos, len);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            std::string_view t = line;
            int indent = 0;
            while (!t.empty() && t.front() == ' ' && indent < 3) {
                t.remove_prefix(1); ++indent;
            }
            const bool is_fence = t.size() >= 3
                && (t[0] == '`' || t[0] == '~') && t[1] == t[0] && t[2] == t[0];
            if (!in_block) {
                if (is_fence) { in_block = true; fence_ch = t[0]; }
            } else if (is_fence && t[0] == fence_ch) {
                std::size_t i = 0;
                while (i < t.size() && t[i] == fence_ch) ++i;
                while (i < t.size() && t[i] == ' ') ++i;
                if (i == t.size()) in_block = false;   // valid close
            }
            if (eol == std::string_view::npos) break;
            pos = eol + 1;
        }
        open_tail = in_block;
    }
    if (had_open) *had_open = open_tail;
    // extract_code_blocks appended the still-open block last; drop it.
    if (open_tail && !blocks.empty()) blocks.pop_back();
    return blocks;
}

struct Closed {};

struct Open {
    // Blocks from the newest assistant message that contained any —
    // document order (top of the reply first, so "block 1" reads
    // naturally against what's on screen).
    std::vector<CodeBlock> blocks;
    int                    index = 0;
};

// Post-run decision state. The interactive run finished and the user
// already watched the output live on the real terminal; now they choose
// what happens to the CAPTURED copy: attach to the composer as an
// Output chip, copy it clean to the clipboard, or discard (the on-screen
// transcript stays in native scrollback above the TUI either way).
// Holding the bytes HERE — not auto-staging — is the whole point: the
// composer only ever receives output the user explicitly asked for.
struct Result {
    std::string command;
    std::string output;
    int         exit_code = 0;
    bool        timed_out = false;
};

} // namespace code_block_picker

using CodeBlockPickerState =
    std::variant<code_block_picker::Closed, code_block_picker::Open,
                 code_block_picker::Result>;

[[nodiscard]] inline bool code_block_picker_is_open(const CodeBlockPickerState& s) noexcept {
    return std::holds_alternative<code_block_picker::Open>(s);
}
[[nodiscard]] inline bool code_block_result_is_open(const CodeBlockPickerState& s) noexcept {
    return std::holds_alternative<code_block_picker::Result>(s);
}
[[nodiscard]] inline       code_block_picker::Open* code_block_picker_opened(CodeBlockPickerState& s)       noexcept { return std::get_if<code_block_picker::Open>(&s); }
[[nodiscard]] inline const code_block_picker::Open* code_block_picker_opened(const CodeBlockPickerState& s) noexcept { return std::get_if<code_block_picker::Open>(&s); }
[[nodiscard]] inline       code_block_picker::Result* code_block_result(CodeBlockPickerState& s)       noexcept { return std::get_if<code_block_picker::Result>(&s); }
[[nodiscard]] inline const code_block_picker::Result* code_block_result(const CodeBlockPickerState& s) noexcept { return std::get_if<code_block_picker::Result>(&s); }

} // namespace agentty
