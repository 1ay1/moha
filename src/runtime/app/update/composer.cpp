// composer_update — reducer for `msg::ComposerMsg`. The composer is purely
// local UI state (text, cursor, expanded flag, queued items, attachments,
// undo/redo stacks, history index); arms here don't reach into network /
// streaming / tools. ComposerEnter / ComposerSubmit route through
// detail::submit_message which handles the broader "kick a new turn" flow.

#include "agentty/runtime/app/update.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#if !defined(_WIN32)
#  include <unistd.h>   // getppid (mosh ancestry walk in the clipboard diagnosis)
#endif
#include <string>
#include <string_view>

#include <maya/terminal/tmux.hpp>
#include <maya/terminal/ansi.hpp>   // env_supports_osc5522 — kitty detection
#include <maya/core/overload.hpp>

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/cmd_factory.hpp"   // cmd::refresh_oauth
#include "agentty/auth/auth.hpp"                  // oauth_proactive_refresh_token
#include "agentty/io/clipboard.hpp"
#include "agentty/provider/selection.hpp"   // prewarm_active_provider
#include "agentty/util/home_dir.hpp"
#include "agentty/util/env.hpp"
#include "agentty/util/image_dims.hpp"
#include "agentty/runtime/panel/palette.hpp"
#include "agentty/runtime/composer_attachment.hpp"
#include "agentty/runtime/panel/mention.hpp"
#include "agentty/runtime/panel/symbol.hpp"
#include "agentty/workspace/files.hpp"
#include "agentty/util/isolated_thread.hpp"
#include "agentty/workspace/symbols.hpp"
#include "agentty/runtime/view/helpers.hpp"

namespace pn = agentty::ui::panel;

namespace agentty::app::detail {

namespace {

namespace fs = std::filesystem;

constexpr std::size_t kUndoDepth = 64;

// Snapshot the current composer payload into the undo stack and clear
// the redo stack. attachments are append-only at the runtime level
// (backspace over a chip removes the placeholder bytes from `text`
// but leaves the Attachment object in the vector — orphans are GC'd
// on composer clear), so storing only the vector's *size* at snapshot
// time is enough to restore on undo.
//
// `coalesce`: when true (a run of ordinary self-inserting typing) and
// the previous op was ALSO a coalescible typing edit, we skip pushing
// a fresh snapshot so one Ctrl+Z rewinds the whole word/run rather
// than a single character. A non-coalescing op (paste, delete, chip
// insert, cursor jump, submit) always breaks the run so structural
// edits stay individually undoable — standard editor semantics.
void push_undo(ComposerState& cs, bool coalesce = false) {
    if (coalesce && cs.undo_coalescing && !cs.undo_stack.empty()) {
        // Continue the current typing run: the snapshot already on top
        // of the stack is the pre-run state we want to restore to.
        // Just keep the redo stack cleared (this is still a new edit).
        cs.redo_stack.clear();
        return;
    }
    if (cs.undo_stack.size() >= kUndoDepth) {
        // Drop the oldest snapshot. erase from begin is O(N) on a
        // vector but N == 64 here so the cost is negligible compared
        // to the alternative of swapping in a deque.
        cs.undo_stack.erase(cs.undo_stack.begin());
    }
    ComposerState::Snapshot s;
    s.text   = cs.text;
    s.cursor = cs.cursor;
    s.attachments = cs.attachments;
    cs.undo_stack.push_back(std::move(s));
    cs.redo_stack.clear();
    cs.undo_coalescing = coalesce;
}

// Drop back to the LIVE draft and discard the round-trip snapshot.
//
// This was two functions — reset_history and reset_queue_peek — that
// differed only in which sentinel they set to -1; both cleared the same
// snapshot. With browsing as a variant there is one "stop browsing"
// transition, so there is one function.
//
// Why any text edit triggers it: while the composer shows a pulled-up
// history item or a peeked queue slot, editing is still editing THAT
// item. But once the user is simply typing, submit's "re-queue the peeked
// slot, removing its original" contract no longer matches their mental
// model — they think they are composing a fresh message. Returning to
// Live makes the edited bytes the live draft, so a stray keystroke cannot
// silently delete the wrong queue entry on submit.
void reset_browsing(ComposerState& cs) {
    cs.browsing = ComposerState::Live{};
    cs.draft_save.reset();
    cs.draft_save_attachments.clear();
}

// begin_edit for STRUCTURAL edits (paste, delete, chip insert): pushes
// a standalone undo snapshot that never coalesces with neighbours.
void begin_edit(ComposerState& cs) {
    push_undo(cs);
    reset_browsing(cs);
    reset_browsing(cs);
}

// begin_edit for a self-inserting keystroke: coalesces consecutive
// typing into a single undo unit (see push_undo).
void begin_typing_edit(ComposerState& cs) {
    push_undo(cs, /*coalesce=*/true);
    reset_browsing(cs);
    reset_browsing(cs);
}

// Word-boundary cursor walks. Boundaries are runs of whitespace; chip
// placeholders count as a single navigation unit (delegated to
// ui::chip_prev / chip_next). Mirrors the `vim`/`bash` Ctrl+W idea:
// skip whitespace, then skip word characters.
bool is_word_char(unsigned char c) noexcept {
    return std::isalnum(c) || c == '_';
}

int word_left(std::string_view s, int pos) noexcept {
    if (pos <= 0) return 0;
    // Step over a chip if the cursor sits at its right edge.
    int chip = ui::chip_prev(s, pos);
    if (chip != pos - 1
        || (pos > 0 && static_cast<unsigned char>(s[pos - 1]) == 0x01))
        return chip;
    int p = pos;
    // Skip trailing whitespace.
    while (p > 0 && std::isspace(static_cast<unsigned char>(s[p - 1]))) --p;
    // Skip a run of word chars.
    while (p > 0 && is_word_char(static_cast<unsigned char>(s[p - 1]))) --p;
    // If we didn't move past anything word-like, skip a RUN of
    // punctuation ("))))" is one unit, matching how word/whitespace
    // runs are consumed) so the cursor advances by a coherent token.
    if (p == pos) {
        while (p > 0
               && !is_word_char(static_cast<unsigned char>(s[p - 1]))
               && !std::isspace(static_cast<unsigned char>(s[p - 1]))
               && static_cast<unsigned char>(s[p - 1]) != 0x01)
            --p;
    }
    return p;
}

int word_right(std::string_view s, int pos) noexcept {
    int n = static_cast<int>(s.size());
    if (pos >= n) return n;
    int chip = ui::chip_next(s, pos);
    if (chip != pos + 1
        || (pos < n && static_cast<unsigned char>(s[pos]) == 0x01))
        return chip;
    int p = pos;
    while (p < n && is_word_char(static_cast<unsigned char>(s[p]))) ++p;
    while (p < n && std::isspace(static_cast<unsigned char>(s[p]))) ++p;
    // No word/whitespace consumed — skip a RUN of punctuation as one
    // unit (symmetric with word_left).
    if (p == pos) {
        while (p < n
               && !is_word_char(static_cast<unsigned char>(s[p]))
               && !std::isspace(static_cast<unsigned char>(s[p]))
               && static_cast<unsigned char>(s[p]) != 0x01)
            ++p;
    }
    return p;
}

// Image-paste detection. Terminal bracketed-paste delivers UTF-8 text
// only — to attach an image the user drops the file's *path* (drag-
// onto-terminal, "copy as path", whatever). We accept it iff the
// payload is a single trimmed line, names a regular file under the
// workspace's filesystem, and starts with one of the recognised
// image magic-byte prefixes.
const char* detect_image_media_type(std::string_view bytes) noexcept {
    auto u = [&](std::size_t i){ return static_cast<unsigned char>(bytes[i]); };
    // PNG: 89 50 4E 47 0D 0A 1A 0A
    if (bytes.size() >= 8 && u(0) == 0x89 && u(1) == 0x50 && u(2) == 0x4E
        && u(3) == 0x47 && u(4) == 0x0D && u(5) == 0x0A && u(6) == 0x1A
        && u(7) == 0x0A) return "image/png";
    // JPEG: FF D8 FF
    if (bytes.size() >= 3 && u(0) == 0xFF && u(1) == 0xD8 && u(2) == 0xFF)
        return "image/jpeg";
    // GIF87a / GIF89a
    if (bytes.size() >= 6
        && bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F'
        && bytes[3] == '8' && (bytes[4] == '7' || bytes[4] == '9')
        && bytes[5] == 'a') return "image/gif";
    // WEBP: "RIFF" .... "WEBP"
    if (bytes.size() >= 12
        && bytes[0] == 'R' && bytes[1] == 'I' && bytes[2] == 'F' && bytes[3] == 'F'
        && bytes[8] == 'W' && bytes[9] == 'E' && bytes[10] == 'B' && bytes[11] == 'P')
        return "image/webp";
    return nullptr;
}

// Returns (path, media_type) if the paste looks like a single-line
// path to a recognised image file. Empty path on no match.
struct ImagePasteResult {
    std::string  path;
    const char*  media_type = nullptr;
    std::string  body;       // raw image bytes
};

// Normalise a pasted path candidate:
//   – expand a leading `~/` to $HOME (file managers / shells emit this)
//   – unescape `\ ` / `\(` / `\)` / `\'` etc. (drag-drop on macOS and
//     several Linux file managers backslash-escape every shell-special
//     character so the path can be re-pasted into a shell verbatim)
//   – drop CR if a CRLF terminal slipped one through
std::string normalize_path_candidate(std::string_view in) {
    // Trim trailing CR.
    while (!in.empty() && (in.back() == '\r' || in.back() == ' ')) in.remove_suffix(1);
    std::string s;
    s.reserve(in.size());
    if (in.size() >= 2 && in[0] == '~' && in[1] == '/') {
        // Unified home root ($HOME on POSIX/MSYS2, $USERPROFILE on native
        // Windows) so a dropped `~/…` path expands on every platform.
        const fs::path home = agentty::util::home_dir();
        if (!home.empty()) {
            s.append(home.string());
            in.remove_prefix(1);  // keep the '/'
        }
    }
#ifdef _WIN32
    // On Windows the backslash is the PATH SEPARATOR, not a shell escape:
    // a dropped native path `C:\Users\foo\bar` must survive verbatim.
    // Stripping `\` here would mangle it to `C:Usersfoobar`. Take the
    // whole remainder as-is (drag/drop sources on Windows don't
    // shell-escape).
    s.append(in);
#else
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\\' && i + 1 < in.size()) {
            // Shell-style escape (macOS / Linux file managers backslash
            // every shell-special char). Drop the backslash, keep the char.
            s.push_back(in[i + 1]);
            ++i;
            continue;
        }
        s.push_back(in[i]);
    }
#endif
    return s;
}

