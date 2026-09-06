#include "agentty/io/persistence.hpp"
#include "agentty/runtime/settings_registry.hpp"

#include "agentty/util/logx.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>

#ifdef _WIN32
#  include <io.h>
#else
#  include <fcntl.h>
#  include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include "agentty/tool/util/utf8.hpp"
#include "agentty/util/base64.hpp"
#include "agentty/util/dbglog.hpp"
#include "agentty/util/home_dir.hpp"
#include "agentty/util/user_root.hpp"

namespace agentty::persistence {

namespace fs = std::filesystem;
using json = nlohmann::json;

// Atomic + durable write: write to <target>.tmp, fsync, rename. A crash
// or ctrl-C mid-write leaves the previous version intact — the loader
// never sees a truncated file that its `catch (...)` would silently drop.
// Binary mode avoids CRLF translation so the on-disk bytes match dump(2).
// Public (declared in persistence.hpp) so other JSON sidecars (ACP session
// index, etc.) share the same crash-safety guarantee.
bool write_json_atomic(const fs::path& target, const std::string& content) {
    fs::path tmp = target;
    tmp += ".tmp";
#ifdef _WIN32
    FILE* fp = ::_wfopen(tmp.wstring().c_str(), L"wb");
#else
    FILE* fp = std::fopen(tmp.c_str(), "wb");
#endif
    if (!fp) return false;
    if (std::fwrite(content.data(), 1, content.size(), fp) != content.size()) {
        std::fclose(fp);
        std::error_code ec; fs::remove(tmp, ec);
        return false;
    }
    std::fflush(fp);
#ifdef _WIN32
    (void)::_commit(::_fileno(fp));
#else
    (void)::fsync(::fileno(fp));
#endif
    if (std::fclose(fp) != 0) {
        std::error_code ec; fs::remove(tmp, ec);
        return false;
    }
    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) {
        std::error_code ec2; fs::remove(tmp, ec2);
        return false;
    }
    // fsync the parent directory so the rename's dentry itself survives a
    // crash/power-loss. Without this the file content is durable (we fsync'd
    // the fd above) but the directory entry that publishes it at `target`
    // may not be — the file can vanish after recovery. Mirrors the hardened
    // atomic write in tool/util/fs_helpers.cpp.
#ifndef _WIN32
    if (fs::path parent = target.parent_path(); !parent.empty()) {
        int dfd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dfd >= 0) { (void)::fsync(dfd); ::close(dfd); }
    }
#endif
    return true;
}

fs::path data_dir() {
    // The single per-user root (~/.agentty or $AGENTTY_HOME) — see
    // util/user_root.hpp for the layout and the reason it is NOT under
    // ~/.config. user_root() creates it 0700 and runs the one-time
    // legacy-config migration.
    fs::path p = util::user_root();
    std::error_code ec;
    // Surface a persistent-storage failure once. Silently swallowing it
    // meant threads/settings/memory writes became no-ops with zero
    // feedback (read-only $HOME, full disk, EACCES). One warning to
    // stderr is enough — it prints before maya takes the screen, and
    // the static guard keeps it from spamming on every save.
    if (p.empty() || !fs::is_directory(p, ec)) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::fprintf(stderr,
                "agentty: warning: cannot create data dir '%s' (%s) — "
                "threads and settings will not persist this session\n",
                p.string().c_str(), ec.message().c_str());
        }
    }
    return p;
}

