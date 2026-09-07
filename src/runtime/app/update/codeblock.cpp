// codeblock_update — reducer for the Ctrl+G code-block picker (the "run
// what the AI just suggested" flow). Open scans the newest assistant
// message for fenced blocks; Select suspends the TUI and runs the chosen
// block INTERACTIVELY on the real terminal — sudo password prompts work,
// output streams live — while a tee captures every output byte;
// RunFinished opens the Result card where the user decides what happens
// to the captured copy: attach to the composer as an Output chip, copy
// it clean, or discard.
//
// Deliberate scope decisions:
//   • Interactive-first: the run happens under maya's Cmd::suspend — the
//     TUI tears down to a cooked tty, the child inherits the REAL
//     terminal for stdin (sudo reads the password from /dev/tty, which
//     stays the real tty regardless of our stdout pipe), and stdout+
//     stderr flow through a tee pipe: every byte hits the user's screen
//     live AND lands in the capture buffer. Ctrl+C uses classic
//     system() semantics — the parent ignores SIGINT/SIGQUIT for the
//     duration, the child takes the default action and dies; agentty
//     resumes cleanly.
//   • The captured output only reaches the COMPOSER when the user
//     presses `a` on the Result card — it lands as an Output attachment
//     (chip), not a conversation message: the same collapse/expand
//     contract as a big paste. The user annotates and submits when (and
//     if) they want the model to see it.
//   • Only shell-ish blocks are runnable (see is_shell_language). For a
//     python/js block Select shows a toast; Edit / Copy still work.
//   • Opening is gated on an idle session: mid-stream the message list
//     is in flux and the "latest assistant reply" is still growing.
//   • Windows: no fork/tcsetattr — falls back to the non-interactive
//     captured runner (same one the bash tool uses). sudo isn't a
//     Windows concept anyway; the honest degradation.

#include "agentty/runtime/app/update/internal.hpp"

#include <algorithm>
#include <utility>

#if !defined(_WIN32)
    #include <csignal>
    #include <cstdio>
    #include <ctime>
    #include <fcntl.h>
    #include <poll.h>
    #include <sys/ioctl.h>
    #include <sys/wait.h>
    #include <termios.h>
    #include <unistd.h>
#else
    #include <atomic>
    #include <chrono>
    #include <cstdio>
    #include <io.h>
    #include <thread>
#endif

#include <maya/core/overload.hpp>

#include "agentty/runtime/panel/code_blocks.hpp"
#include "agentty/io/clipboard.hpp"
#include "agentty/runtime/win_shell_encode.hpp"
#include "agentty/tool/util/subprocess.hpp"

namespace pn = agentty::ui::panel;