ImagePasteResult sniff_image_paste(std::string_view text) {
    ImagePasteResult r;
    // Trim leading / trailing whitespace (incl. CR).
    std::size_t a = 0, b = text.size();
    while (a < b && std::isspace(static_cast<unsigned char>(text[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(text[b - 1]))) --b;
    auto trimmed = text.substr(a, b - a);
    if (trimmed.empty()) return r;
    // Must be a single line — embedded newlines hint at pasted prose
    // rather than a path.
    if (trimmed.find('\n') != std::string_view::npos) return r;
    if (trimmed.size() > 4096) return r;  // sane upper bound

    // Strip surrounding quotes (drag-and-drop on macOS / GNOME quotes
    // paths automatically).
    if (trimmed.size() >= 2
        && (trimmed.front() == '\'' || trimmed.front() == '"')
        && trimmed.back() == trimmed.front()) {
        trimmed = trimmed.substr(1, trimmed.size() - 2);
    }
    // file:// URIs land here on some desktops. Accept either two or
    // three leading slashes (file:// vs file:///).
    constexpr std::string_view kFileUri = "file://";
    if (trimmed.size() > kFileUri.size()
        && trimmed.substr(0, kFileUri.size()) == kFileUri) {
        trimmed.remove_prefix(kFileUri.size());
        // Some emitters use file:/// for absolute paths. Collapse the
        // extra slash so the result starts with exactly one '/'.
        if (!trimmed.empty() && trimmed.front() == '/') {
            // already absolute, fine
        }
    }

    auto candidate = normalize_path_candidate(trimmed);
    if (candidate.empty()) return r;

    fs::path p{candidate};
    std::error_code ec;
    if (!fs::is_regular_file(p, ec) || ec) return r;
    // Sniff first 16 bytes for a magic prefix.
    std::ifstream in(p, std::ios::binary);
    if (!in) return r;
    char buf[16]{};
    in.read(buf, sizeof(buf));
    auto got = static_cast<std::size_t>(in.gcount());
    auto* mt = detect_image_media_type(std::string_view{buf, got});
    if (!mt) return r;
    // Slurp full bytes. 8 MiB cap — Anthropic's per-image limit is
    // 5 MB and base64 expansion adds ~33 %, so anything bigger than
    // ~6 MB on disk would fail server-side anyway.
    auto sz = fs::file_size(p, ec);
    if (ec || sz > 8 * 1024 * 1024) return r;
    in.clear();
    in.seekg(0);
    std::string body(static_cast<std::size_t>(sz), '\0');
    in.read(body.data(), static_cast<std::streamsize>(sz));
    if (in.gcount() != static_cast<std::streamsize>(sz)) return r;

    r.path       = p.string();
    r.media_type = mt;
    r.body       = std::move(body);
    return r;
}

// Build the reverse-chronological list of user-message text refs in
// the active thread. Used by ↑/↓ history walking. We deliberately
// don't deduplicate (a user who typed "y" three times to retry
// expects all three to be visitable — the editing context for each
// was different even if the text was the same).
//
// Returns refs to (text, attachments). Each User message persists its
// text in chip-form (placeholders + `attachments` vector); restoring
// both keeps the round-trip non-destructive — a recalled message
// renders as chips, edits like chips, and re-submits as chips.
struct HistoryEntryRef {
    const std::string*             text;
    const std::vector<Attachment>* attachments;
};
std::vector<HistoryEntryRef> previous_user_texts(const Model& m) {
    std::vector<HistoryEntryRef> out;
    out.reserve(m.d.current.messages.size() / 2);
    for (auto it = m.d.current.messages.rbegin();
         it != m.d.current.messages.rend(); ++it) {
        // Proactive <retrieved-context> blocks are synthetic User messages
        // (Role::User + proactive_context) the model treats as reference, not
        // the user's words. They must NEVER surface in the composer's ↑/↓
        // history recall — recalling one would paste the raw fenced block as
        // if the user had typed it.
        if (it->role == Role::User && !it->is_proactive_context() && !it->fork_note && !it->text.empty())
            out.push_back({&it->text, &it->attachments});
    }
    return out;
}

void apply_history_entry(ComposerState& cs, const HistoryEntryRef& entry) {
    cs.text        = *entry.text;
    cs.attachments = *entry.attachments;
    cs.cursor      = static_cast<int>(cs.text.size());
    if (cs.text.find('\n') != std::string::npos) cs.expanded = true;
}

// Smart-paste from the OS clipboard. Image first, then text, in that
// order — image's failure message ("no image on clipboard") is the
// least useful diagnostic for a user who copied text from a browser
// and pressed Ctrl+V, so we silently fall through. The text fallback
// piggybacks on the existing ComposerPaste reducer arm so the same
// always-chip / line-normalisation rules apply as for native
// bracketed-paste sequences.
//
// Used by:
//   • Ctrl+V / Alt+V          (subscribe.cpp → ComposerImagePasteFromClipboard)
//   • Empty bracketed paste   (ComposerPaste arm with e.text.empty())
//
// All three routes through this one helper so the behaviour is
// identical no matter which trigger fired.
Step smart_paste_from_clipboard(Model m) {
    std::string img_err;
    if (auto img = read_clipboard_image(&img_err)) {
        begin_edit(m.ui.composer);
        // Read the real pixel dimensions from the header. A hi-DPI screenshot
        // is a tiny file but can be 3000+ px wide, which Anthropic rejects in
        // a many-image request — the wire drops it, so warn NOW rather than let
        // the turn silently lose the image (or, pre-gate, 400 outright).
        const auto dims = util::image_dimensions(img->bytes);
        const bool oversized = dims.known()
                            && dims.longest() > util::kMaxWireImageSide;
        Attachment att;
        att.kind       = Attachment::Kind::Image;
        att.path       = "<clipboard>";
        att.media_type = std::move(img->media_type);
        att.byte_count = img->bytes.size();
        att.body       = std::move(img->bytes);
        std::size_t idx = m.ui.composer.attachments.size();
        m.ui.composer.attachments.push_back(std::move(att));
        auto placeholder = attachment::make_placeholder(idx);
        m.ui.composer.text.insert(m.ui.composer.cursor, placeholder);
        m.ui.composer.cursor += static_cast<int>(placeholder.size());
        m.ui.composer.expanded = true;
        if (oversized) {
            return {std::move(m), set_status_toast(
                m, "Image is " + std::to_string(dims.w) + "\xc3\x97"
                + std::to_string(dims.h) + " px — over the "
                + std::to_string(util::kMaxWireImageSide)
                + " px limit, it won't be sent. Resize it first.",
                std::chrono::seconds{8})};
        }
        return done(std::move(m));
    }

    // No image — try text. Re-enter the ComposerPaste arm with the
    // captured text so we get newline normalisation + always-chip
    // treatment for free (same path bracketed paste takes).
    std::string txt_err;
    if (auto txt = read_clipboard_text(&txt_err); txt && !txt->empty()) {
        return composer_update(std::move(m), ComposerPaste{std::move(*txt)});
    }

    // Every local path failed to produce an image or text. Last resort,
    // ALWAYS: ask the terminal itself. The terminal emulator runs on the
    // user's LOCAL machine even across SSH, so its reply carries the
    // laptop's clipboard back over the pty — maya decodes it into a
    // PasteEvent that re-enters the ComposerPaste arm below (image
    // magic-byte sniff ingests a PNG/JPEG; otherwise the text path takes
    // it). Two dialects, chosen inside maya's query_clipboard:
    //   • OSC 5522 (kitty) — multi-format: carries IMAGE bytes, so a
    //     screenshot pastes across SSH with no remote tool at all.
    //   • OSC 52 read (iTerm2/WezTerm/foot/Ghostty/xterm w/ opts) —
    //     text-only; for images on these, AGENTTY_CLIPBOARD_CMD or the
    //     airgap --clipboard-relay remain the answer.
    // This is the ONE clipboard read that needs no remote tool and
    // no env var, so it's the universal fallback: headless/SSH host with
    // no wl-paste/xclip, a local terminal whose native tools are missing,
    // OR an AGENTTY_CLIPBOARD_CMD ferry that was set but failed (laptop
    // unreachable, wrong reader, no sshd for the callback).
    //
    // Previously this was gated off whenever AGENTTY_CLIPBOARD_CMD was
    // set — "the user picked an explicit ferry, honour its error." But
    // the airgap launcher SETS that env var automatically, so a broken
    // ferry callback dead-ended at the ferry's error and never tried the
    // terminal, the one path that works out of the box. The ferry is
    // still PREFERRED (it runs first, in read_clipboard_image); OSC 52 is
    // just the safety net under it. Terminals that don't honour OSC 52
    // reads simply never reply — the "reading clipboard…" toast lapses
    // and nothing is stranded. A terminal that DOES reply delivers the
    // bytes as a PasteEvent → the ComposerPaste arm below (no dedicated
    // result Msg); an empty/"?" reply is dropped by maya's parse_osc and
    // the toast simply lapses. img_err/txt_err are intentionally not
    // surfaced here — OSC 52 is strictly more capable than the local
    // probes that produced them, so their "no clipboard here" wording
    // would be misleading while the terminal query is in flight.
    {
        auto toast = set_status_toast(
            m, "reading clipboard from your terminal\xE2\x80\xA6",
            std::chrono::seconds{3});
        // Arm the no-reply diagnosis: if no paste lands within the window,
        // ClipboardQueryTimeout surfaces an actionable message naming the
        // user's exact situation (terminal / tmux / mosh) instead of the
        // toast just lapsing into silence. 1.2s is comfortably above a
        // slow SSH round-trip yet short enough to feel responsive.
        const std::uint64_t seq = ++m.ui.clipboard_query_seq;
        // This path is only reached from an IMAGE paste intent (Ctrl+V /
        // Alt+V after the local probes found no image). Remember that, so a
        // TEXT-only reply can explain itself instead of silently inserting
        // prose where the user expected a screenshot.
        m.ui.clipboard_wanted_image = true;
        // How long to wait before declaring "the terminal never answered".
        //
        // 1.2 s is right for a LOCAL terminal answering a text read: the
        // reply is a few hundred bytes and comes back in microseconds, so a
        // longer wait would only delay an honest error. It is far too short
        // for an IMAGE over ssh: a screenshot is base64 PNG measured in
        // megabytes, and on a remote link the reply is still streaming when
        // the timer fires — so the paste that matters most is exactly the
        // one that always failed. Give a remote session room, and let the
        // progress check below extend it further while bytes keep landing.
        const bool remote = std::getenv("SSH_CONNECTION") != nullptr
                         || std::getenv("SSH_TTY") != nullptr;
        const auto deadline = std::chrono::milliseconds{remote ? 6000 : 1200};
        // Snapshot the byte counter so the timeout arm can tell whether the
        // terminal answered at all.
        m.ui.clipboard_rx_mark =
            maya::clipboard_rx_bytes().load(std::memory_order_relaxed);
        return {std::move(m),
                maya::Cmd<Msg>::batch(
                    maya::Cmd<Msg>::query_clipboard(),
                    std::move(toast),
                    maya::Cmd<Msg>::after(deadline,
                                          Msg{ClipboardQueryTimeout{seq}}))};
    }
}

} // namespace

using maya::overload;

Step composer_update(Model m, msg::ComposerMsg cm) {
    // Stamp the last-interaction clock on ANY composer message (keystroke,
    // edit, cursor move, paste, history walk). The idle blink-stop in the
    // maya composer widget keys off this: 15 s after the last interaction
    // the painted cursor goes solid and stops requesting frames, so an
    // idle agentty stops driving the terminal compositor. Stamping here
    // (the single entry point for all composer msgs) covers every path
    // without touching each arm.
    m.ui.composer.last_edit_ms = maya::anim::default_clock().now_ms();

    // ── LOOP mode makes the composer READ-ONLY ───────────────────────
    // While ^B is armed the box DISPLAYS the prompt that is being re-sent
    // every turn. That display must stay identical to loop_text, or the
    // user is reading one thing while agentty sends another — so every
    // mutating message is dropped rather than allowed to diverge it.
    //
    // ComposerToggleLoop is the deliberate exception: the way out has to
    // keep working from inside the locked state, or the mode is a trap.
    // (Cursor moves are harmless but pointless on a frozen buffer, and
    // dropping them keeps the rule one line instead of an allow-list that
    // rots.) Esc-to-cancel and every global chord live outside this
    // reducer and are unaffected.
    if (m.ui.composer.looping()
        && !std::holds_alternative<ComposerToggleLoop>(cm)) {
        return done(std::move(m));
    }
    // ── Idle-lapse connection re-warm ──────────────────────────
    // The pool's warm socket dies after ~90 s idle (http idle_ttl), so a
    // submit after any longer pause pays a cold TCP+TLS+H2 dial (~150-400
    // ms) on top of TTFT. Typing is the earliest reliable "a request is
    // coming" signal — fire an opportunistic prewarm on the FIRST composer
    // event after the warm window lapsed. Throttled to one dial per idle
    // TTL so key-repeat can't spawn dial threads; prewarm itself is
    // tracked + cancel-safe and swallows errors.
    {
        static std::chrono::steady_clock::time_point last_warm{};
        const auto now = std::chrono::steady_clock::now();
        const bool wire_stale =
            m.s.last_wire_at.time_since_epoch().count() == 0
            || now - m.s.last_wire_at > std::chrono::seconds(85);
        const bool warm_throttled =
            last_warm.time_since_epoch().count() != 0
            && now - last_warm < std::chrono::seconds(85);
        if (wire_stale && !warm_throttled && !m.s.active()) {
            last_warm = now;
            provider::prewarm_active_provider();
        }
    }
    // ── Proactive OAuth refresh ────────────────────────────────
    // Sibling to the socket re-warm above. A token valid at launch can lapse
    // WHILE the user reads output / composes; the first submit would then
    // 401, park the stream, refresh, and retry — visible as first-message
    // lag. Typing is the "a request is coming" signal: if the on-disk OAuth
    // token is within ~5 min of expiry (or already past it) and carries a
    // refresh_token, kick the SAME background refresh init() and the 401
    // handler use, so a fresh bearer is in place before Enter. Gated on
    // oauth_refresh_in_flight (no double-fire; TokenRefreshed clears it) and
    // throttled so key-repeat can't spam the token endpoint.
    maya::Cmd<Msg> proactive_refresh = maya::Cmd<Msg>::none();
    {
        static std::chrono::steady_clock::time_point last_refresh_probe{};
        const auto now = std::chrono::steady_clock::now();
        const bool probe_throttled =
            last_refresh_probe.time_since_epoch().count() != 0
            && now - last_refresh_probe < std::chrono::seconds(30);
        if (!probe_throttled && !m.s.active() && !m.s.oauth_refresh_in_flight) {
            last_refresh_probe = now;
            if (auto tok = auth::oauth_proactive_refresh_token()) {
                m.s.oauth_refresh_in_flight = true;
                proactive_refresh = cmd::refresh_oauth(std::move(*tok));
            }
        }
    }
    Step step = std::visit(overload{
        [&](ComposerCharInput e) -> Step {
            // '/' opens the command palette when it's LINE-LEADING —
            // the cursor sits at the start of the buffer or right
            // after a newline, with no attachment placeholder
            // immediately before it. This matches how real shells /
            // Claude Code treat slash-commands (line-leading) and is
            // symmetric with the '@'/'#' word-boundary rule below,
            // rather than the older "only on a totally empty buffer"
            // gate that silently swallowed '/' at the start of an
            // existing draft. Mid-prose slashes (URLs, regexes,
            // formula divides) still type literally because they're
            // never line-leading. Once the palette is open, subscribe
            // routes subsequent keystrokes to on_command_palette so
            // the slash itself is not consumed twice; the open-palette
            // starts with an empty query.
            auto at_line_start = [&]{
                if (m.ui.composer.cursor == 0) return true;
                char prev = m.ui.composer.text[
                    static_cast<std::size_t>(m.ui.composer.cursor) - 1];
                return prev == '\n';
            };
            if (e.ch == U'/' && at_line_start()) {
                m.ui.panel = pn::CommandPalette{};
                return done(std::move(m));
            }
            // '@' opens the file mention picker. Unlike '/' this is
            // permitted mid-prose ("ping @alice tomorrow" is a fine
            // English sentence), so we don't require an empty buffer
            // — we only require the previous character to be a word
            // boundary (start-of-string or whitespace) so URLs / emails
            // / e.g. "alice@example.com" don't trigger.
            auto at_word_boundary = [&]{
                if (m.ui.composer.cursor == 0) return true;
                char prev = m.ui.composer.text[
                    static_cast<std::size_t>(m.ui.composer.cursor) - 1];
                return prev == ' ' || prev == '\t' || prev == '\n';
            };
            if (e.ch == U'@' && at_word_boundary()) {
                mention::Open o;
                // Non-blocking: snapshot the file list ONLY if the prewarm
                // has landed (files_ready()). If it's still indexing, open
                // with an empty snapshot + "indexing…" hint rather than
                // freezing the UI on an inline walk; the next keystroke
                // re-pulls once the background thread publishes.
                if (files_ready()) o.files = list_workspace_files();
                m.ui.panel = pn::Mention{std::move(o)};
                // Refresh git signals in the background so the working-set
                // ranking reflects edits made since startup (the agent may
                // have modified files this session). Cheap (~two git calls);
                // this open uses the current map, the next keystroke the
                // fresh one. Terminate-proof detach (a throw out of a bare
                // detached thread is process death).
                agentty::util::run_isolated_detached(
                    "composer.git_refresh", []{ refresh_git_signals(); });
                return done(std::move(m));
            }
            // '#' opens the symbol picker — mirrors '@'. Non-blocking:
            // snapshot only if the (parallel) symbol scan has landed;
            // otherwise open with an empty snapshot + "indexing…" hint and
            // fill on the first keystroke. Never blocks the UI on the scan.
            if (e.ch == U'#' && at_word_boundary()) {
                symbol_palette::Open o;
                if (symbols_ready()) o.entries = list_workspace_symbols();
                m.ui.panel = pn::Symbol{std::move(o)};
                return done(std::move(m));
            }
            // Coalesce consecutive typing into one undo unit, but
            // break the run on whitespace so Ctrl+Z rewinds word by
            // word (not the whole paragraph at once). A space starts a
            // fresh snapshot; the following word coalesces onto it.
            const bool is_ws = (e.ch == U' ' || e.ch == U'\t');
            if (is_ws) begin_edit(m.ui.composer);
            else       begin_typing_edit(m.ui.composer);
            auto utf8 = ui::utf8_encode(e.ch);
            m.ui.composer.text.insert(m.ui.composer.cursor, utf8);
            m.ui.composer.cursor += static_cast<int>(utf8.size());
            return done(std::move(m));
        },
        [&](ComposerBackspace) -> Step {
            if (m.ui.composer.cursor > 0 && !m.ui.composer.text.empty()) {
                begin_edit(m.ui.composer);
                // chip_prev jumps over a whole placeholder if the
                // cursor is at the right edge of one — backspace then
                // erases the entire chip token in a single keystroke,
                // which is the user's mental model: "delete the
                // attachment, not the closing sentinel byte." The
                // attachment object stays in the vector by index
                // (renumbering would break other placeholders pointing
                // at later indices); orphans get GC'd when the
                // composer next clears.
                int p = ui::chip_prev(m.ui.composer.text, m.ui.composer.cursor);
                m.ui.composer.text.erase(p, m.ui.composer.cursor - p);
                m.ui.composer.cursor = p;
            }
            return done(std::move(m));
        },
        [&](ComposerEnter)  { return submit_message(std::move(m)); },
        [&](ComposerSubmit) { return submit_message(std::move(m)); },
        [&](ComposerNewline) -> Step {
            begin_edit(m.ui.composer);
            m.ui.composer.text.insert(m.ui.composer.cursor, "\n");
            m.ui.composer.cursor += 1;
            m.ui.composer.expanded = true;
            return done(std::move(m));
        },
        [&](ComposerToggleExpand) -> Step {
            m.ui.composer.expanded = !m.ui.composer.expanded;
            return done(std::move(m));
        },
        [&](ComposerToggleLoop) -> Step {
            auto& c = m.ui.composer;
            // Already looping → disarm. Deliberately does NOT cancel the
            // in-flight turn: you're saying "stop after this one", which is
            // the non-destructive reading. Esc still cancels the turn itself.
            if (c.loop_armed) {
                const int n = c.loop_iterations;
                c.loop_armed = false;
                c.loop_text.clear();
                c.loop_attachments.clear();
                c.loop_iterations = 0;
                auto toast = set_status_toast(
                    m, n > 0 ? "loop off \xc2\xb7 " + std::to_string(n) + " sent"
                             : std::string{"loop off"});
                return {std::move(m), std::move(toast)};
            }
            // Arming needs something to repeat. An empty composer would arm a
            // loop with no payload — make that unrepresentable rather than
            // silently no-op later.
            if (c.text.empty()) {
                auto toast = set_status_toast(
                    m, "loop: type a message first");
                return {std::move(m), std::move(toast)};
            }
            // Snapshot the payload, then submit it. The snapshot (not the
            // live composer) is what repeats, so the user can keep typing.
            c.loop_armed       = true;
            c.loop_text        = c.text;
            c.loop_attachments = c.attachments;
            c.loop_iterations  = 0;
            auto step = submit_message(std::move(m));
            // submit drains the composer; put the armed payload BACK so the
            // box keeps showing what is on repeat. While looping the composer
            // is read-only (see the editing guard), so this is a display of
            // the armed prompt rather than an editable draft — without it the
            // user watches an empty box auto-send something they can't see.
            auto& sc = step.first.ui.composer;
            sc.text        = sc.loop_text;
            sc.attachments = sc.loop_attachments;
            sc.cursor      = static_cast<int>(sc.text.size());
            return step;
        },
        [&](ComposerCursorLeft) -> Step {
            m.ui.composer.undo_coalescing = false;
            m.ui.composer.cursor = ui::chip_prev(m.ui.composer.text, m.ui.composer.cursor);
            return done(std::move(m));
        },
        [&](ComposerCursorRight) -> Step {
            m.ui.composer.undo_coalescing = false;
            m.ui.composer.cursor = ui::chip_next(m.ui.composer.text, m.ui.composer.cursor);
            return done(std::move(m));
        },
        [&](ComposerCursorHome) -> Step {
            m.ui.composer.undo_coalescing = false;
            m.ui.composer.cursor = 0;
            return done(std::move(m));
        },
        [&](ComposerCursorEnd) -> Step {
            m.ui.composer.undo_coalescing = false;
            m.ui.composer.cursor = static_cast<int>(m.ui.composer.text.size());
            return done(std::move(m));
        },
        [&](ComposerCursorWordLeft) -> Step {
            m.ui.composer.undo_coalescing = false;
            m.ui.composer.cursor = word_left(m.ui.composer.text, m.ui.composer.cursor);
            return done(std::move(m));
        },
        [&](ComposerCursorWordRight) -> Step {
            m.ui.composer.undo_coalescing = false;
            m.ui.composer.cursor = word_right(m.ui.composer.text, m.ui.composer.cursor);
            return done(std::move(m));
        },
        [&](ComposerKillToEndOfLine) -> Step {
            const auto& s = m.ui.composer.text;
            int n = static_cast<int>(s.size());
            int p = m.ui.composer.cursor;
            if (p >= n) return done(std::move(m));
            int q = p;
            while (q < n && s[q] != '\n') ++q;
            // Standard readline: Ctrl+K on a line of text deletes to
            // EOL exclusive of '\n'; on an empty line it deletes the
            // newline itself (collapses the empty line away).
            if (q == p && q < n && s[q] == '\n') ++q;
            begin_edit(m.ui.composer);
            m.ui.composer.text.erase(p, q - p);
            return done(std::move(m));
        },
        [&](ComposerKillToBeginningOfLine) -> Step {
            const auto& s = m.ui.composer.text;
            int p = m.ui.composer.cursor;
            if (p <= 0) return done(std::move(m));
            int q = p;
            while (q > 0 && s[q - 1] != '\n') --q;
            if (q == p) return done(std::move(m));
            begin_edit(m.ui.composer);
            m.ui.composer.text.erase(q, p - q);
            m.ui.composer.cursor = q;
            return done(std::move(m));
        },
        [&](ComposerDeleteWordBack) -> Step {
            // Ctrl+W — readline unix-word-rubout. Delete from the
            // previous word boundary up to the cursor. Reuses the
            // chip-aware word_left boundary so a Ctrl+W at the right
            // edge of an attachment chip removes the whole token in
            // one stroke (same mental model as chip-aware Backspace).
            int p = m.ui.composer.cursor;
            if (p <= 0) return done(std::move(m));
            int q = word_left(m.ui.composer.text, p);
            if (q >= p) return done(std::move(m));
            begin_edit(m.ui.composer);
            m.ui.composer.text.erase(static_cast<std::size_t>(q),
                                     static_cast<std::size_t>(p - q));
            m.ui.composer.cursor = q;
            return done(std::move(m));
        },
        [&](ComposerDeleteWordForward) -> Step {
            // Alt+D — readline kill-word. Delete from the cursor up to
            // the next word boundary; cursor stays put. Symmetric to
            // Ctrl+W and chip-aware via word_right.
            const auto& s = m.ui.composer.text;
            int p = m.ui.composer.cursor;
            if (p >= static_cast<int>(s.size())) return done(std::move(m));
            int q = word_right(s, p);
            if (q <= p) return done(std::move(m));
            begin_edit(m.ui.composer);
            m.ui.composer.text.erase(static_cast<std::size_t>(p),
                                     static_cast<std::size_t>(q - p));
            return done(std::move(m));
        },
        [&](ComposerUndo) -> Step {
            if (m.ui.composer.undo_stack.empty()) return done(std::move(m));
            ComposerState::Snapshot cur;
            cur.text   = std::move(m.ui.composer.text);
            cur.cursor = m.ui.composer.cursor;
            cur.attachments = std::move(m.ui.composer.attachments);
            auto prev = std::move(m.ui.composer.undo_stack.back());
            m.ui.composer.undo_stack.pop_back();
            if (m.ui.composer.redo_stack.size() >= kUndoDepth)
                m.ui.composer.redo_stack.erase(m.ui.composer.redo_stack.begin());
            m.ui.composer.redo_stack.push_back(std::move(cur));
            m.ui.composer.text        = std::move(prev.text);
            m.ui.composer.cursor      = prev.cursor;
            m.ui.composer.attachments = std::move(prev.attachments);
            m.ui.composer.undo_coalescing = false;
            reset_browsing(m.ui.composer);
            reset_browsing(m.ui.composer);
            return done(std::move(m));
        },
        [&](ComposerRedo) -> Step {
            if (m.ui.composer.redo_stack.empty()) return done(std::move(m));
            ComposerState::Snapshot cur;
            cur.text   = std::move(m.ui.composer.text);
            cur.cursor = m.ui.composer.cursor;
            cur.attachments = std::move(m.ui.composer.attachments);
            auto next = std::move(m.ui.composer.redo_stack.back());
            m.ui.composer.redo_stack.pop_back();
            if (m.ui.composer.undo_stack.size() >= kUndoDepth)
                m.ui.composer.undo_stack.erase(m.ui.composer.undo_stack.begin());
            m.ui.composer.undo_stack.push_back(std::move(cur));
            m.ui.composer.text        = std::move(next.text);
            m.ui.composer.cursor      = next.cursor;
            m.ui.composer.attachments = std::move(next.attachments);
            m.ui.composer.undo_coalescing = false;
            reset_browsing(m.ui.composer);
            reset_browsing(m.ui.composer);
            return done(std::move(m));
        },
        [&](ComposerHistoryPrev) -> Step {
            auto texts = previous_user_texts(m);
            if (texts.empty()) return done(std::move(m));
            auto& cs = m.ui.composer;
            // Where we are now: browsing history, or anywhere else (Live,
            // or peeking the queue — ↑ leaves that and enters history).
            const auto cur = cs.history_index();
            int next_idx = cur.value_or(-1) + 1;
            if (next_idx >= static_cast<int>(texts.size()))
                next_idx = static_cast<int>(texts.size()) - 1;
            // First ↑ out of the live draft — snapshot whatever the user
            // had typed so ↓ all the way back restores it.
            if (!cur) cs.draft_save = cs.text;
            cs.browsing = ComposerState::History{next_idx};
            cs.undo_coalescing = false;
            apply_history_entry(cs, texts[static_cast<std::size_t>(next_idx)]);
            // History walk does NOT push undo: ↑↓ alone are
            // non-destructive (they leave draft_save intact). Once
            // the user edits, begin_edit fires reset_browsing and
            // the walked text becomes the new live draft.
            return done(std::move(m));
        },
        [&](ComposerHistoryNext) -> Step {
            auto& cs = m.ui.composer;
            const auto cur = cs.history_index();
            if (!cur) return done(std::move(m));   // not walking history
            cs.undo_coalescing = false;
            auto texts = previous_user_texts(m);
            const int next_idx = *cur - 1;
            if (next_idx < 0) {
                // Walked all the way back to the live draft.
                cs.browsing = ComposerState::Live{};
                cs.text = cs.draft_save.value_or(std::string{});
                cs.cursor = static_cast<int>(cs.text.size());
                cs.draft_save.reset();
                return done(std::move(m));
            }
            cs.browsing = ComposerState::History{next_idx};
            if (next_idx < static_cast<int>(texts.size()))
                apply_history_entry(cs, texts[static_cast<std::size_t>(next_idx)]);
            return done(std::move(m));
        },
        [&](ComposerImagePasteFromClipboard) -> Step {
            // Bracketed paste delivers UTF-8 text only; for an image-
            // on-clipboard we ask the OS clipboard directly (see
            // io/clipboard.cpp for the per-OS implementation). Sync —
            // the helpers exit immediately on a no-image clipboard.
            // Same code path as the Alt+V trigger and the empty-
            // bracketed-paste detection (Windows Terminal swallows
            // Ctrl+V, our two fallbacks land here).
            return smart_paste_from_clipboard(std::move(m));
        },
        [&](ComposerPaste& e) -> Step {
            // ANY paste (bracketed, OSC 52 reply, OSC 5522 image reply)
            // satisfies an in-flight escape-based clipboard query — cancel
            // the pending no-reply diagnosis.
            m.ui.clipboard_query_done = m.ui.clipboard_query_seq;
            const bool wanted_image = m.ui.clipboard_wanted_image;
            m.ui.clipboard_wanted_image = false;
            // Empty bracketed paste → Windows Terminal signature for
            // "user hit Ctrl+V but the clipboard has no text content".
            // The terminal swallows Ctrl+V to run its own paste action;
            // when CF_UNICODETEXT is absent (e.g. clipboard holds an
            // image from Win+Shift+S), the action sends `\x1b[200~
            // \x1b[201~` with nothing in between. Route it through the
            // same path Alt+V uses so the user gets the image without
            // having to learn an alternate shortcut.
            if (e.text.empty())
                return smart_paste_from_clipboard(std::move(m));

            // Bracketed-paste of raw image bytes — vanishingly rare
            // (most terminals scrub binary out of paste), but Kitty
            // and Wezterm with `--allow-passthrough`-style options
            // will hand us the bytes verbatim. Detect by magic prefix
            // and ingest as Image. Tested before path-detection so a
            // PNG that happens to start with bytes that could parse
            // as a path takes the right branch.
            if (auto* mt = detect_image_media_type(e.text); mt != nullptr) {
                begin_edit(m.ui.composer);
                Attachment att;
                att.kind       = Attachment::Kind::Image;
                att.path       = "<paste>";
                att.media_type = mt;
                att.byte_count = e.text.size();
                att.body       = std::move(e.text);
                std::size_t idx = m.ui.composer.attachments.size();
                m.ui.composer.attachments.push_back(std::move(att));
                auto placeholder = attachment::make_placeholder(idx);
                m.ui.composer.text.insert(m.ui.composer.cursor, placeholder);
                m.ui.composer.cursor += static_cast<int>(placeholder.size());
                m.ui.composer.expanded = true;
                return done(std::move(m));
            }

            // Image-path paste: a single-line path naming a real image
            // file becomes an Image attachment. Try this first so a
            // path under 800 bytes doesn't get inlined as plain text.
            // Sniff the first 16 bytes for a known magic prefix; only
            // ingest if it actually looks like an image.
            if (auto img = sniff_image_paste(e.text); !img.path.empty()) {
                begin_edit(m.ui.composer);
                Attachment att;
                att.kind       = Attachment::Kind::Image;
                att.path       = std::move(img.path);
                att.media_type = img.media_type;
                att.byte_count = img.body.size();
                att.body       = std::move(img.body);
                std::size_t idx = m.ui.composer.attachments.size();
                m.ui.composer.attachments.push_back(std::move(att));
                auto placeholder = attachment::make_placeholder(idx);
                m.ui.composer.text.insert(m.ui.composer.cursor, placeholder);
                m.ui.composer.cursor += static_cast<int>(placeholder.size());
                m.ui.composer.expanded = true;
                return done(std::move(m));
            }

            // Empty paste (clipboard manager hiccup, terminal dropped
            // a binary clipboard) — nothing to do.
            if (e.text.empty()) return done(std::move(m));

            // Normalize line endings: some terminals (and ssh tty cooked
            // mode) translate \n → \r in bracketed paste so the bytes
            // look like the user pressed Enter at every line. Maya's
            // word_wrap splits on \n only, so leaving \r in the body
            // collapses the whole paste into one logical line that
            // wraps based on width — visible as a giant single-row
            // user message with stray rendering, *not* the line-by-line
            // render the user expects. Canvas::write_text skips \r as
            // a control char so the cells don't get the carriage-return
            // byte either; without this normalization the paste both
            // looks wrong AND throws the parent layout off (the layout
            // sees one wrapped line, but the cached/measured height
            // mismatch leaves visible gaps between the user turn and
            // the assistant turn). Strip stray \r and convert \r-only
            // / \r\n to \n; pure-\n input is unchanged.
            {
                std::string norm;
                norm.reserve(e.text.size());
                for (std::size_t i = 0; i < e.text.size(); ++i) {
                    char c = e.text[i];
                    if (c == '\r') {
                        norm.push_back('\n');
                        if (i + 1 < e.text.size() && e.text[i + 1] == '\n')
                            ++i;
                    } else {
                        norm.push_back(c);
                    }
                }
                e.text = std::move(norm);
            }

            // Always-chip: every paste, regardless of size, becomes
            // an Attachment + inline placeholder. Single-line pastes
            // get a preview caption ("Pasted: hello world") so they
            // stay legible; multi-line pastes get the lines/bytes
            // summary. Inline-as-text was a UX regression for any
            // paste with structure — the previous threshold-based
            // collapse mixed two presentations and made the composer
            // height twitch with paste size.
            const std::size_t lines =
                std::ranges::count(e.text, '\n')
                + (e.text.back() == '\n' ? 0 : 1);

            begin_edit(m.ui.composer);
            Attachment att;
            att.kind       = Attachment::Kind::Paste;
            att.line_count = lines;
            att.byte_count = e.text.size();
            att.body       = std::move(e.text);
            std::size_t idx = m.ui.composer.attachments.size();
            m.ui.composer.attachments.push_back(std::move(att));
            auto placeholder = attachment::make_placeholder(idx);
            m.ui.composer.text.insert(m.ui.composer.cursor, placeholder);
            m.ui.composer.cursor += static_cast<int>(placeholder.size());
            if (lines > 1) m.ui.composer.expanded = true;
            // The terminal answered with TEXT. Whether that deserves a
            // warning depends entirely on what was on the clipboard, and we
            // cannot see that from here — a text reply to a text clipboard
            // is a complete success, not a degraded image paste.
            //
            // This used to fire on every Ctrl+V over SSH. `wanted_image` is
            // set for ANY paste (Ctrl+V is the only paste key), and over SSH
            // the local probes always fail, so every ordinary text paste
            // reached this arm and was met with "kitty needs clipboard READ
            // permission" — advice that is both wrong and, once the user has
            // already granted it, actively confusing. A warning that fires
            // when nothing is wrong trains people to ignore the one that
            // matters.
            //
            // Only speak up when the terminal CANNOT deliver images at all
            // (no OSC 5522), because then a genuine image paste is silently
            // impossible and the user needs to know. A kitty that answered
            // text simply had text on the clipboard: say nothing.
            const bool terminal_can_do_images =
                maya::ansi::env_supports_osc5522();
            if (wanted_image && !terminal_can_do_images) {
                // Reaching here means the terminal has no image dialect at
                // all — the OSC 5522 branch is handled above by staying
                // quiet, because a kitty that replied with text was simply
                // asked for text. So there is exactly one thing to say, and
                // it is about capability, not permission.
                auto toast = set_status_toast(
                    m,
                    "pasted text \xe2\x80\x94 your terminal can't send images "
                    "over SSH (needs kitty's OSC 5522); attach by path, "
                    "or set AGENTTY_CLIPBOARD_CMD",
                    std::chrono::seconds{9});
                return {std::move(m), std::move(toast)};
            }
            return done(std::move(m));
        },
        [&](ClipboardQueryTimeout& e) -> Step {
            // The escape-based clipboard read went unanswered. Stale guard:
            // only diagnose if THIS query is still the latest and no paste
            // arrived meanwhile.
            if (e.seq != m.ui.clipboard_query_seq
                || m.ui.clipboard_query_done >= e.seq)
                return done(std::move(m));
            // The terminal may be ANSWERING, just not finished: an image
            // reply is megabytes of base64 and streams in over seconds on a
            // remote link. Diagnosing "no answer" while bytes are actively
            // landing is how a large screenshot became impossible to paste.
            // If the counter moved since we armed, re-arm on the new mark
            // and let it finish; only genuine silence reaches the diagnosis
            // below. Bounded by the transfer itself — when the bytes stop,
            // the next timeout sees no movement and reports honestly.
            {
                const std::uint64_t now =
                    maya::clipboard_rx_bytes().load(std::memory_order_relaxed);
                if (now != m.ui.clipboard_rx_mark) {
                    m.ui.clipboard_rx_mark = now;
                    const auto again = std::chrono::milliseconds{1500};
                    return {std::move(m),
                            maya::Cmd<Msg>::after(again,
                                Msg{ClipboardQueryTimeout{e.seq}})};
                }
            }
            // This query is dead. Clear the image-intent latch so it cannot
            // leak into an unrelated later paste and mislabel it.
            m.ui.clipboard_wanted_image = false;
            // Name the user's EXACT situation and the shortest path out —
            // an unanswered query must never dead-end in silence.
            const bool in_mosh = [] {
                // mosh-server exports no reliable env marker into its child
                // shell — detect it by walking the process ancestry instead
                // (Linux/procfs; other platforms fall through to false and
                // get the generic SSH wording, which still applies).
#if defined(__linux__)
                int pid = static_cast<int>(::getppid());
                for (int hop = 0; hop < 12 && pid > 1; ++hop) {
                    std::string base = "/proc/" + std::to_string(pid);
                    if (std::ifstream comm(base + "/comm"); comm) {
                        std::string name;
                        std::getline(comm, name);
                        if (name.find("mosh-server") != std::string::npos)
                            return true;
                    }
                    std::ifstream stat(base + "/stat");
                    if (!stat) break;
                    // stat: pid (comm) state ppid … — comm may contain
                    // spaces, so parse from after the LAST ')'.
                    std::string line;
                    std::getline(stat, line);
                    auto rp = line.rfind(')');
                    if (rp == std::string::npos) break;
                    int state_and_ppid_at = static_cast<int>(rp) + 2;
                    std::istringstream tail(line.substr(
                        static_cast<std::size_t>(state_and_ppid_at)));
                    char state_ch; int ppid = 0;
                    tail >> state_ch >> ppid;
                    if (ppid <= 1) break;
                    pid = ppid;
                }
#endif
                // Ancestry misses the common persistent-tmux topology:
                // when the tmux SERVER is started by systemd (or an older
                // login) the mosh client attaches to it later, so
                // mosh-server is a sibling of the server, never an ancestor
                // of us. Ask tmux who is attached and walk THAT process
                // instead — otherwise a genuine mosh session is diagnosed
                // with the tmux wording and the user chases a tmux config
                // that was never the problem.
#if defined(__linux__)
                if (maya::tmux::active()) {
                    if (const int cpid = maya::tmux::client_pid(); cpid > 1) {
                        int p = cpid;
                        for (int hop = 0; hop < 12 && p > 1; ++hop) {
                            std::string base = "/proc/" + std::to_string(p);
                            if (std::ifstream comm(base + "/comm"); comm) {
                                std::string name;
                                std::getline(comm, name);
                                if (name.find("mosh-server") != std::string::npos)
                                    return true;
                            }
                            std::ifstream stat(base + "/stat");
                            if (!stat) break;
                            std::string line;
                            std::getline(stat, line);
                            auto rp = line.rfind(')');
                            if (rp == std::string::npos) break;
                            std::istringstream tail(line.substr(rp + 2));
                            char st; int pp = 0;
                            tail >> st >> pp;
                            if (pp <= 1) break;
                            p = pp;
                        }
                    }
                }
#endif
                return false;
            }();
            // Single source of truth for tmux presence: maya::tmux owns
            // both topologies ($TMUX here, or a tmux-*/screen-* TERM that
            // survived ssh) plus the passthrough/feature probes below.
            const bool in_tmux = maya::tmux::active();
            // The capability answers below come from a probe maya memoises
            // for the whole process. tmux capabilities are per-CLIENT, so
            // detaching a desktop kitty and reattaching from a phone (or
            // simply fixing tmux.conf and reattaching) leaves the cached
            // verdict describing a terminal that is no longer there — the
            // failure then repeats forever and no amount of correct config
            // clears it. A clipboard read has just FAILED, which is exactly
            // the moment a stale verdict is worth one ~8 ms round-trip to
            // re-check. Only re-probes when the attached client changed.
            if (in_tmux) (void)maya::tmux::refresh_if_client_changed();
            const bool in_ssh  = std::getenv("SSH_CONNECTION") != nullptr
                              || std::getenv("SSH_TTY") != nullptr;
            std::string msg;
            if (in_mosh) {
                msg = "clipboard: mosh doesn't relay terminal clipboard "
                      "replies \xe2\x80\x94 use plain ssh (or tmux over ssh), or set "
                      "AGENTTY_CLIPBOARD_CMD";
            } else if (in_tmux) {
                // Name the ACTUAL blocker instead of guessing. Order
                // matters: these are checked in the order tmux applies
                // them, so the first failure reported is the first one
                // the user has to fix.
                if (!maya::tmux::clipboard_reads_relayed()) {
                    // THE common case, and silent until now: tmux's
                    // `get-clipboard` defaults to `buffer`, so tmux
                    // answers a clipboard read from its OWN paste buffer
                    // and never asks the terminal. A paste buffer holds
                    // TEXT — an image can never come back through it, no
                    // matter what the outer terminal supports.
                    msg = "clipboard: tmux answers reads from its own paste "
                          "buffer (text only) \xe2\x80\x94 run `tmux set -g "
                          "get-clipboard both` so it asks your terminal, "
                          "then retry";
                } else if (!maya::tmux::passthrough_allowed()) {
                    msg = "clipboard: tmux is dropping the request \xe2\x80\x94 run "
                          "`tmux set -g allow-passthrough on` (it is OFF by "
                          "default), then retry";
                } else if (!maya::tmux::has_feature(
                               maya::tmux::Feature::Clipboard)) {
                    msg = "clipboard: tmux reports your outer terminal has no "
                          "clipboard support \xe2\x80\x94 add `set -ga terminal-features "
                          "\",*:clipboard\"` if it does, or set AGENTTY_CLIPBOARD_CMD";
                } else {
                    msg = "clipboard: no reply through tmux \xe2\x80\x94 passthrough and "
                          "clipboard are on, so the outer terminal didn't answer "
                          "(images need a kitty outer terminal)";
                }
            } else if (in_ssh) {
                msg = "clipboard: your terminal didn't answer \xe2\x80\x94 images "
                      "over SSH need kitty (OSC 5522); else set "
                      "AGENTTY_CLIPBOARD_CMD='ssh <laptop> wl-paste -t image/png'";
            } else {
                msg = "clipboard: terminal didn't answer the read query "
                      "\xe2\x80\x94 install wl-clipboard/xclip (Linux) or use a "
                      "terminal with OSC 52 read support";
            }
            auto toast = set_status_toast(m, msg, std::chrono::seconds{8});
            return {std::move(m), std::move(toast)};
        },
        [&](ComposerRecallQueued) -> Step {
            // No-op when there's nothing to recall — the caller (the
            // Up-arrow keymap) only emits this when the queue is
            // non-empty, but the predicate is racy across frames so
            // be defensive.
            if (m.ui.composer.queued.empty()) return done(std::move(m));

            // Drain the queue into the composer, joined by '\n', and
            // append any pre-existing composer text after another
            // '\n'. Mirrors Claude Code's `Lc_` (binary offset
            // 76303220): a single ↑ press drains the WHOLE editable
            // queue into one composer load — no per-item cycling.
            // Multi-line queued items keep their newlines so a paste
            // that became a queued message survives the recall
            // round-trip.
            //
            // Each queued slot may carry its own attachments[] with
            // 0-based placeholder indices in its text. We merge the
            // attachment vectors and rewrite each slot's placeholders
            // to point at the new (merged) indices as we concatenate.
            std::string             recalled;
            std::vector<Attachment> merged_atts;
            auto append_with_remap = [&](std::string_view text,
                                         std::vector<Attachment>& atts) {
                // base = index where this slot's attachments will
                // land in merged_atts after we push them.
                std::size_t base = merged_atts.size();
                for (std::size_t i = 0; i < text.size(); ) {
                    if (static_cast<unsigned char>(text[i]) == attachment::kSentinel) {
                        auto len = attachment::placeholder_len_at(text, i);
                        if (len > 0) {
                            auto local_idx = attachment::placeholder_index(text, i);
                            // Drop placeholders that don't resolve
                            // (corruption / stale index) — same
                            // defensive policy as attachment::expand.
                            if (local_idx < atts.size())
                                recalled += attachment::make_placeholder(base + local_idx);
                            i += len;
                            continue;
                        }
                    }
                    recalled.push_back(text[i++]);
                }
                for (auto& a : atts) merged_atts.push_back(std::move(a));
            };
            for (std::size_t i = 0; i < m.ui.composer.queued.size(); ++i) {
                if (i > 0) recalled.push_back('\n');
                append_with_remap(m.ui.composer.queued[i].text,
                                  m.ui.composer.queued[i].attachments);
            }
            // Cursor lands at the boundary between recalled text and
            // the user's pre-existing composer input — exactly where
            // they'd want to start editing or appending. (Claude
            // Code's seam is `O.join("\n").length + 1 + _`; we use
            // the same idea: end-of-recalled + 1 if there's anything
            // after, else end-of-recalled.)
            int boundary = static_cast<int>(recalled.size());
            if (!m.ui.composer.text.empty()) {
                // The user's pre-existing composer text might ALSO
                // carry placeholders into composer.attachments; merge
                // those last and remap before splicing.
                recalled.push_back('\n');
                ++boundary;
                append_with_remap(m.ui.composer.text, m.ui.composer.attachments);
            }
            begin_edit(m.ui.composer);
            m.ui.composer.text        = std::move(recalled);
            m.ui.composer.attachments = std::move(merged_atts);
            m.ui.composer.cursor      = boundary;
            // Multi-line content → flip expanded so the composer's
            // `expanded` cap (16 rows) takes effect, not the 8-row
            // unexpanded cap. Same trigger as ComposerPaste.
            if (m.ui.composer.text.find('\n') != std::string::npos)
                m.ui.composer.expanded = true;
            // Destructive recall: queued items now live ONLY in the
            // composer buffer. Re-submit re-queues at the tail (fresh
            // tail position). Clearing the composer drops them. Same
            // trade-off as Claude Code — keeps the data model simple
            // (no "soft-deleted, recallable" intermediate state).
            m.ui.composer.queued.clear();
            return done(std::move(m));
        },
        [&](ComposerQueuePeekPrev) -> Step {
            // Alt+↑ — step further INTO the queue. Order: queue[last]
            // (most-recently queued, closest to "the one I just
            // typed") → queue[last-1] → … → queue[0]. So the first
            // press loads the tail item, which is what the user
            // usually wants when correcting a typo in their last
            // queued message.
            if (m.ui.composer.queued.empty()) return done(std::move(m));
            auto& cs = m.ui.composer;
            cs.undo_coalescing = false;
            const int n = static_cast<int>(cs.queued.size());
            int next_idx;
            // Entering the queue from history is just a state change now:
            // assigning QueuePeek REPLACES History, so the "mutually
            // exclusive — abandon any history pick" block that used to sit
            // here is gone. The snapshot still has to be dropped, because
            // the text on screen WAS the history pick and saving it would
            // conflate it with the live draft.
            if (const auto peek = cs.queue_peek_index()) {
                // Already peeking — commit the user's edits back into the
                // queue slot they came from before moving on. Without this,
                // Alt+↑ → type → Alt+↑ would silently discard the typed
                // correction.
                auto& slot = cs.queued[static_cast<std::size_t>(*peek)];
                slot.text        = std::move(cs.text);
                slot.attachments = std::move(cs.attachments);
                next_idx = *peek - 1;
                if (next_idx < 0) next_idx = 0;   // clamp, no wrap
            } else {
                if (!cs.is_live()) {
                    // Came from a history pick: drop its snapshot.
                    cs.draft_save.reset();
                    cs.draft_save_attachments.clear();
                } else {
                    // First press from the live draft — snapshot text +
                    // attachments so Alt+↓ past the tail restores them.
                    cs.draft_save             = cs.text;
                    cs.draft_save_attachments = cs.attachments;
                }
                next_idx = n - 1;
            }
            cs.browsing = ComposerState::QueuePeek{next_idx};
            // Move the slot into the live composer (we'll write it
            // back on the next cycle / submit).
            cs.text        = cs.queued[static_cast<std::size_t>(next_idx)].text;
            cs.attachments = cs.queued[static_cast<std::size_t>(next_idx)].attachments;
            cs.cursor = static_cast<int>(cs.text.size());
            // Peek doesn't snapshot undo (round-trip non-destructive).
            // Multi-line peeked content → honour expanded cap.
            if (cs.text.find('\n') != std::string::npos)
                cs.expanded = true;
            return done(std::move(m));
        },
        [&](ComposerQueuePeekNext) -> Step {
            // Alt+↓ — walk back OUT of the queue toward the live draft.
            // No-op when not peeking.
            auto& cs = m.ui.composer;
            const auto peek = cs.queue_peek_index();
            if (!peek) return done(std::move(m));
            cs.undo_coalescing = false;
            const int n = static_cast<int>(cs.queued.size());
            // Commit the current edit back into its slot first.
            if (*peek < n) {
                auto& slot = cs.queued[static_cast<std::size_t>(*peek)];
                slot.text        = std::move(cs.text);
                slot.attachments = std::move(cs.attachments);
            }
            const int next_idx = *peek + 1;
            if (next_idx >= n) {
                // Walked past the tail — restore the live draft
                // (text + chips) and leave peek mode.
                cs.browsing   = ComposerState::Live{};
                cs.text        = cs.draft_save.value_or(std::string{});
                cs.attachments = std::move(cs.draft_save_attachments);
                cs.draft_save_attachments.clear();
                cs.cursor = static_cast<int>(cs.text.size());
                cs.draft_save.reset();
                return done(std::move(m));
            }
            cs.browsing    = ComposerState::QueuePeek{next_idx};
            cs.text        = cs.queued[static_cast<std::size_t>(next_idx)].text;
            cs.attachments = cs.queued[static_cast<std::size_t>(next_idx)].attachments;
            cs.cursor      = static_cast<int>(cs.text.size());
            if (cs.text.find('\n') != std::string::npos)
                cs.expanded = true;
            return done(std::move(m));
        },
        [&](ComposerQueuePopLast) -> Step {
            // Alt+Backspace on an empty composer with no peek active
            // — "undo queue": remove the most recently queued item.
            // Useful when you've fired off a message you immediately
            // regret while the agent is still busy. The popped bytes
            // are dropped (not restored to the composer) so this is a
            // pure delete, mirroring how a real Backspace deletes
            // characters. If you want to edit it instead, Alt+↑.
            if (m.ui.composer.queued.empty()) return done(std::move(m));
            m.ui.composer.queued.pop_back();
            // If the peek index pointed at or past the dropped tail,
            // invalidate it. (Subscribe gates this Msg on the composer
            // being live, but be defensive.)
            if (const auto peek = m.ui.composer.queue_peek_index();
                peek && *peek >= static_cast<int>(m.ui.composer.queued.size())) {
                m.ui.composer.browsing = ComposerState::Live{};
                m.ui.composer.draft_save.reset();
                m.ui.composer.draft_save_attachments.clear();
            }
            return done(std::move(m));
        },
    }, cm);

    // Fold in the proactive OAuth refresh (if armed above) without
    // disturbing whatever Cmd the matched arm produced. none() short-
    // circuits the common case to zero overhead.
    if (!proactive_refresh.is_none()) {
        step.second = maya::Cmd<Msg>::batch(std::vector<maya::Cmd<Msg>>{
            std::move(step.second), std::move(proactive_refresh)});
    }
    return step;
}

} // namespace agentty::app::detail