fs::path threads_dir() {
    auto p = data_dir() / "threads";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

static std::string role_to_string(Role r);

namespace {

// Transcript budgets. The transcript is a fork's on-disk memory of its
// parent: the model READS it on demand (paginated / greppable), so it must
// stay a bounded, useful artifact even when the parent thread is enormous
// (a 1M-token agentic run). Two independent caps:
//
//   • kMaxMsgTextBytes  — one giant pasted/emitted `text` block can't
//     dominate; it's clipped head+tail with a "… N bytes elided …" marker.
//   • kMaxTranscriptBytes — total output ceiling. When the whole thread
//     doesn't fit we keep the MOST RECENT turns (the ones a fork is most
//     likely to need) and drop the oldest, noting how many we elided.
//
// Bounding the OUTPUT also bounds peak memory: the in-RAM string is at most
// ~kMaxTranscriptBytes, so there's no unbounded ostringstream on a fork of a
// runaway thread. Tool OUTPUT is never written (only name + a 120-char arg
// hint), which already strips the heaviest bytes of a long thread.
constexpr std::size_t kMaxMsgTextBytes     = 16 * 1024;    // 16 KB / message
constexpr std::size_t kMaxTranscriptBytes  = 512 * 1024;   // 512 KB total

// Clip a text block to a byte budget, keeping a head and a tail (the ends
// carry the most signal — a question's ask + its conclusion) and marking
// the gap. UTF-8-safe: cuts land on codepoint boundaries so the .md never
// contains a truncated multibyte sequence.
std::string clip_text(const std::string& s, std::size_t budget) {
    if (s.size() <= budget) return s;
    const std::size_t head = budget * 3 / 4;       // 75% head, 25% tail
    const std::size_t tail = budget - head;
    const std::size_t hcut = tools::util::safe_utf8_cut(s, head);
    // Tail start: back off `tail` bytes from the end, then forward to the
    // next codepoint boundary so we never begin mid-sequence.
    std::size_t tstart = s.size() > tail ? s.size() - tail : 0;
    while (tstart < s.size() && (static_cast<unsigned char>(s[tstart]) & 0xC0) == 0x80)
        ++tstart;
    const std::size_t elided = tstart > hcut ? tstart - hcut : 0;
    std::string out;
    out.reserve(hcut + (s.size() - tstart) + 48);
    out.append(s, 0, hcut);
    out.append("\n… [").append(std::to_string(elided)).append(" bytes elided] …\n");
    out.append(s, tstart, std::string::npos);
    return out;
}

// Render ONE message to its transcript chunk (header + clipped text +
// collapsed tool lines). Synthetic view-only cards with no content
// (smart_routing) are skipped entirely — they'd be empty noise. Returns an
// empty string for a message that contributes nothing.
std::string render_message_md(const Message& m) {
    if (m.smart_routing) return {};   // zero-content routing telemetry
    std::string chunk = "## ";
    chunk += role_to_string(m.role);
    chunk += '\n';
    bool any = false;
    if (!m.text.empty()) {
        chunk += clip_text(tools::util::to_valid_utf8(m.text), kMaxMsgTextBytes);
        chunk += '\n';
        any = true;
    }
    for (const auto& tc : m.tool_calls) {
        chunk += "› tool(";
        chunk += tc.name.value;
        chunk += ')';
        if (!tc.args.is_null()) {
            std::string a = tc.args.dump();
            if (a.size() > 120) { a.resize(tools::util::safe_utf8_cut(a, 120)); a += "…"; }
            chunk += ' ';
            chunk += a;
        }
        chunk += '\n';
        any = true;
    }
    if (!any) return {};   // e.g. an empty assistant placeholder
    chunk += '\n';
    return chunk;
}

} // namespace

fs::path write_thread_transcript_md(const Thread& t) {
    // Clean, BOUNDED transcript: "## user" / "## assistant" headers + the
    // (clipped) text, tool calls collapsed to a single `› tool(name)` line.
    // None of the <id>.json noise. Small and greppable so a fork can `read`
    // it cheaply; recency-biased + size-capped so even a huge parent thread
    // yields a useful artifact instead of a multi-MB file.
    //
    // Two-pass for recency bias: render newest→oldest, accumulating until the
    // total budget is hit, then emit the kept slice oldest→newest (natural
    // reading order) with an elision marker if we dropped the oldest turns.
    std::vector<std::string> kept;   // newest-first while building
    std::size_t used = 0;
    std::size_t kept_count = 0;
    bool truncated = false;
    for (auto it = t.messages.rbegin(); it != t.messages.rend(); ++it) {
        std::string chunk = render_message_md(*it);
        if (chunk.empty()) continue;
        if (used + chunk.size() > kMaxTranscriptBytes && !kept.empty()) {
            // Budget hit and we already have at least the newest turn — stop.
            // (The `!kept.empty()` guard guarantees we ALWAYS keep the most
            // recent contentful message even if it alone exceeds the budget;
            // its own text was already clipped to kMaxMsgTextBytes.)
            truncated = true;
            break;
        }
        used += chunk.size();
        ++kept_count;
        kept.push_back(std::move(chunk));
    }

    std::string md;
    md.reserve(used + 256);
    md += "# Transcript: ";
    md += (t.title.empty() ? t.id.value : t.title);
    md += '\n';
    md += "# (";
    md += std::to_string(t.messages.size());
    md += " messages total";
    if (truncated) {
        md += "; showing the ";
        md += std::to_string(kept_count);
        md += " most recent — read the parent thread for older turns";
    }
    md += "; read/grep as needed)\n\n";
    if (truncated) {
        md += "_[… older turns elided to keep this transcript bounded; the ";
        md += "newest ";
        md += std::to_string(kept_count);
        md += " messages follow …]_\n\n";
    }
    // Emit oldest→newest (reverse of the newest-first `kept`).
    for (auto it = kept.rbegin(); it != kept.rend(); ++it) md += *it;

    // Write next to the thread files under a stable, discoverable name.
    const fs::path out = threads_dir() / (t.id.value + ".transcript.md");
    if (!write_json_atomic(out, md)) return {};
    return out;
}

static std::string role_to_string(Role r) {
    switch (r) {
        case Role::User: return "user";
        case Role::Assistant: return "assistant";
        case Role::System: return "system";
    }
    return "user";
}
static Role role_from_string(const std::string& s) {
    if (s == "assistant") return Role::Assistant;
    if (s == "system")    return Role::System;
    return Role::User;
}

static json message_to_json(const Message& m) {
    // Belt-and-suspenders UTF-8 scrub. Tool output and freeform text can
    // contain raw bytes from arbitrary files (Latin-1 .htm, Shift-JIS logs)
    // that nlohmann::json::dump() refuses to serialise — it throws
    // type_error.316 and we used to terminate(). Scrub at the boundary so
    // bad bytes can never reach dump(). Tools that already scrub upstream
    // pay only the validate cost here.
    json j;
    // Round-trip the per-message stable id so on-disk → in-memory
    // reload preserves cache keys across sessions. Generated fresh
    // when missing on load (see parse_message) so older threads upgrade
    // transparently — no migration step needed.
    j["id"] = m.id.value;
    j["role"] = role_to_string(m.role);
    j["text"] = tools::util::to_valid_utf8(m.text);
    j["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
        m.timestamp.time_since_epoch()).count();
    json tcs = json::array();
    for (const auto& tc : m.tool_calls) {
        json t;
        t["id"] = tc.id;
        t["name"] = tc.name;
        t["args"] = tc.args;
        t["output"] = tools::util::to_valid_utf8(tc.output()); // empty unless terminal
        t["status"] = std::string{tc.status_name()};
        tcs.push_back(std::move(t));
    }
    j["tool_calls"] = std::move(tcs);
    // Image attachments on User messages — stored on disk as base64
    // so a thread reload can be re-sent on a follow-up turn without
    // having to re-paste the original. Adds ~33% to the message JSON
    // size; absent for messages that didn't carry images, so non-
    // image threads are unaffected.
    if (!m.images.empty()) {
        json imgs = json::array();
        for (const auto& img : m.images) {
            json e;
            e["media_type"] = img.media_type;
            e["data"]       = util::base64_encode(img.bytes);
            imgs.push_back(std::move(e));
        }
        j["images"] = std::move(imgs);
    }
    if (m.checkpoint_id) j["checkpoint_id"] = *m.checkpoint_id;
    // Persist the per-message error so reopening a thread shows which
    // turn died and why. UTF-8 scrubbed for the same reason as `text`.
    if (m.error) j["error"] = tools::util::to_valid_utf8(*m.error);
    if (m.is_compact_summary) j["is_compact_summary"] = true;
    // Proactive-retrieval marker + the confidence that gated it. Persisted
    // so a reloaded thread still renders the quiet "Retrieved context" card
    // (with its source list + confidence bar) instead of surfacing the raw
    // <retrieved-context> block as if the user had typed it.
    if (m.proactive) {
        j["proactive_context"] = true;
        if (m.proactive->confidence)
            j["proactive_confidence"] = *m.proactive->confidence;
    }
    // Fork provenance card. Persisted so a reloaded fork still renders the
    // "\u2443 Forked" event card and the model still sees the transcript pointer
    // (it's a real wire User message, so it must round-trip like one).
    if (m.fork_note) {
        j["fork_note"] = true;
        if (!m.fork_transcript.empty())
            j["fork_transcript"] = tools::util::to_valid_utf8(m.fork_transcript);
    }
    // Turn provenance: the model that ACTUALLY served this turn (Smart Mode
    // routes it away from the picker selection) and the role it played.
    // Only written when set, so non-Smart-Mode threads gain no bytes.
    if (!m.served_model.empty())
        j["served_model"] = m.served_model;
    if (!m.served_role.empty())
        j["served_role"] = m.served_role;
    // Adaptive-thinking block (Assistant turns under an effort setting).
    // Persisted so a reloaded thread can replay it on a follow-up turn —
    // Anthropic 400s a tool_use turn whose thinking block was dropped.
    if (!m.thinking.empty())
        j["thinking"] = tools::util::to_valid_utf8(m.thinking);
    if (!m.thinking_signature.empty())
        j["thinking_signature"] = m.thinking_signature;
    // Reasoning duration (ms) for the settled "· 3.2s" header meter.
    if (m.reasoning_ms > 0)
        j["reasoning_ms"] = m.reasoning_ms;
    // Per-block (text, signature) pairs — the authoritative replay source
    // when interleaved thinking produced several signed blocks. The legacy
    // pair above stays for older-binary compat.
    if (!m.thinking_blocks.empty()) {
        json blocks = json::array();
        for (const auto& tb : m.thinking_blocks) {
            json b{{"text", tools::util::to_valid_utf8(tb.text)},
                   {"signature", tb.signature}};
            if (!tb.redacted_data.empty()) b["redacted_data"] = tb.redacted_data;
            blocks.push_back(std::move(b));
        }
        j["thinking_blocks"] = std::move(blocks);
    }
    // Legacy visible-reasoning fallback (paths that populate
    // reasoning_summary directly, e.g. external ACP backends). Without this
    // the reasoning block vanishes from a reloaded thread.
    if (!m.reasoning_summary.empty())
        j["reasoning_summary"] = tools::util::to_valid_utf8(m.reasoning_summary);
    // Codex/Responses encrypted reasoning blob(s). Persisted so a reloaded
    // thread can still replay chain-of-thought across tool rounds. Opaque
    // base64-ish ciphertext (ASCII), so no UTF-8 scrub needed.
    if (!m.reasoning_encrypted.empty())
        j["reasoning_encrypted"] = m.reasoning_encrypted;
    // Non-image attachments (Paste / FileRef / Symbol). Persisted so a
    // reloaded thread can rebuild its wire payload — the user's `text`
    // carries chip placeholders, and the model only sees real content
    // after `attachment::expand(...)` splices the bodies back in at
    // request-build time. Body bytes are base64-encoded since pasted
    // text can contain anything (NULs, lone surrogates, control bytes
    // that the UTF-8 scrub would otherwise mangle).
    if (!m.attachments.empty()) {
        json atts = json::array();
        for (const auto& a : m.attachments) {
            json e;
            switch (a.kind) {
                case Attachment::Kind::Paste:   e["kind"] = "paste";   break;
                case Attachment::Kind::FileRef: e["kind"] = "fileref"; break;
                case Attachment::Kind::Symbol:  e["kind"] = "symbol";  break;
                case Attachment::Kind::Image:   e["kind"] = "image";   break;
                case Attachment::Kind::Output:  e["kind"] = "output";  break;
            }
            e["body"]        = util::base64_encode(a.body);
            if (!a.path.empty())       e["path"]        = a.path;
            if (!a.media_type.empty()) e["media_type"] = a.media_type;
            if (!a.name.empty())       e["name"]        = a.name;
            if (a.line_number > 0)     e["line_number"] = a.line_number;
            e["line_count"] = a.line_count;
            e["byte_count"] = a.byte_count;
            atts.push_back(std::move(e));
        }
        j["attachments"] = std::move(atts);
    }
    return j;
}

// ── Typed deserializers ──────────────────────────────────────────────────
// One source of truth for "what does a valid Thread JSON look like."
// Required fields fail with `MissingField`; wrong-type fields fail with
// `InvalidValue`; unrecognised discriminators fail with `InvalidVariantTag`.
// Optional fields fall back to defaults silently (timestamps, error strings)
// — those are recoverable; missing them shouldn't kill the whole thread.

std::string DeserializeError::render() const {
    static constexpr std::string_view kind_str[] = {
        "json_parse", "missing_field", "invalid_value",
        "invalid_variant_tag", "io",
    };
    // Pin the table to the enum: adding a DeserializeErrorKind arm without a
    // matching row is a COMPILE error, not a silent out-of-bounds read. `Io`
    // is the last arm, so its underlying value + 1 is the arm count.
    static_assert(std::size(kind_str)
                      == std::to_underlying(DeserializeErrorKind::Io) + 1u,
                  "kind_str is out of sync with DeserializeErrorKind — "
                  "add the missing row");
    std::string out = "[";
    out += kind_str[std::to_underlying(kind)];
    out += "] ";
    if (!field.empty()) { out += field; out += ": "; }
    out += detail;
    return out;
}

static std::expected<ToolUse::Status, DeserializeError>
parse_tool_status(std::string_view status_tag, std::string&& output) {
    // Reconstruct the variant. Persisted threads only ever land in
    // terminal states (in-flight tools are never serialized), so the
    // intermediate states reset to a no-arg-time-stamp default.
    if (status_tag == "done")
        return ToolUse::Status{ToolUse::Done{{}, {}, std::move(output)}};
    if (status_tag == "failed" || status_tag == "error")
        return ToolUse::Status{ToolUse::Failed{{}, {}, std::move(output)}};
    if (status_tag == "rejected") return ToolUse::Status{ToolUse::Rejected{{}}};
    // A persisted thread SHOULD only carry terminal tool states, but a
    // session killed mid-tool (crash, SIGKILL, power loss) leaves a
    // pending/running/approved tool on disk. Such a tool never
    // completed and never will — coerce it to a terminal Failed state
    // so the run is freezable/renderable on resume (run_is_freezable
    // refuses any non-terminal tool, which would otherwise drop the
    // whole trailing run from the rehydrated transcript).
    if (status_tag == "running" || status_tag == "approved"
        || status_tag == "pending") {
        std::string note = output.empty() ? "interrupted" : std::move(output);
        return ToolUse::Status{ToolUse::Failed{{}, {}, std::move(note)}};
    }
    return std::unexpected(DeserializeError{
        DeserializeErrorKind::InvalidVariantTag, "tool_calls[*].status",
        std::string{"unknown status tag: "} + std::string{status_tag}});
}

static std::expected<Message, DeserializeError> parse_message(const json& j) {
    if (!j.is_object())
        return std::unexpected(DeserializeError{
            DeserializeErrorKind::InvalidValue, "messages[*]",
            "expected object"});
    Message m;
    // `id` was added in 2026-05; older thread files don't carry it,
    // so the default-constructed `m.id` (already-fresh from
    // new_message_id()) stands in for them. New writes will persist
    // this fresh id, so a save-after-load completes the migration.
    if (auto it = j.find("id"); it != j.end() && it->is_string()
        && !it->get<std::string>().empty())
        m.id = MessageId{it->get<std::string>()};
    m.role = role_from_string(j.value("role", "user"));
    m.text = j.value("text", "");
    // Turn provenance (which model/role actually served it). Absent on
    // threads written before the field existed — the view falls back to the
    // live selection, which is what those turns used to render anyway.
    m.served_model = j.value("served_model", "");
    m.served_role  = j.value("served_role", "");
    m.thinking = j.value("thinking", "");
    m.thinking_signature = j.value("thinking_signature", "");
    m.reasoning_ms = j.value("reasoning_ms", static_cast<std::int64_t>(0));
    if (auto it = j.find("thinking_blocks"); it != j.end() && it->is_array())
        for (const auto& tb : *it)
            if (tb.is_object())
                m.thinking_blocks.push_back(Message::ThinkingBlock{
                    tb.value("text", ""), tb.value("signature", ""),
                    tb.value("redacted_data", "")});
    m.reasoning_summary = j.value("reasoning_summary", "");
    m.reasoning_encrypted = j.value("reasoning_encrypted", "");
    if (auto it = j.find("error"); it != j.end() && it->is_string()
        && !it->get<std::string>().empty())
        m.error = it->get<std::string>();
    if (j.contains("timestamp")) {
        const auto& ts = j["timestamp"];
        if (!ts.is_number_integer())
            return std::unexpected(DeserializeError{
                DeserializeErrorKind::InvalidValue, "messages[*].timestamp",
                "expected integer seconds-since-epoch"});
        m.timestamp = std::chrono::system_clock::time_point{
            std::chrono::seconds{ts.get<long long>()}};
    }
    if (j.contains("tool_calls")) {
        const auto& arr = j["tool_calls"];
        if (!arr.is_array())
            return std::unexpected(DeserializeError{
                DeserializeErrorKind::InvalidValue, "messages[*].tool_calls",
                "expected array"});
        for (const auto& t : arr) {
            ToolUse tc;
            tc.id = ToolCallId{t.value("id", "")};
            tc.name = ToolName{t.value("name", "")};
            tc.args = t.value("args", json::object());
            // Old persisted threads stored status as an int enum; new ones
            // use the string tag returned by ToolUse::status_name(). Accept
            // both so existing on-disk threads keep loading.
            std::string status_tag = "pending";
            std::string output = t.value("output", "");
            if (auto it = t.find("status"); it != t.end()) {
                if (it->is_string()) {
                    status_tag = it->get<std::string>();
                } else if (it->is_number()) {
                    static constexpr std::string_view legacy[] = {
                        "pending","approved","running","done","failed","rejected"};
                    int idx = it->get<int>();
                    status_tag = idx >= 0 && idx < static_cast<int>(std::size(legacy))
                        ? std::string{legacy[idx]} : std::string{"pending"};
                }
            }
            auto status = parse_tool_status(status_tag, std::move(output));
            if (!status) return std::unexpected(std::move(status).error());
            tc.status = std::move(*status);
            m.tool_calls.push_back(std::move(tc));
        }
    }
    if (j.contains("checkpoint_id")) {
        const auto& cp = j["checkpoint_id"];
        if (!cp.is_string())
            return std::unexpected(DeserializeError{
                DeserializeErrorKind::InvalidValue, "messages[*].checkpoint_id",
                "expected string"});
        m.checkpoint_id = CheckpointId{cp.get<std::string>()};
    }
    if (j.contains("is_compact_summary")) {
        const auto& v = j["is_compact_summary"];
        if (v.is_boolean()) m.is_compact_summary = v.get<bool>();
    }
    // The on-disk shape is nested (confidence only inside a proactive
    // message), and now the in-memory shape matches it. Reading them as two
    // independent keys used to allow a stray `proactive_confidence` with no
    // `proactive_context` to land a confidence on an ordinary message; the
    // grouping makes that unrepresentable rather than merely unlikely.
    if (auto it = j.find("proactive_context");
        it != j.end() && it->is_boolean() && it->get<bool>()) {
        Message::ProactiveContext pc;
        if (auto ct = j.find("proactive_confidence");
            ct != j.end() && ct->is_number())
            pc.confidence = ct->get<double>();
        m.proactive = std::move(pc);
    }
    if (auto it = j.find("fork_note");
        it != j.end() && it->is_boolean())
        m.fork_note = it->get<bool>();
    if (auto it = j.find("fork_transcript");
        it != j.end() && it->is_string())
        m.fork_transcript = it->get<std::string>();
    if (j.contains("images")) {
        const auto& arr = j["images"];
        if (!arr.is_array())
            return std::unexpected(DeserializeError{
                DeserializeErrorKind::InvalidValue, "messages[*].images",
                "expected array"});
        for (const auto& e : arr) {
            if (!e.is_object()) continue;
            ImageContent img;
            img.media_type = e.value("media_type", "image/png");
            auto data_b64 = e.value("data", std::string{});
            img.bytes = util::base64_decode(data_b64);
            // Drop entries that decode to nothing — corrupted base64
            // shouldn't kill the whole thread load.
            if (!img.bytes.empty()) m.images.push_back(std::move(img));
        }
    }
    if (j.contains("attachments")) {
        const auto& arr = j["attachments"];
        if (!arr.is_array())
            return std::unexpected(DeserializeError{
                DeserializeErrorKind::InvalidValue, "messages[*].attachments",
                "expected array"});
        for (const auto& e : arr) {
            if (!e.is_object()) continue;
            Attachment a;
            auto kind = e.value("kind", std::string{"paste"});
            if      (kind == "paste")   a.kind = Attachment::Kind::Paste;
            else if (kind == "fileref") a.kind = Attachment::Kind::FileRef;
            else if (kind == "symbol")  a.kind = Attachment::Kind::Symbol;
            else if (kind == "image")   a.kind = Attachment::Kind::Image;
            else if (kind == "output")  a.kind = Attachment::Kind::Output;
            else                        a.kind = Attachment::Kind::Paste;
            a.body        = util::base64_decode(e.value("body", std::string{}));
            a.path        = e.value("path", std::string{});
            a.media_type  = e.value("media_type", std::string{});
            a.name        = e.value("name", std::string{});
            a.line_number = e.value("line_number", 0);
            a.line_count  = e.value("line_count", std::size_t{0});
            a.byte_count  = e.value("byte_count", std::size_t{0});
            m.attachments.push_back(std::move(a));
        }
    }
    return m;
}

// Populates id/title/timestamps only; leaves messages empty. Used by the
// directory-walking metadata load that backs the thread picker.
static std::expected<Thread, DeserializeError>
parse_thread_meta_only(const json& j) {
    if (!j.is_object())
        return std::unexpected(DeserializeError{
            DeserializeErrorKind::InvalidValue, "", "expected top-level object"});
    Thread t;
    auto id_str = j.value("id", "");
    if (id_str.empty())
        return std::unexpected(DeserializeError{
            DeserializeErrorKind::MissingField, "id",
            "thread JSON has no `id` field"});
    t.id = ThreadId{std::move(id_str)};
    t.title = j.value("title", "");
    t.forked_from = j.value("forked_from", "");
    // On-disk form is unchanged: key absent = inherit, else the RagMode's
    // integer. Anything outside the enum is treated as "inherit" rather than
    // cast blindly — a corrupt or future value must not become a mode.
    if (const auto it = j.find("rag_mode_override");
        it != j.end() && it->is_number_integer()) {
        const int v = it->get<int>();
        if (v >= static_cast<int>(store::RagMode::On)
            && v <= static_cast<int>(store::RagMode::Off))
            t.rag_mode_override = static_cast<store::RagMode>(v);
    }
    if (j.contains("created_at"))
        t.created_at = std::chrono::system_clock::time_point{
            std::chrono::seconds{j["created_at"].get<long long>()}};
    if (j.contains("updated_at"))
        t.updated_at = std::chrono::system_clock::time_point{
            std::chrono::seconds{j["updated_at"].get<long long>()}};
    return t;
}

static std::expected<Thread, DeserializeError> parse_thread(const json& j) {
    auto meta = parse_thread_meta_only(j);
    if (!meta) return meta;
    Thread t = std::move(*meta);
    for (const auto& mj : j.value("messages", json::array())) {
        auto msg = parse_message(mj);
        if (!msg) return std::unexpected(std::move(msg).error());
        t.messages.push_back(std::move(*msg));
    }
    // Compactions: optional. Missing on threads from before the feature
    // existed and on threads that simply haven't been compacted yet —
    // both indistinguishable from on-disk and both correctly default to
    // an empty vector. Per-field tolerance is intentional: a malformed
    // record (e.g. negative `up_to_index`) is skipped rather than
    // failing the whole load, because the wire-substitution helper
    // already validates `up_to_index <= messages.size()` and gracefully
    // falls back to "no compaction" when it doesn't — worst case the
    // user re-compacts manually, vs. losing the entire thread.
    if (j.contains("compactions") && j["compactions"].is_array()) {
        for (const auto& cj : j["compactions"]) {
            if (!cj.is_object()) continue;
            Thread::CompactionRecord rec;
            if (cj.contains("up_to_index") && cj["up_to_index"].is_number_integer()) {
                auto v = cj["up_to_index"].get<long long>();
                if (v < 0) continue;
                rec.up_to_index = static_cast<std::size_t>(v);
            } else {
                continue;
            }
            if (cj.contains("summary") && cj["summary"].is_string()) {
                rec.summary = cj["summary"].get<std::string>();
            }
            if (cj.contains("created_at") && cj["created_at"].is_number_integer()) {
                rec.created_at = std::chrono::system_clock::time_point{
                    std::chrono::seconds{cj["created_at"].get<int64_t>()}};
            }
            // Discard records whose boundary doesn't fit the loaded
            // transcript — typically only happens when a save was
            // interrupted mid-compaction.
            if (rec.up_to_index <= t.messages.size()) {
                t.compactions.push_back(std::move(rec));
            }
        }
    }
    return t;
}

// SAX handler that pulls the four top-level metadata fields out of a thread
// JSON file without ever materialising the messages array. The directory
// walk runs this once per file at startup; with 649 files at ~580 KB each
// the previous tree-build path peaked at hundreds of MB of intermediate
// json::value_t allocations *and* left the converted Thread::messages live
// forever. SAX gives us O(file_bytes) parse cost with O(1) live state per
// file: a few dozen bytes for the metadata fields plus depth tracking.
//
// Note the on-disk key order is alphabetical (json::dump(2) sorts keys),
// so the layout is `created_at, id, messages, title, updated_at` — i.e.
// `title` and `updated_at` arrive *after* the messages array. The skip
// state must therefore unwind cleanly when the array closes, otherwise
// those two fields are silently lost.
struct ThreadMetaSax {
    Thread out;
    std::string last_key;
    int depth = 0;        // current nesting depth inside the JSON
    int skip_target = -1; // -1 == not skipping; otherwise resume when depth <= skip_target
    bool got_id = false;

    bool skipping() const noexcept { return skip_target >= 0; }

    bool key(std::string& v) {
        last_key = std::move(v);
        return true;
    }
    bool string(std::string& v) {
        if (!skipping() && depth == 1) {
            if (last_key == "id") {
                if (v.empty()) return false; // hard fail — caller treats as parse error
                out.id = ThreadId{std::move(v)};
                got_id = true;
            } else if (last_key == "title") {
                out.title = std::move(v);
            }
        }
        return true;
    }
    bool number_integer(std::int64_t v)            { return num(static_cast<long long>(v)); }
    bool number_unsigned(std::uint64_t v)          { return num(static_cast<long long>(v)); }
    bool number_float(double, const std::string&)  { return true; }
    bool num(long long v) {
        if (!skipping() && depth == 1) {
            if (last_key == "created_at")
                out.created_at = std::chrono::system_clock::time_point{
                    std::chrono::seconds{v}};
            else if (last_key == "updated_at")
                out.updated_at = std::chrono::system_clock::time_point{
                    std::chrono::seconds{v}};
        }
        return true;
    }
    bool boolean(bool)             { return true; }
    bool null()                    { return true; }
    bool start_object(std::size_t) { return enter_container(); }
    bool end_object()              { return leave_container(); }
    bool start_array(std::size_t)  { return enter_container(); }
    bool end_array()               { return leave_container(); }
    bool binary(json::binary_t&)   { return true; }
    bool parse_error(std::size_t, const std::string&,
                     const json::exception&) { return false; }

    bool enter_container() {
        // If we're at the top-level object (depth==1) and the latest key
        // was "messages", arm the skip: we'll resume when depth comes
        // back down to 1. The ++depth must come AFTER this check so that
        // nested containers inside the messages array don't re-arm it.
        if (!skipping() && depth == 1 && last_key == "messages")
            skip_target = 1;
        ++depth;
        return true;
    }
    bool leave_container() {
        --depth;
        if (skipping() && depth == skip_target)
            skip_target = -1;
        return true;
    }
};

[[nodiscard]] static std::expected<Thread, DeserializeError>
load_thread_meta_file(const fs::path& p) {
    std::ifstream ifs(p);
    if (!ifs) return std::unexpected(DeserializeError{
        DeserializeErrorKind::Io, "", "open failed: " + p.string()});
    ThreadMetaSax sax;
    bool ok = json::sax_parse(ifs, &sax);
    if (!ok || !sax.got_id) {
        return std::unexpected(DeserializeError{
            DeserializeErrorKind::JsonParse, "",
            "metadata sax parse failed: " + p.string()});
    }
    return std::move(sax.out);
}

std::expected<Thread, DeserializeError>
load_thread_file(const std::filesystem::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return std::unexpected(DeserializeError{
        DeserializeErrorKind::Io, "", "open failed: " + p.string()});
    // Slurp the whole file into one string, then parse from contiguous
    // memory. nlohmann's istream_iterator path reads the DOM one char at
    // a time through the stream's locale/sentry machinery — measurably
    // slower on multi-MB transcripts (a 12 MB thread parsed ~110 ms that
    // way, blocking the reducer on thread-switch). A single sized read +
    // json::parse over the contiguous buffer roughly halves it.
    std::string buf;
    {
        std::error_code ec;
        auto sz = fs::file_size(p, ec);
        if (!ec && sz > 0) {
            buf.resize(static_cast<std::size_t>(sz));
            ifs.read(buf.data(), static_cast<std::streamsize>(sz));
            // A short read (file shrank between stat and read) leaves the
            // tail uninitialised — trim to what we actually got so we
            // never hand json::parse stale bytes.
            buf.resize(static_cast<std::size_t>(ifs.gcount()));
        } else {
            // size_t unknown (pipe/procfs/stat error) — fall back to a
            // streaming slurp.
            buf.assign(std::istreambuf_iterator<char>(ifs),
                       std::istreambuf_iterator<char>());
        }
    }
    json j;
    try { j = json::parse(buf); }
    catch (const std::exception& e) {
        return std::unexpected(DeserializeError{
            DeserializeErrorKind::JsonParse, "",
            std::string{"json parse failed: "} + e.what()});
    }
    return parse_thread(j);
}

// ── Thread metadata index sidecar ────────────────────────────────────
//
// The picker only needs id + title + created_at + updated_at, but those
// keys live AFTER the multi-MB `compactions` / `messages` arrays in each
// thread file, so the metadata-only SAX walk still streams every byte of
// every file to reach them — ~1 s for a 247-thread / 281 MB history.
//
// `index.json` caches that metadata in one small file: a map of
// id → {title, created_at, updated_at, mtime}. On load we read the index
// and trust an entry whose recorded mtime still matches the thread
// file's on-disk mtime; only files that are new or changed since the
// index was written get the full per-file SAX parse (and refresh the
// index). Delete index.json and everything self-heals via the cold
// path. This turns the warm startup into one small read + N stat()s.
namespace {

fs::path thread_index_path() { return threads_dir() / "index.json"; }

// index.json is read-modify-written from TWO threads: the AsyncWriter
// worker (reindex_thread, after each save) and the UI/reducer thread
// (delete_thread, load_all_threads). Without a lock the two interleave
// as a classic lost update — both read the same map, both write their
// own stale copy, and whichever lands second silently drops the other's
// entry. write_json_atomic's rename means the file never CORRUPTS, so
// the damage shows up as a ghost row in the picker (a deleted thread
// that reappears) or a stale title, and it persists until the next cold
// rebuild. One process-wide mutex held across the whole read-modify-
// write closes it. Contention is nil: these are startup + per-save.
std::mutex& thread_index_mu() {
    static std::mutex m;
    return m;
}

struct IndexEntry {
    std::string title;
    long long   created_at = 0;   // unix seconds
    long long   updated_at = 0;   // unix seconds
    long long   mtime      = 0;   // file last_write_time, ns since clock epoch
    long long   size       = -1;  // file size in bytes; -1 == unknown (pre-v2)
};

long long file_mtime_ns(const fs::path& p) {
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    if (ec) return 0;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               t.time_since_epoch()).count();
}

// Companion to file_mtime_ns for the freshness check. mtime alone is not
// sufficient: mtime granularity is filesystem-dependent (FAT 2 s, HFS+
// 1 s), so a thread rewritten within the same tick as its recorded stamp
// compares equal and the cache serves the OLD title/updated_at forever —
// nothing else ever invalidates it. Pairing mtime with size catches every
// same-tick edit that changes the file's length, which for a JSON
// transcript that just grew a message is essentially all of them. This
// mirrors the mtime+size rule fs_helpers.cpp already uses for its
// StaleVerdict check. Returns -1 on error so it can't alias a real size.
long long file_size_bytes(const fs::path& p) {
    std::error_code ec;
    auto s = fs::file_size(p, ec);
    if (ec) return -1;
    return static_cast<long long>(s);
}

// Raw index read. PRECONDITION: caller holds thread_index_mu(). The
// read and the matching write must sit inside ONE critical section or
// the lost-update race described above reopens.
std::unordered_map<std::string, IndexEntry> read_thread_index_locked() {
    std::unordered_map<std::string, IndexEntry> out;
    std::ifstream ifs(thread_index_path());
    if (!ifs) return out;
    try {
        json j; ifs >> j;
        if (!j.is_object()) return out;
        auto& threads = j.contains("threads") ? j["threads"] : j;
        if (!threads.is_object()) return out;
        for (auto& [id, e] : threads.items()) {
            if (!e.is_object()) continue;
            IndexEntry ie;
            ie.title      = e.value("title", std::string{});
            ie.created_at = e.value("created_at", 0LL);
            ie.updated_at = e.value("updated_at", 0LL);
            ie.mtime      = e.value("mtime", 0LL);
            // Absent in v1 indexes — -1 means "unknown", which the
            // freshness check treats as a miss so the entry gets
            // re-parsed once and upgraded to a v2 entry with a size.
            ie.size       = e.value("size", -1LL);
            out.emplace(id, std::move(ie));
        }
    } catch (const std::exception&) {
        // Corrupt index — treat as empty; the cold path rebuilds it.
        out.clear();
    }
    return out;
}

// Raw index write. PRECONDITION: caller holds thread_index_mu().
void write_thread_index_locked(const std::unordered_map<std::string, IndexEntry>& idx) {
    json threads = json::object();
    for (const auto& [id, e] : idx) {
        json ej;
        ej["title"]      = e.title;
        ej["created_at"] = e.created_at;
        ej["updated_at"] = e.updated_at;
        ej["mtime"]      = e.mtime;
        ej["size"]       = e.size;
        threads[id] = std::move(ej);
    }
    json j;
    j["version"] = 2;
    j["threads"] = std::move(threads);
    try { (void)write_json_atomic(thread_index_path(), j.dump()); }
    catch (const std::exception&) { /* best-effort cache */ }
}

Thread thread_from_index(const std::string& id, const IndexEntry& e) {
    Thread t;
    t.id    = ThreadId{id};
    t.title = e.title;
    t.created_at = std::chrono::system_clock::time_point{
        std::chrono::seconds{e.created_at}};
    t.updated_at = std::chrono::system_clock::time_point{
        std::chrono::seconds{e.updated_at}};
    return t;
}

// Refresh a single thread's index entry after its file is (re)written,
// stamping the NEW on-disk mtime so the next startup takes the fast
// path for this thread instead of re-parsing it. Best-effort: a failed
// index update just means one slow parse next launch, then self-heals.
void reindex_thread(const Thread& t) {
    const auto file = threads_dir() / (t.id.value + ".json");
    // Stat OUTSIDE the lock, then read-modify-write inside it, so the
    // whole update is atomic with respect to delete_thread().
    const long long mt = file_mtime_ns(file);
    const long long sz = file_size_bytes(file);
    std::lock_guard<std::mutex> lk(thread_index_mu());
    auto idx = read_thread_index_locked();
    IndexEntry ie;
    ie.title      = t.title;
    ie.created_at = std::chrono::duration_cast<std::chrono::seconds>(
                        t.created_at.time_since_epoch()).count();
    ie.updated_at = std::chrono::duration_cast<std::chrono::seconds>(
                        t.updated_at.time_since_epoch()).count();
    ie.mtime      = mt;
    ie.size       = sz;
    idx[t.id.value] = std::move(ie);
    write_thread_index_locked(idx);
}

} // namespace