namespace agentty::app::detail {

using maya::overload;
namespace cbp = agentty::code_block_picker;

namespace {

// Newest assistant message that yields at least one fenced block.
// Document order within the message is preserved (block 1 = topmost),
// matching how the user visually indexes the reply on screen.
[[nodiscard]] std::vector<CodeBlock> latest_assistant_blocks(const Model& m) {
    for (auto it = m.d.current.messages.rbegin();
         it != m.d.current.messages.rend(); ++it) {
        if (it->role != Role::Assistant) continue;
        if (it->text.empty()) continue;
        auto blocks = cbp::extract_code_blocks(it->text);
        if (!blocks.empty()) return blocks;
        // Keep walking: an assistant turn with prose but no fences
        // shouldn't mask an earlier reply that HAS runnable blocks
        // (common shape: reply N has the commands, reply N+1 is a
        // short "let me know how it goes" follow-up).
    }
    return {};
}

// Blocks available RIGHT NOW from a still-streaming reply. The live text
// lands in `streaming_text` (settled prose moves to `text` only at turn end),
// so mid-turn we read whichever the live assistant message carries and use
// the CLOSED-only extractor: a block whose ``` fence hasn't arrived yet is
// still being typed and must not be offered (running a half-line is unsafe).
// `saw_open` reports that a block is mid-fence, so the caller can say "still
// streaming" instead of "no blocks".
[[nodiscard]] std::vector<CodeBlock>
streaming_reply_closed_blocks(const Model& m, bool& saw_open) {
    saw_open = false;
    for (auto it = m.d.current.messages.rbegin();
         it != m.d.current.messages.rend(); ++it) {
        if (it->role != Role::Assistant) continue;
        // Prefer the live streaming buffer; fall back to settled text (the
        // finalize may have already moved it while the phase lag clears).
        std::string_view live = !it->streaming_text.empty()
                                    ? std::string_view{it->streaming_text}
                                    : std::string_view{it->text};
        if (live.empty()) continue;
        bool open_here = false;
        auto blocks = cbp::extract_closed_code_blocks(live, &open_here);
        saw_open = saw_open || open_here;
        if (!blocks.empty()) return blocks;
    }
    return {};
}
// the real terminal with a stdout/stderr tee — fully interactive, output
// captured. Windows: non-interactive captured runner (subprocess), same
// as the bash tool.
#if !defined(_WIN32)

// Run `command` via /bin/sh -c on the REAL terminal (we're inside maya's
// suspend: cooked tty, TUI escapes torn down). stdin is inherited — the
// actual tty — so line editing / password reads behave; sudo talks to
// /dev/tty directly either way. stdout+stderr are dup2'd onto a pipe the
// parent tees: each read chunk is written straight to the tty (live
// output) and appended to the capture buffer. Returns the finished Msg.
// Small helpers so the transcript looks the same whether or not stdout is
// a real terminal. Colour/heartbeat are TTY-only (piped output stays clean
// for `agentty ... | tee`), but the command echo + status always print.
namespace runner_ui {
    inline bool tty() { return ::isatty(STDOUT_FILENO) == 1; }
    inline const char* dim()   { return tty() ? "\x1b[2m"  : ""; }
    inline const char* bold()  { return tty() ? "\x1b[1m"  : ""; }
    inline const char* green() { return tty() ? "\x1b[32m" : ""; }
    inline const char* red()   { return tty() ? "\x1b[31m" : ""; }
    inline const char* yellow(){ return tty() ? "\x1b[33m" : ""; }
    inline const char* cyan()  { return tty() ? "\x1b[36m" : ""; }
    inline const char* reset() { return tty() ? "\x1b[0m"  : ""; }
    inline void emit(const std::string& s) {
        (void)!::write(STDOUT_FILENO, s.data(), s.size());
    }
    // Clear the current line (used to wipe an in-place heartbeat before
    // real output or the final status lands on top of it).
    inline void clear_line() { if (tty()) emit("\r\x1b[2K"); }
    // Set / restore the terminal window title (OSC 2). This is an
    // ALWAYS-ON elapsed-time readout that lives in the titlebar, so it
    // updates every second even while output is streaming without ever
    // touching the scrollback area. No-op off a TTY.
    inline void set_title(const std::string& t) {
        if (tty()) emit("\x1b]2;" + t + "\x07");
    }
    inline void restore_title() { if (tty()) emit("\x1b]2;\x07"); }