std::vector<Thread> load_all_threads() {
    // Metadata-only directory walk, index-accelerated (see the index
    // sidecar note above). A cached entry whose mtime still matches the
    // file's on-disk mtime is used verbatim — no open/parse. Only new or
    // changed files get the full SAX meta parse, and the index is
    // rewritten if anything changed so the next startup is warm again.
    std::vector<Thread> out;
    std::error_code ec;
    if (!fs::exists(threads_dir(), ec)) return out;

    // Held across the whole walk: the read at the top and the refresh at
    // the bottom are one read-modify-write, and a save landing in the
    // middle must not have its entry clobbered by our `fresh` snapshot.
    std::lock_guard<std::mutex> lk(thread_index_mu());
    auto index = read_thread_index_locked();
    std::unordered_map<std::string, IndexEntry> fresh;
    fresh.reserve(index.size() + 8);
    bool index_dirty = false;

    for (const auto& e : fs::directory_iterator(threads_dir(), ec)) {
        if (e.path().extension() != ".json") continue;
        // acp_sessions.json is the ACP server's sidecar session index
        // (sessionId → {cwd, title, updatedAt}), not a thread file; and
        // index.json is our own cache. Neither is a thread — skip both.
        const auto fname = e.path().filename();
        if (fname == "acp_sessions.json" || fname == "index.json") continue;

        const std::string id = e.path().stem().string();
        const long long   mt = file_mtime_ns(e.path());
        const long long   sz = file_size_bytes(e.path());

        // Fast path: index entry whose mtime AND size both still match —
        // no open/parse. size == -1 (a v1 entry, or a failed stat) never
        // matches a real size, so those fall through and get upgraded.
        if (auto it = index.find(id);
            it != index.end() && it->second.mtime == mt && mt != 0
            && it->second.size == sz && sz >= 0) {
            out.push_back(thread_from_index(id, it->second));
            fresh.emplace(id, it->second);
            continue;
        }

        // Slow path: new or changed file — SAX-parse metadata + reindex.
        auto loaded = load_thread_meta_file(e.path());
        if (loaded) {
            IndexEntry ie;
            ie.title      = loaded->title;
            ie.created_at = std::chrono::duration_cast<std::chrono::seconds>(
                                loaded->created_at.time_since_epoch()).count();
            ie.updated_at = std::chrono::duration_cast<std::chrono::seconds>(
                                loaded->updated_at.time_since_epoch()).count();
            ie.mtime      = mt;
            ie.size       = sz;
            fresh.emplace(id, std::move(ie));
            index_dirty = true;
            out.push_back(std::move(*loaded));
        } else {
            // Log and skip — corrupt or schema-incompatible files don't
            // kill the rest of the directory walk. The typed kind is
            // visible to anyone watching stderr; programmatic callers
            // who want a strict load can use load_thread_file directly.
            std::fprintf(stderr,
                "agentty: skipping %s — %s\n",
                e.path().string().c_str(),
                loaded.error().render().c_str());
        }
    }

    // Prune entries whose files vanished (deleted threads) and persist
    // the refreshed index for the next warm startup.
    if (fresh.size() != index.size()) index_dirty = true;
    if (index_dirty) write_thread_index_locked(fresh);

    std::sort(out.begin(), out.end(), [](const Thread& a, const Thread& b){
        return a.updated_at > b.updated_at;
    });
    return out;
}

// Synchronous worker — builds JSON, fsync, atomic-rename. The public
// `save_thread` entry point enqueues onto a background writer instead
// of running this on the caller's thread; serialising a 200-message
// transcript can take 50-200 ms and blocking the reducer there froze
// the UI at every turn-finalize. Called only by the worker below; the
// worker holds at most one pending Thread per id (newer save wins),
// so two finalize-back-to-back calls do at most one disk write.
static void save_thread_sync(const Thread& t) {
    if (t.id.empty() || t.messages.empty()) return;
    json j;
    j["id"] = t.id;
    j["title"] = t.title;
    if (!t.forked_from.empty()) j["forked_from"] = t.forked_from;
    if (t.rag_mode_override)
        j["rag_mode_override"] = static_cast<int>(*t.rag_mode_override);
    j["created_at"] = std::chrono::duration_cast<std::chrono::seconds>(
        t.created_at.time_since_epoch()).count();
    j["updated_at"] = std::chrono::duration_cast<std::chrono::seconds>(
        t.updated_at.time_since_epoch()).count();
    json msgs = json::array();
    for (const auto& m : t.messages) {
        // Smart Mode routing cards are view-only telemetry (no wire content) —
        // never persist them, exactly like they're never sent to the model.
        if (m.smart_routing) continue;
        msgs.push_back(message_to_json(m));
    }
    j["messages"] = std::move(msgs);
    // Wire-only compaction records. Persisting these lets a reloaded
    // thread keep sending the SAME wire payload it was sending before
    // shutdown — if the user compacted at turn 40 then closed the app,
    // the next request after reload still summarises the [0, 40) prefix
    // instead of resending all 40 raw turns and blowing context. Empty
    // for threads that have never been compacted (the common case);
    // older on-disk threads predate the field and parse_thread defaults
    // it to empty, so upgrade is transparent.
    if (!t.compactions.empty()) {
        json comps = json::array();
        for (const auto& c : t.compactions) {
            json cj;
            cj["up_to_index"] = c.up_to_index;
            cj["summary"]     = tools::util::to_valid_utf8(c.summary);
            cj["created_at"]  = std::chrono::duration_cast<std::chrono::seconds>(
                                    c.created_at.time_since_epoch()).count();
            comps.push_back(std::move(cj));
        }
        j["compactions"] = std::move(comps);
    }
    // dump() throws type_error.316 on non-UTF-8 bytes. Scrubbing in
    // message_to_json should have caught everything, but swallow the
    // exception as a belt-and-suspenders guard against future regressions —
    // a silently-skipped save beats a process-terminating uncaught throw.
    try {
        const bool ok = write_json_atomic(
            threads_dir() / (t.id.value + ".json"), j.dump(2));
        // A failed save loses the user's conversation with nothing on screen
        // to say so — the worst kind of silent failure, and previously
        // invisible because the result was discarded.
        if (!ok)
            AGT_LOG(Persist, Error, "thread.save",
                    "result=write_failed id={} messages={}",
                    t.id.value, t.messages.size());
        else
            AGT_LOG(Persist, Debug, "thread.save", "result=ok id={} messages={}",
                    t.id.value, t.messages.size());
        // Keep the metadata index in lock-step with the file we just
        // wrote so the next startup's fast path picks it up.
        reindex_thread(t);
    } catch (const nlohmann::json::exception& e) {
        // caller can't react; best-effort persistence is acceptable here.
        AGT_LOG(Persist, Error, "thread.save", "result=json_error id={} err={}",
                t.id.value, e.what());
    }
}