    // ── Pinned bottom-line status (DECSTBM) ──────────────────────────────
    // A universal fallback for terminals/multiplexers that drop the OSC-2
    // window title: reserve the bottom screen row as a fixed status line so
    // the elapsed-time readout is ALWAYS visible even while output streams.
    // Output written by the tee loop scrolls within rows 1..h-1 (the
    // DECSTBM margin); we repaint row h out-of-band. Every escape here is
    // TTY-gated and fully torn down by end_status(), so piped output and
    // the captured buffer never see any of it.
    inline int term_rows() {
        struct winsize ws{};
        if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 2)
            return ws.ws_row;
        return 0;   // unknown / too small — caller disables the status line
    }
    // Enter status mode: set the scroll region to rows 1..(rows-1), leaving
    // row `rows` reserved. Cursor is parked at the top of the scroll region.
    inline void begin_status(int rows) {
        if (!tty() || rows < 3) return;
        std::string s;
        s += "\x1b[1;" + std::to_string(rows - 1) + "r";  // DECSTBM margin
        s += "\x1b[" + std::to_string(rows - 1) + ";1H";  // park cursor
        emit(s);
    }
    // Repaint the reserved bottom row with `text` (already styled), then
    // return the cursor into the scroll region so output continues above.
    inline void paint_status(int rows, const std::string& text) {
        if (!tty() || rows < 3) return;
        std::string s;
        s += "\x1b[s";                                    // save cursor
        s += "\x1b[" + std::to_string(rows) + ";1H";      // to status row
        s += "\x1b[2K";                                   // clear it
        s += text;
        s += "\x1b[u";                                    // restore cursor
        emit(s);
    }
    // Leave status mode: reset the scroll region to full screen and clear
    // the reserved row so the transcript ends clean.
    inline void end_status(int rows) {
        if (!tty() || rows < 3) return;
        std::string s;
        s += "\x1b[r";                                    // reset DECSTBM
        s += "\x1b[" + std::to_string(rows) + ";1H";
        s += "\x1b[2K";
        s += "\x1b[" + std::to_string(rows - 1) + ";1H";
        emit(s);
    }
    // Hand the child a clean screen. maya's inline-mode suspend only emits
    // "\r\n" on teardown — it moves the cursor onto a fresh line but leaves
    // the whole last TUI frame (transcript, composer, picker) still painted
    // ABOVE it. The run header + child output then scroll up INTO that stale
    // frame and overlap it ("text already there that overlaps when you
    // type"). Clear the visible viewport and home the cursor so the child
    // starts on a blank screen.
    //
    // ED2 (\x1b[2J) clears the VIEWPORT only; scrollback is untouched (that
    // needs ED3 / \x1b[3J, which we deliberately do NOT send) so the
    // conversation history the user scrolled through stays in native
    // scrollback. TTY-gated, so piped/captured output never sees it.
    inline void reset_screen() {
        if (!tty()) return;
        emit("\x1b[2J\x1b[H");   // ED2 clear viewport + cursor home
    }
}

[[nodiscard]] CodeBlockRunFinished run_on_real_tty(const std::string& command) {
    namespace ui = runner_ui;
    CodeBlockRunFinished fin;
    fin.command = command;

    // Hand the child a CLEAN screen. maya's inline suspend leaves the last
    // TUI frame painted above the cursor; without this the header and child
    // output scroll up into it and overlap. reset_screen() clears the
    // viewport (scrollback preserved) and homes the cursor.
    ui::reset_screen();

    // Framed run header: a clearly-delimited banner so the user can see at
    // a glance WHERE the run started (the TUI just tore down, so a bare
    // "$ cmd" line was easy to lose) and, crucially, that Ctrl-C stops it.
    // The command still reads like a shell prompt so the transcript is
    // copy-paste-faithful.
    {
        std::string header;
        header += ui::dim();
        // No leading \n — reset_screen() already homed the cursor to row 1.
        header += "\xe2\x95\xad\xe2\x94\x80 running \xe2\x94\x80 Ctrl-C to stop \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80";
        header += ui::reset();
        header += "\n";
        header += ui::cyan();
        header += "$ ";
        header += ui::reset();
        header += ui::bold();
        header += command;
        header += ui::reset();
        header += "\n";
        ui::emit(header);
    }

    int fds[2];
    if (::pipe(fds) != 0) {
        fin.output    = "[failed to start: pipe() failed]";
        fin.exit_code = -1;
        return fin;
    }
    // CLOEXEC both ends: another thread's fork/exec (MCP server spawn, tool
    // subprocess) must not inherit this capture pipe — a long-lived sibling
    // child pinning fds[1] open would starve the read loop of EOF forever.
    // Our own child's dup2 onto stdout below clears the flag on the duplicate.
    (void)::fcntl(fds[0], F_SETFD, ::fcntl(fds[0], F_GETFD) | FD_CLOEXEC);
    (void)::fcntl(fds[1], F_SETFD, ::fcntl(fds[1], F_GETFD) | FD_CLOEXEC);

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]); ::close(fds[1]);
        fin.output    = "[failed to start: fork() failed]";
        fin.exit_code = -1;
        return fin;
    }

    if (pid == 0) {
        // ── Child ── default signal dispositions (the parent ignores
        // SIGINT/SIGQUIT below; we must NOT inherit that or Ctrl+C
        // couldn't stop the command).
        ::signal(SIGINT,  SIG_DFL);
        ::signal(SIGQUIT, SIG_DFL);
        ::close(fds[0]);
        ::dup2(fds[1], STDOUT_FILENO);
        ::dup2(fds[1], STDERR_FILENO);
        ::close(fds[1]);
        ::execl("/bin/sh", "sh", "-c", command.c_str(), (char*)nullptr);
        _exit(127);
    }

    // ── Parent ── classic system() semantics: ignore INT/QUIT while the
    // child runs so Ctrl+C kills only the command, not agentty.
    struct sigaction ign{}, old_int{}, old_quit{};
    ign.sa_handler = SIG_IGN;
    ::sigaction(SIGINT,  &ign, &old_int);
    ::sigaction(SIGQUIT, &ign, &old_quit);

    ::close(fds[1]);
    // Tee loop: live to the tty, captured to the buffer. Bounded capture
    // (2 MB) so a runaway command can't OOM the composer — the SCREEN
    // still shows everything; only the buffer stops growing.
    //
    // We poll(2) with a 1s timeout instead of a bare blocking read so a
    // SILENT command (npm install fetching, a network wait, a `sleep`)
    // still shows a heartbeat — "⠿ running… 12s (Ctrl-C to stop)" redrawn
    // in place — instead of a frozen-looking screen. The heartbeat is
    // TTY-only and is wiped the instant real output or the exit status
    // arrives, so it never pollutes the captured buffer or piped output.
    constexpr std::size_t kCaptureMax = 2u * 1024 * 1024;
    bool truncated = false;
    char buf[8192];
    const auto started = ::time(nullptr);
    long last_status_secs = -1;
    int  spin = 0;
    static constexpr const char* kSpin[] = {"⣷","⣯","⣟","⡿","⣾","⣽","⣻","⣷"};
    // Short command label for the timers (first token, clipped).
    std::string label = command.substr(0, command.find_first_of(" \t\n"));
    if (label.size() > 24) label.resize(24);

    // Two always-on elapsed readouts, belt-and-suspenders so SOMETHING is
    // visible regardless of terminal quirks:
    //   1. OSC-2 window title — non-intrusive, but some multiplexers drop it.
    //   2. A DECSTBM-pinned bottom status row — universal; output scrolls
    //      above it. Enabled only when we can read the terminal height.
    const int rows = ui::term_rows();
    ui::begin_status(rows);
    auto tick = [&] {
        const long secs = static_cast<long>(::time(nullptr) - started);
        if (secs == last_status_secs) return;
        last_status_secs = secs;
        ui::set_title("● " + std::to_string(secs) + "s — " + label + " — agentty");
        if (rows >= 3) {
            std::string bar = ui::dim();
            bar += kSpin[spin++ % 8];
            bar += " running… " + std::to_string(secs) + "s · " + label
                 + " · Ctrl-C to stop";
            bar += ui::reset();
            ui::paint_status(rows, bar);
        }
    };
    tick();
    for (;;) {
        struct pollfd pfd{fds[0], POLLIN, 0};
        const int pr = ::poll(&pfd, 1, 1000);
        tick();   // refresh timers whether or not output arrived this tick
        if (pr == 0) continue;
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        const ssize_t n = ::read(fds[0], buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        (void)!::write(STDOUT_FILENO, buf, static_cast<std::size_t>(n));
        if (fin.output.size() < kCaptureMax) {
            const auto room = kCaptureMax - fin.output.size();
            fin.output.append(buf, std::min(static_cast<std::size_t>(n), room));
            if (static_cast<std::size_t>(n) > room) truncated = true;
        } else {
            truncated = true;
        }
    }
    ui::end_status(rows);
    ui::restore_title();
    ::close(fds[0]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    ::sigaction(SIGINT,  &old_int,  nullptr);
    ::sigaction(SIGQUIT, &old_quit, nullptr);

    // Distinguish a user interrupt (Ctrl-C / Ctrl-\) from a genuine
    // command failure — "failed exit 130" reads like the command broke
    // when the user actually stopped it on purpose.
    bool interrupted = false;
    if (WIFEXITED(status)) {
        fin.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        const int sig = WTERMSIG(status);
        interrupted = (sig == SIGINT || sig == SIGQUIT);
        fin.exit_code = 128 + sig;
    } else {
        fin.exit_code = -1;
    }
    if (truncated) fin.output += "\n[capture truncated at 2 MB — full output was shown on screen]";

    // Completion footer: a colour-coded status line so success/failure/
    // interrupt is unmistakable at a glance, with elapsed time.
    {
        const long secs = static_cast<long>(::time(nullptr) - started);
        const bool ok = (fin.exit_code == 0);
        std::string tail = "\n";
        if (interrupted)   { tail += ui::yellow(); tail += "╰─ ■ stopped"; }
        else if (ok)       { tail += ui::green();  tail += "╰─ ✓ done"; }
        else               { tail += ui::red();    tail += "╰─ ✗ failed"; }
        tail += ui::reset();
        tail += ui::dim();
        tail += "  exit " + std::to_string(fin.exit_code)
              + "  ·  " + std::to_string(secs) + "s";
        tail += ui::reset();
        tail += "\n";
        ui::emit(tail);
    }

    // Keypress-to-continue: hold the transcript on screen until the user
    // acknowledges, so a fast command's output isn't repainted away before
    // it can be read. Only when stdout AND stdin are a real terminal (so
    // we can actually read a key); piped/redirected runs skip it. The
    // prompt is on its own line and cleared afterward so the restored TUI
    // starts on a clean row. Any key continues.
    if (ui::tty() && ::isatty(STDIN_FILENO) == 1) {
        std::string prompt = ui::dim();
        prompt += "   press any key to return to agentty…";
        prompt += ui::reset();
        ui::emit(prompt);
        // Best-effort raw single-key read: disable canonical mode + echo
        // so a lone keypress (no Enter) continues, then restore.
        struct termios oldt{}, raw{};
        const bool have_termios = (::tcgetattr(STDIN_FILENO, &oldt) == 0);
        if (have_termios) {
            raw = oldt;
            raw.c_lflag &= ~(ICANON | ECHO);
            raw.c_cc[VMIN]  = 1;
            raw.c_cc[VTIME] = 0;
            ::tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        }
        char c;
        while (::read(STDIN_FILENO, &c, 1) < 0 && errno == EINTR) {}
        if (have_termios) ::tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        ui::clear_line();
    }
    return fin;
}

[[nodiscard]] maya::Cmd<Msg> run_block_cmd(std::string command, cbp::BlockShell /*shell*/) {
    return maya::Cmd<Msg>::suspend(
        [cmd = std::move(command)]() -> Msg {
            return Msg{run_on_real_tty(cmd)};
        });
}

#else  // _WIN32 — non-interactive fallback via the shared subprocess runner

// Wrap the block body for the chosen Windows interpreter. cmd.exe is the
// default shell of run_command_s (it wraps in `cmd.exe /S /C "..."`), so
// a Cmd block passes through verbatim. A PowerShell block is handed to
// powershell -NoProfile -Command; we base64-encode it via -EncodedCommand
// so arbitrary quoting/newlines survive the cmd.exe wrapper intact
// (nested quotes through cmd /C are the classic breakage). Multi-line
// scripts Just Work because the whole body is one encoded argument.
[[nodiscard]] std::string wrap_for_windows_shell(cbp::BlockShell shell,
                                                 const std::string& body) {
    if (shell != cbp::BlockShell::PowerShell) return body;  // Cmd: verbatim
    // PowerShell -EncodedCommand contract (base64 of UTF-16LE) lives in a
    // shared, platform-independent header so it can be verified off Windows
    // — the header carries a compile-time known-answer static_assert block
    // that fails THIS build if the transform ever regresses.
    return win_shell::powershell_command(body);
}

[[nodiscard]] maya::Cmd<Msg> run_block_cmd(std::string command, cbp::BlockShell shell) {
    return maya::Cmd<Msg>::task_isolated(
        [cmd = std::move(command), shell](std::function<void(Msg)> dispatch) {
            const std::string wrapped = wrap_for_windows_shell(shell, cmd);

            // Windows parity for "what's happening while it runs": the
            // shared subprocess runner (run_command_s) is blocking and
            // non-streaming — output only appears in the Result card at the
            // end — so we can't tee live. But we CAN show the run is alive:
            // print a header, spin a background thread that ticks an
            // elapsed-time heartbeat (OSC-2 title + an in-place line;
            // modern Windows Terminal / conhost with VT processing render
            // both), run the command, then a footer. All decoration is
            // console-only and never enters the captured buffer.
            const bool con = (::_isatty(_fileno(stdout)) != 0);
            auto out = [](const std::string& s) {
                std::fputs(s.c_str(), stdout); std::fflush(stdout);
            };
            if (con) {
                out("\x1b[2m\n╭─ running ─ (output shown when it finishes) ─────\x1b[0m\n"
                    "\x1b[36m$ \x1b[0m\x1b[1m" + cmd + "\x1b[0m\n");
            }

            std::string label = cmd.substr(0, cmd.find_first_of(" \t\n"));
            if (label.size() > 24) label.resize(24);
            std::atomic<bool> done_flag{false};
            std::thread ticker;
            if (con) {
                ticker = std::thread([&done_flag, label] {
                    static constexpr const char* kSpin[] =
                        {"⣷","⣯","⣟","⡿","⣾","⣽","⣻","⣷"};
                    int spin = 0;
                    const auto start = std::chrono::steady_clock::now();
                    while (!done_flag.load(std::memory_order_relaxed)) {
                        const long secs = static_cast<long>(
                            std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::steady_clock::now() - start).count());
                        std::string s = "\x1b]2;● " + std::to_string(secs)
                                      + "s — " + label + " — agentty\x07";
                        s += "\r\x1b[2K\x1b[2m" + std::string(kSpin[spin++ % 8])
                           + " running… " + std::to_string(secs) + "s\x1b[0m";
                        std::fputs(s.c_str(), stdout); std::fflush(stdout);
                        std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    }
                });
            }

            const auto start = std::chrono::steady_clock::now();
            auto r = tools::util::run_command_s(wrapped);
            const long secs = static_cast<long>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - start).count());

            done_flag.store(true, std::memory_order_relaxed);
            if (ticker.joinable()) ticker.join();

            CodeBlockRunFinished fin;
            fin.command   = cmd;   // show the ORIGINAL body in the card
            fin.timed_out = r.timed_out;
            if (!r.started) {
                fin.output    = "[failed to start: " + r.start_error + "]";
                fin.exit_code = -1;
            } else {
                fin.output    = std::move(r.output);
                fin.exit_code = r.exit_code;
                if (r.truncated) fin.output += "\n[output truncated]";
            }

            if (con) {
                const bool ok = (fin.exit_code == 0 && !r.timed_out);
                std::string tail = "\r\x1b[2K\x1b]2;\x07";   // wipe hb + restore title
                tail += ok ? "\x1b[32m╰─ ✓ done"
                     : r.timed_out ? "\x1b[33m╰─ ■ timed out"
                     : "\x1b[31m╰─ ✗ failed";
                tail += "\x1b[0m\x1b[2m  exit " + std::to_string(fin.exit_code)
                      + "  ·  " + std::to_string(secs) + "s\x1b[0m\n";
                out(tail);
            }

            dispatch(Msg{std::move(fin)});
        });
}