// ── Async writer ─────────────────────────────────────────────────────
//
// Single background thread + coalescing pending-map keyed by ThreadId.
// `save_thread(t)` upserts t into the map and signals the worker; the
// worker drains the map by repeatedly extracting one entry at a time
// and running `save_thread_sync` on it. Two saves of the same thread
// arriving while the worker is busy collapse to one disk write of the
// latest snapshot — the map already deduplicates by key. Two saves of
// DIFFERENT threads each get one write.
//
// Lifetime: the worker is started lazily on the first save and runs
// for the life of the process. There's no `flush + join` on shutdown
// because the reducer's Quit handler issues a final save_thread()
// before maya returns; we wait for the queue to drain inside
// flush_and_stop() which `main` calls right after maya::run returns.
struct AsyncWriter {
    std::mutex                              mu;
    std::condition_variable                 cv;
    std::unordered_map<std::string, Thread> pending;
    bool                                    stopping = false;
    std::thread                             worker;

    void enqueue(Thread t) {
        {
            std::lock_guard<std::mutex> lk(mu);
            // Newer snapshot supersedes any older one still queued
            // for the same thread id. Move-assign so we don't copy
            // the messages vector twice.
            pending.insert_or_assign(t.id.value, std::move(t));
            if (!worker.joinable()) start_locked();
        }
        cv.notify_one();
    }

    void flush_and_stop() {
        std::thread to_join;
        {
            std::lock_guard<std::mutex> lk(mu);
            stopping = true;
            // Hand the worker handle out under the lock so a concurrent
            // enqueue() can't observe a half-stopped state, and join
            // outside the lock. run() drains every queued save before it
            // sees `stopping` and exits, so nothing enqueued before this
            // call is dropped.
            if (worker.joinable()) to_join = std::move(worker);
        }
        cv.notify_one();
        if (to_join.joinable()) to_join.join();
    }

    ~AsyncWriter() { flush_and_stop(); }

private:
    void start_locked() {
        worker = std::thread([this] { run(); });
    }

    void run() {
        for (;;) {
            Thread next;
            {
                std::unique_lock<std::mutex> lk(mu);
                cv.wait(lk, [this] { return !pending.empty() || stopping; });
                // Drain-then-stop: even when `stopping` is set we keep
                // popping until `pending` is empty, so the final
                // snapshot the Quit reducer enqueued always lands.
                if (pending.empty()) {
                    if (stopping) return;
                    continue;
                }
                auto it = pending.begin();
                next = std::move(it->second);
                pending.erase(it);
            }
            // Run outside the lock so concurrent enqueue() calls don't
            // block on the (potentially slow) fsync.
            try { save_thread_sync(next); }
            catch (const std::exception& e) {
                // best-effort, same policy as the sync path
                util::dbglog("persistence.async_save", e.what());
            }
            catch (...) { util::dbglog("persistence.async_save", "non-std exception"); }
        }
    }
};

static AsyncWriter& async_writer() {
    static AsyncWriter w;
    return w;
}

void save_thread(const Thread& t) {
    if (t.id.empty() || t.messages.empty()) return;
    async_writer().enqueue(t);
}

void flush_pending_saves() {
    async_writer().flush_and_stop();
}

void delete_thread(const ThreadId& id) {
    std::error_code ec;
    fs::remove(threads_dir() / (id.value + ".json"), ec);
    // Drop the metadata index entry too so the picker list doesn't show
    // a ghost row until the next full walk prunes it. Under the index
    // lock: a concurrent reindex_thread() on the AsyncWriter worker
    // would otherwise resurrect this id from its stale in-memory copy.
    std::lock_guard<std::mutex> lk(thread_index_mu());
    auto idx = read_thread_index_locked();
    if (idx.erase(id.value) > 0) write_thread_index_locked(idx);
}