#endif

} // namespace

Step codeblock_update(Model m, msg::CodeBlockMsg cm) {
    return std::visit(overload{
        [&](OpenCodeBlockPicker) -> Step {
            // Mid-stream is allowed: the network stream runs on a background
            // worker (StreamDelta posts back to the UI thread), so suspending
            // the TUI to run a block does NOT pause the reply — deltas keep
            // arriving and land when you return. While streaming we offer the
            // blocks whose ``` fence has ALREADY closed in the live reply; a
            // block still being typed is withheld until its fence lands.
            std::vector<CodeBlock> blocks;
            if (m.s.active()) {
                bool saw_open = false;
                blocks = streaming_reply_closed_blocks(m, saw_open);
                if (blocks.empty()) {
                    auto cmd = set_status_toast(m,
                        saw_open
                          ? "a code block is still streaming \xe2\x80\x94 try again in a moment"
                          : "no complete code blocks yet");
                    return {std::move(m), std::move(cmd)};
                }
            } else {
                blocks = latest_assistant_blocks(m);
                if (blocks.empty()) {
                    auto cmd = set_status_toast(m,
                        "no code blocks in the last reply");
                    return {std::move(m), std::move(cmd)};
                }
            }
            m.ui.panel = pn::CodeBlocks{{std::move(blocks), 0}};
            m.ui.code_blocks_scroll.y = 0;
            return done(std::move(m));
        },
        [&](CloseCodeBlockPicker) -> Step {
            m.ui.panel.close<pn::CodeBlocks>(); m.ui.panel.close<pn::CodeBlockResult>();
            return done(std::move(m));
        },
        [&](CodeBlockPickerMove& e) -> Step {
            if (auto* o = m.ui.panel.get<pn::CodeBlocks>()) {
                int sz = static_cast<int>(o->blocks.size());
                if (sz <= 0) return done(std::move(m));
                o->index = std::clamp(o->index + e.delta, 0, sz - 1);
                return done(std::move(m));
            }
            if (m.ui.panel.get<pn::CodeBlockResult>()) {
                // Read-only result card: Move deltas scroll the capture
                // viewport directly. max_y is paint-written-back by the
                // Picker widget; clamp against it (0 before first paint
                // — harmless, the writeback lands next frame).
                auto& sc = m.ui.code_blocks_scroll;
                sc.y = std::clamp(sc.y + e.delta, 0, std::max(0, sc.max_y));
            }
            return done(std::move(m));
        },
        [&](CodeBlockPickerSelect& e) -> Step {
            auto* o = m.ui.panel.get<pn::CodeBlocks>();
            if (!o) return done(std::move(m));
            const int idx = e.index.value_or(o->index);
            if (idx < 0 || idx >= static_cast<int>(o->blocks.size()))
                return done(std::move(m));
            CodeBlock block = o->blocks[static_cast<std::size_t>(idx)];
            const cbp::BlockShell shell = cbp::shell_for_language(block.language);
            if (shell == cbp::BlockShell::None) {
                // Not runnable on this platform — nudge toward the actions
                // that DO make sense for this block. Picker stays open so
                // `e` / `y` are one keystroke away.
                const std::string tag = block.language.empty()
                    ? std::string{"this"} : "'" + block.language + "'";
                auto cmd = set_status_toast(m,
                    tag + " block isn't runnable here — "
                    "press e to edit or y to copy");
                return {std::move(m), std::move(cmd)};
            }
            m.ui.panel.close<pn::CodeBlocks>(); m.ui.panel.close<pn::CodeBlockResult>();
            return {std::move(m), run_block_cmd(std::move(block.body), shell)};
        },
        [&](CodeBlockPickerEdit) -> Step {
            auto* o = m.ui.panel.get<pn::CodeBlocks>();
            if (!o) return done(std::move(m));
            const int idx = o->index;
            if (idx < 0 || idx >= static_cast<int>(o->blocks.size()))
                return done(std::move(m));
            std::string body = o->blocks[static_cast<std::size_t>(idx)].body;
            m.ui.panel.close<pn::CodeBlocks>(); m.ui.panel.close<pn::CodeBlockResult>();
            // Splice at the cursor rather than replacing — same
            // convention as the @file / #symbol chips. The common case
            // is an empty composer, where this IS a replace.
            m.ui.composer.text.insert(
                static_cast<std::size_t>(m.ui.composer.cursor), body);
            m.ui.composer.cursor += static_cast<int>(body.size());
            return done(std::move(m));
        },
        [&](CodeBlockPickerCopy) -> Step {
            auto* o = m.ui.panel.get<pn::CodeBlocks>();
            if (!o) return done(std::move(m));
            const int idx = o->index;
            if (idx < 0 || idx >= static_cast<int>(o->blocks.size()))
                return done(std::move(m));
            std::string body = o->blocks[static_cast<std::size_t>(idx)].body;
            m.ui.panel.close<pn::CodeBlocks>(); m.ui.panel.close<pn::CodeBlockResult>();
            // Write via native tooling (pbcopy/wl-copy/xclip) synchronously
            // AND emit the OSC 52 Cmd. OSC 52 is opt-in in Terminal.app /
            // iTerm2 so it silently no-ops there; the native write is what
            // actually lands the copy locally. OSC 52 still carries it across
            // an SSH/tmux hop to a remote clipboard.
            (void)write_clipboard_text(body);
            auto toast = set_status_toast(m, "copied clean block to clipboard");
            return {std::move(m),
                    maya::Cmd<Msg>::batch(
                        maya::Cmd<Msg>::write_clipboard(std::move(body)),
                        std::move(toast))};
        },
        [&](CodeBlockRunFinished& e) -> Step {
            // Don't auto-stage — open the RESULT card instead. The user
            // already watched the output live on the real terminal; this
            // is the decision beat: attach the captured copy to the
            // composer, copy it clean, or discard. The composer only
            // ever receives output the user explicitly asked for.
            m.ui.panel = pn::CodeBlockResult{{
                std::move(e.command), std::move(e.output),
                e.exit_code, e.timed_out}};
            m.ui.code_blocks_scroll.y = 0;
            return done(std::move(m));
        },
        [&](CodeBlockResultAttach) -> Step {
            auto* r = m.ui.panel.get<pn::CodeBlockResult>();
            if (!r) return done(std::move(m));
            // Fold the captured output into the composer as an Output
            // attachment — the SAME collapse-to-chip / expand-on-submit
            // machinery a big paste uses. However huge the log is, the
            // composer shows one pill ("Output: sudo mkfs… · 1240 lines
            // · 48 KB"); the whole body only materialises on the wire
            // when the user actually submits.
            std::string out = std::move(r->output);
            std::size_t lines = out.empty() ? 0 : 1;
            for (char c : out) if (c == '\n') ++lines;

            Attachment att;
            att.kind       = Attachment::Kind::Output;
            att.name       = std::move(r->command);   // chip caption
            att.line_count = lines;
            att.byte_count = out.size();
            att.body       = std::move(out);
            std::size_t idx = m.ui.composer.attachments.size();
            m.ui.composer.attachments.push_back(std::move(att));
            auto placeholder = attachment::make_placeholder(idx);
            m.ui.composer.text.insert(
                static_cast<std::size_t>(m.ui.composer.cursor), placeholder);
            m.ui.composer.cursor += static_cast<int>(placeholder.size());
            m.ui.composer.expanded = true;
            m.ui.panel.close<pn::CodeBlocks>(); m.ui.panel.close<pn::CodeBlockResult>();
            auto toast = set_status_toast(m, "output attached to composer");
            return {std::move(m), std::move(toast)};
        },
        [&](CodeBlockResultCopy) -> Step {
            auto* r = m.ui.panel.get<pn::CodeBlockResult>();
            if (!r) return done(std::move(m));
            std::string body = std::move(r->output);
            m.ui.panel.close<pn::CodeBlocks>(); m.ui.panel.close<pn::CodeBlockResult>();
            (void)write_clipboard_text(body);   // native pbcopy/wl-copy/xclip
            auto toast = set_status_toast(m, "output copied to clipboard");
            return {std::move(m),
                    maya::Cmd<Msg>::batch(
                        maya::Cmd<Msg>::write_clipboard(std::move(body)),
                        std::move(toast))};
        },
        [&](CodeBlockResultDiscard) -> Step {
            m.ui.panel.close<pn::CodeBlocks>(); m.ui.panel.close<pn::CodeBlockResult>();
            return done(std::move(m));
        },
    }, cm);
}

} // namespace agentty::app::detail