store::Settings load_settings() {
    store::Settings s;
    std::ifstream ifs(data_dir() / "settings.json");
    if (!ifs) return s;
    try {
        json j; ifs >> j;
        s.model_id = ModelId{j.value("model_id", "")};
        s.profile = static_cast<Profile>(j.value("profile", 0));
        auto favs = j.value("favorite_models", std::vector<std::string>{});
        for (auto& f : favs) s.favorite_models.push_back(ModelId{std::move(f)});
        s.provider = j.value("provider", "");
        if (j.contains("provider_keys") && j["provider_keys"].is_object()) {
            for (auto& [k, v] : j["provider_keys"].items())
                if (v.is_string()) s.provider_keys[k] = v.get<std::string>();
        }
        if (j.contains("provider_models") && j["provider_models"].is_object()) {
            for (auto& [k, v] : j["provider_models"].items())
                if (v.is_string()) s.provider_models[k] = v.get<std::string>();
        }
        if (j.contains("recent_models") && j["recent_models"].is_array()) {
            for (auto& v : j["recent_models"])
                if (v.is_string()) s.recent_models.push_back(v.get<std::string>());
        }
        s.effort = j.value("effort", "");
        if (j.contains("reasoning_effort_overrides")
            && j["reasoning_effort_overrides"].is_object()) {
            for (auto& [k, v] : j["reasoning_effort_overrides"].items())
                if (v.is_boolean()) s.reasoning_effort_overrides[k] = v.get<bool>();
        }
        if (j.contains("learned_effort_sets")
            && j["learned_effort_sets"].is_object()) {
            for (auto& [k, v] : j["learned_effort_sets"].items())
                if (v.is_number_unsigned())
                    s.learned_effort_sets[k] =
                        static_cast<std::uint8_t>(v.get<unsigned>() & 0xFFu);
        }
        auto grants = j.value("always_allow_tools", std::vector<std::string>{});
        s.always_allow_tools = std::move(grants);
        s.context_1m_blocked = j.value("context_1m_blocked", false);
        // ACCOUNT-SCOPED entitlements (see domain/entitlement.hpp). Keys are
        // opaque strings built by entitlement::key_for; stored as an object
        // so a hand-edited settings.json stays readable.
        if (j.contains("entitlements") && j["entitlements"].is_object()) {
            for (auto& [k, v] : j["entitlements"].items())
                if (v.is_boolean() && v.get<bool>())
                    s.entitlements[k] = true;
        }
        s.show_changes_strip = j.value("show_changes_strip", false);
        s.show_reasoning     = j.value("show_reasoning", false);
        if (j.contains("rag") && j["rag"].is_object()) {
            const auto& r = j["rag"];
            auto& c = s.rag;
            c.configured        = r.value("configured", true); // present ⇒ user-set
            c.mode              = static_cast<store::RagMode>(
                                      r.value("mode", static_cast<int>(c.mode)));
            c.skills            = r.value("skills", c.skills);
            c.memory            = r.value("memory", c.memory);
            c.mcp_resources     = r.value("mcp_resources", c.mcp_resources);
            c.contextual        = r.value("contextual", c.contextual);
            c.dedup             = r.value("dedup", c.dedup);
            c.mmr               = r.value("mmr", c.mmr);
            c.stitch            = r.value("stitch", c.stitch);
            c.autocut           = r.value("autocut", c.autocut);
            c.prf               = r.value("prf", c.prf);
            c.corrective        = r.value("corrective", c.corrective);
            c.graph             = r.value("graph", c.graph);
            c.expand            = r.value("expand", c.expand);
            c.hyde              = r.value("hyde", c.hyde);
            c.fusion            = r.value("fusion", c.fusion);
            c.adaptive_fusion   = r.value("adaptive_fusion", c.adaptive_fusion);
            c.proactive         = r.value("proactive", c.proactive);
            c.proactive_min_conf= r.value("proactive_min_conf", c.proactive_min_conf);
            c.proactive_bytes   = r.value("proactive_bytes", c.proactive_bytes);
            c.persist           = r.value("persist", c.persist);
            c.learn             = r.value("learn", c.learn);
            c.trace             = r.value("trace", c.trace);
            // Registry-owned rows, read by walking the table — a knob added
            // there is loaded here with no edit. Keys are the row ids
            // ("rag.mmr_lambda"), stored flat so a rename is visible in the
            // file rather than silently resetting to the default.
            for (const auto& d : settings::registry::kSettings) {
                const std::string key{d.id};
                if (!r.contains(key)) continue;
                const auto& v = r.at(key);
                std::string as_text;
                if      (v.is_boolean())        as_text = v.get<bool>() ? "true" : "false";
                else if (v.is_number_integer()) as_text = std::to_string(v.get<long long>());
                else if (v.is_number())         as_text = std::to_string(v.get<double>());
                else if (v.is_string())         as_text = v.get<std::string>();
                else continue;
                (void)settings::registry::set(c, d, as_text);
            }
            // Embeddings. Absent keys leave the env-derived default intact.
            c.embed_backend        = r.value("embed_backend", c.embed_backend);
            c.embed_model          = r.value("embed_model", c.embed_model);
            c.embed_host           = r.value("embed_host", c.embed_host);
            c.embed_port           = static_cast<std::uint16_t>(
                                        r.value("embed_port", static_cast<int>(c.embed_port)));
            c.embed_tls            = r.value("embed_tls", c.embed_tls);
            c.embed_path           = r.value("embed_path", c.embed_path);
            c.embed_model_path     = r.value("embed_model_path", c.embed_model_path);
            c.embed_tokenizer_path = r.value("embed_tokenizer_path", c.embed_tokenizer_path);
            c.embed_dim            = static_cast<std::uint32_t>(
                                        r.value("embed_dim", static_cast<int>(c.embed_dim)));
        }
        if (j.contains("smart") && j["smart"].is_object()) {
            const auto& sm = j["smart"];
            s.smart.enabled = sm.value("enabled", false);
            // The seven sub-layer flags (route_internal, orchestrate,
            // route_subagents, learn_routing, outcome_feedback, speculative,
            // recall_plans) are no longer read. Keys left in an existing
            // settings.json are ignored — three folded into the master switch,
            // four deleted with the self-supervised layers. No migration
            // needed: an unknown key was always tolerated.

            // Slots read straight into the domain type. The FLAT wire keys are
            // unchanged, so a settings.json written by an older build loads
            // with its pins intact — the shape changed in memory, not on disk.
            //
            // A pin is `set` when it names a model, which is the same rule the
            // old three-field mapping applied; keeping it here means there is
            // one place that decides what a pinned slot IS.
            const auto slot = [&](const char* model_key, const char* eff_key,
                                  const char* prov_key) {
                smart::SlotOverride o;
                o.model = sm.value(model_key, "");
                if (!o.model.empty()) {
                    o.effort = effort_from_wire(sm.value(eff_key, ""));
                    o.set    = true;
                    // Missing (settings written before pins were
                    // provider-scoped) ⇒ "" = unknown provenance, honoured
                    // under every provider. Same behaviour as before the
                    // field existed.
                    o.provider = sm.value(prov_key, "");
                }
                return o;
            };
            s.smart.strategic =
                slot("strategic_model", "strategic_effort", "strategic_provider");
            s.smart.implementation =
                slot("impl_model", "impl_effort", "impl_provider");
            s.smart.utility =
                slot("utility_model", "utility_effort", "utility_provider");

            // Numeric routing policy, read by WALKING the registry — which
            // is what clamps each value to its row's range on the way in, so
            // a hand-edited settings.json cannot put the config into a state
            // the UI could never produce. An absent key keeps the default.
            for (const auto& d : settings::registry::kSettings) {
                if (d.owner() != settings::registry::Owner::Smart) continue;
                const auto dot = d.id.find('.');
                const std::string key{d.id.substr(dot + 1)};
                if (!sm.contains(key)) continue;
                const auto& v = sm[key];
                std::string as_text;
                if      (v.is_boolean())        as_text = v.get<bool>() ? "true" : "false";
                else if (v.is_number_integer()) as_text = std::to_string(v.get<long long>());
                else if (v.is_number())         as_text = std::to_string(v.get<double>());
                else if (v.is_string())         as_text = v.get<std::string>();
                else continue;
                (void)settings::registry::set(s.smart, d, as_text);
            }
        }
        // Environment overrides, applied by WALKING the registry. This is what
        // makes a row's env alias real for every Settings-owned knob at once,
        // and it clamps to each row's range on the way in.
        //
        // OUTSIDE the `smart` block on purpose: an export must take effect on
        // a config that has never had that section written, which is every
        // config until the user changes something. Applied after the JSON so
        // an export beats a stored value — the layering the locked row in the
        // settings UI advertises.
        settings::registry::apply_env(s.smart);
    } catch (const std::exception& e) {
        util::dbglog("persistence.load_settings", e.what());
    } catch (...) {
        util::dbglog("persistence.load_settings", "non-std exception");
    }
    return s;
}

void save_settings(const store::Settings& s) {
    json j;
    j["model_id"] = s.model_id;
    j["profile"] = static_cast<int>(s.profile);
    json favs = json::array();
    for (const auto& mid : s.favorite_models) favs.push_back(mid);
    j["favorite_models"] = std::move(favs);
    if (!s.provider.empty()) j["provider"] = s.provider;
    if (!s.provider_keys.empty()) {
        json keys = json::object();
        for (const auto& [k, v] : s.provider_keys) keys[k] = v;
        j["provider_keys"] = std::move(keys);
    }
    if (!s.provider_models.empty()) {
        json pm = json::object();
        for (const auto& [k, v] : s.provider_models) pm[k] = v;
        j["provider_models"] = std::move(pm);
    }
    if (!s.recent_models.empty()) {
        json rm = json::array();
        for (const auto& e : s.recent_models) rm.push_back(e);
        j["recent_models"] = std::move(rm);
    }
    if (!s.effort.empty()) j["effort"] = s.effort;
    if (!s.reasoning_effort_overrides.empty()) {
        json ro = json::object();
        for (const auto& [k, v] : s.reasoning_effort_overrides) ro[k] = v;
        j["reasoning_effort_overrides"] = std::move(ro);
    }
    if (!s.learned_effort_sets.empty()) {
        json le = json::object();
        for (const auto& [k, v] : s.learned_effort_sets)
            le[k] = static_cast<unsigned>(v);
        j["learned_effort_sets"] = std::move(le);
    }
    if (!s.always_allow_tools.empty())
        j["always_allow_tools"] = s.always_allow_tools;
    // Legacy account-blind flag is READ (for migration, above) but never
    // written back — the keyed `entitlements` store supersedes it. Dropping
    // it on the next save is the migration's final step: an older agentty
    // reading this file simply re-learns the fact on its first 400, which is
    // exactly the behaviour it had before.
    if (!s.entitlements.empty()) {
        json ent = json::object();
        for (const auto& [k, v] : s.entitlements)
            if (v) ent[k] = true;
        if (!ent.empty()) j["entitlements"] = std::move(ent);
    }
    // Only persisted when turned ON (default is off), keeping fresh configs clean.
    if (s.show_changes_strip) j["show_changes_strip"] = true;
    if (s.show_reasoning)     j["show_reasoning"] = true;
    if (s.rag.configured) {
        const auto& c = s.rag;
        // The picker only sets `mode`; the rest are internal defaults, still
        // round-tripped so an env/power-user override survives a save.
        j["rag"] = {
            {"configured",         true},
            {"mode",               static_cast<int>(c.mode)},
            {"skills",             c.skills},
            {"memory",             c.memory},
            {"mcp_resources",      c.mcp_resources},
            {"contextual",         c.contextual},
            {"dedup",              c.dedup},
            {"mmr",                c.mmr},
            {"stitch",             c.stitch},
            {"autocut",            c.autocut},
            {"prf",                c.prf},
            {"corrective",         c.corrective},
            {"graph",              c.graph},
            {"expand",             c.expand},
            {"hyde",               c.hyde},
            {"fusion",             c.fusion},
            {"adaptive_fusion",    c.adaptive_fusion},
            {"proactive",          c.proactive},
            {"proactive_min_conf", c.proactive_min_conf},
            {"proactive_bytes",    c.proactive_bytes},
            {"persist",            c.persist},
            {"learn",              c.learn},
            {"trace",              c.trace},
        };
        // Registry-owned rows: written by walking the table, and ONLY when
        // they differ from the shipped default. A config that never touched a
        // knob stays clean, and a future change to a default reaches users
        // who never overrode it.
        {
            auto& r = j["rag"];
            for (const auto& d : settings::registry::kSettings) {
                if (settings::registry::is_default(c, d)) continue;
                const std::string key{d.id};
                const std::string val = settings::registry::get(c, d);
                switch (d.type) {
                    case settings::registry::Type::Bool: r[key] = (val == "true"); break;
                    case settings::registry::Type::Int:
                        try { r[key] = std::stoll(val); } catch (...) {}
                        break;
                    case settings::registry::Type::Real:
                        try { r[key] = std::stod(val); } catch (...) {}
                        break;
                    case settings::registry::Type::Enum: r[key] = val; break;
                }
            }
        }
        // Embeddings: written only once the user has actually chosen a
        // backend, so a config that never touched the pane stays clean and
        // keeps following the env/default path. The API key is NEVER written
        // here — it lives in the OS keystore.
        if (!c.embed_backend.empty()) {
            auto& r = j["rag"];
            r["embed_backend"] = c.embed_backend;
            if (!c.embed_model.empty())          r["embed_model"]          = c.embed_model;
            if (!c.embed_host.empty())           r["embed_host"]           = c.embed_host;
            if (c.embed_port != 0)               r["embed_port"]           = c.embed_port;
            if (c.embed_tls)                     r["embed_tls"]            = true;
            if (!c.embed_path.empty())           r["embed_path"]           = c.embed_path;
            if (!c.embed_model_path.empty())     r["embed_model_path"]     = c.embed_model_path;
            if (!c.embed_tokenizer_path.empty()) r["embed_tokenizer_path"] = c.embed_tokenizer_path;
            if (c.embed_dim != 0)                r["embed_dim"]            = c.embed_dim;
        }
    }
    // Smart Mode: persist only when meaningfully configured (enabled, any
    // slot pinned, or a tuning row moved off its default) so a fresh config
    // stays clean.
    const bool smart_tuned = [&] {
        for (const auto& d : settings::registry::kSettings)
            if (d.owner() == settings::registry::Owner::Smart
                && !settings::registry::is_default(s.smart, d)) return true;
        return false;
    }();
    if (s.smart.enabled || s.smart.strategic.set || s.smart.implementation.set
        || s.smart.utility.set || smart_tuned) {
        nlohmann::json sm;
        sm["enabled"] = s.smart.enabled;

        // Slots written FLAT, the wire format older builds already read — the
        // in-memory shape changed, the file did not. Provider provenance rides
        // with each pin: a model id is only meaningful to the endpoint that
        // serves it, so resolve_role replays a pin ONLY under the provider it
        // was made on. Absent (old settings) reads back as "" = honour
        // everywhere.
        const auto put_slot = [&](const smart::SlotOverride& o,
                                  const char* model_key, const char* eff_key,
                                  const char* prov_key) {
            if (o.model.empty()) return;
            sm[model_key] = o.model;
            if (const auto e = effort_wire(o.effort); !e.empty())
                sm[eff_key] = std::string{e};
            if (!o.provider.empty()) sm[prov_key] = o.provider;
        };
        put_slot(s.smart.strategic, "strategic_model", "strategic_effort",
                 "strategic_provider");
        put_slot(s.smart.implementation, "impl_model", "impl_effort",
                 "impl_provider");
        put_slot(s.smart.utility, "utility_model", "utility_effort",
                 "utility_provider");

        // Numeric routing policy, written by WALKING the registry — the same
        // discipline the rag block uses. Only non-default rows are emitted,
        // so settings.json stays readable and a shipped-default change still
        // reaches users who never touched the knob.
        for (const auto& d : settings::registry::kSettings) {
            if (d.owner() != settings::registry::Owner::Smart) continue;
            if (settings::registry::is_default(s.smart, d)) continue;
            const auto dot = d.id.find('.');
            const std::string key{d.id.substr(dot + 1)};
            const std::string val = settings::registry::get(s.smart, d);
            switch (d.type) {
                case settings::registry::Type::Bool: sm[key] = (val == "true"); break;
                case settings::registry::Type::Int:
                    sm[key] = std::atoi(val.c_str()); break;
                case settings::registry::Type::Real:
                    sm[key] = std::atof(val.c_str()); break;
                case settings::registry::Type::Enum: sm[key] = val; break;
            }
        }
        j["smart"] = std::move(sm);
    }
    // A failed settings write silently discards the user's provider keys,
    // model choice and preferences — they simply "don't stick" across
    // restarts, with nothing to explain why. The result was discarded here.
    if (!write_json_atomic(data_dir() / "settings.json", j.dump(2)))
        AGT_LOG(Persist, Error, "settings.save", "result=write_failed path={}",
                (data_dir() / "settings.json").string());
}

ThreadId new_id() {
    static std::mt19937_64 rng{std::random_device{}()};
    static std::mutex      mu;
    std::lock_guard<std::mutex> lk(mu);
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << dist(rng);
    return ThreadId{oss.str()};
}

std::string title_from_first_message(std::string_view text) {
    std::string t{text};
    for (auto& c : t) if (c == '\n' || c == '\r') c = ' ';
    if (t.size() > 60) {
        // UTF-8-safe cut: a naive resize(57) can split a multi-byte sequence,
        // and the partial bytes propagate into json::dump() which throws
        // type_error.316 on any invalid UTF-8. Scrub afterward as belt-and-
        // suspenders in case `text` itself arrived malformed.
        t.resize(tools::util::safe_utf8_cut(t, 57));
        t = tools::util::to_valid_utf8(std::move(t));
        t += "...";
    }
    if (t.empty()) t = "New thread";
    return t;
}

} // namespace agentty::persistence

namespace agentty {

// Per-Message stable identity. Generated at Message default-construction
// (see Message::id in conversation.hpp). The cache key (thread_id,
// message_id) is stable across vector index shifts (compaction,
// deletion, reordering) so a render-cache lookup never returns a
// stale Element for a now-different message at the same position.
//
// Implementation mirrors persistence::new_id() — 64-bit random hex,
// thread-safe via static mt19937 + std::random_device. 16 hex digits
// is more than enough for within-process uniqueness; the chance of two
// IDs colliding within a session is ~2⁻³² even at a million messages,
// well below any realistic load.
MessageId new_message_id() {
    static std::mt19937_64 rng{std::random_device{}()};
    static std::mutex      mu;
    std::lock_guard<std::mutex> lk(mu);
    std::uniform_int_distribution<uint64_t> dist;
    // Zero-pad to 16 hex digits so every id is fixed width. Variable
    // width was technically unambiguous given the ":" separator in cache
    // keys but brittle: a 0x1 roll produced "1", which is a substring of
    // most other ids. Fixed width also makes persisted ids look uniform.
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << dist(rng);
    return MessageId{oss.str()};
}

} // namespace agentty
