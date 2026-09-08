#pragma once
// visual_parts for the panel domain — every type the structural frame-hash
// walk (visual.hpp) cannot auto-decompose, with a completeness proof each.
//
// A parts list is the type's VISIBILITY DECISION, one line per base/member:
// walk it, project it (lengths, counts), or visual::exempt it with a reason.
// static_assert(parts_cover_all<T>) fires the moment a member is added
// without deciding — at the type, not as a missed repaint later.
//
// Included by program.hpp only (the hash site); zero cost anywhere else.

#include "agentty/domain/catalog.hpp"
#include "agentty/mcp/plugin_model.hpp"
#include "agentty/runtime/login.hpp"
#include "agentty/runtime/panel/slot.hpp"
#include "agentty/runtime/visual.hpp"

#include <maya/core/scroll_state.hpp>

// ── form: Secret is the load-bearing exemption ─────────────────────
namespace agentty::form::field {

// LENGTH ONLY. The credential's bytes must never reach any hash — a hash is
// an information channel, and this projection is the type-level guarantee
// the walk preserves. (Same-length overwrite is invisible to the gate;
// unreachable from single edits, which also move the cursor below.)
inline auto visual_parts(const Secret& s) {
    return std::make_tuple(s.value.size(), s.cursor);
}
static_assert(visual::parts_cover_all<Secret>);

} // namespace agentty::form::field

namespace agentty::rag::embed {

// EmbedConfig carries a PLAINTEXT api_key — the second secret the walk
// must never see. Everything else is visible verbatim (the form rows
// render it), so this list is: every field walked, the key as a length.
inline auto visual_parts(const EmbedConfig& c) {
    return std::make_tuple(c.backend, visual::ref(c.model),
                           visual::ref(c.host), c.port, c.tls,
                           visual::ref(c.path),
                           visual::ref(c.model_path),
                           visual::ref(c.tokenizer_path),
                           c.api_key.size(),          // LENGTH, never bytes
                           visual::ref(c.dim));
}
static_assert(visual::parts_cover_all<EmbedConfig>);

} // namespace agentty::rag::embed

// ── domain: the fused-model catalog + panel-adjacent snapshots ───────
namespace agentty {

// Id<Tag> has constructors (not an aggregate); one visible member.
template <class Tag>
inline auto visual_parts(const Id<Tag>& id) {
    return std::make_tuple(visual::ref(id.value));
}

// ProviderCatalog: the DERIVED caches (search_keys, row_keys,
// display_labels, reason_flags — rebuilt from `models` and only when it
// changes) are exempt: walking 450 lowercased haystacks per frame buys
// nothing `models` doesn't already signal. loaded_at_ms is a freshness
// clock, not pixels.
inline auto visual_parts(const ProviderCatalog& c) {
    return std::make_tuple(visual::ref(c.provider_id), visual::ref(c.label),
                           c.state, visual::ref(c.models),
                           visual::ref(c.account_label),
                           visual::exempt,   // loaded_at_ms: TTL clock
                           visual::exempt,   // search_keys:  derived cache
                           visual::exempt,   // row_keys:     derived cache
                           visual::exempt,   // display_labels: derived cache
                           visual::exempt,   // reason_flags: derived cache
                           visual::exempt);  // reason_epoch: cache version stamp
}
static_assert(visual::parts_cover_all<ProviderCatalog>);

} // namespace agentty

namespace agentty::mcp {

// ServerState: everything renders (the settings list projects the whole
// row — connection state, error, origin badge, the tool subtree).
// Auto-decomposition would work if every member were public+aggregate;
// spelled out because Origin lives beside strings and the list doubles as
// the visibility record.
inline auto visual_parts(const ServerState& s) {
    return std::make_tuple(visual::ref(s.name), visual::ref(s.command),
                           visual::ref(s.url), s.connected, s.disabled,
                           visual::ref(s.error), s.origin,
                           visual::ref(s.config_dir), s.untrusted,
                           visual::ref(s.tools));
}
static_assert(visual::parts_cover_all<ServerState>);

} // namespace agentty::mcp

// ── maya types the panels read ───────────────────────────────
namespace maya {

// ScrollState: only the OFFSETS are model-visible (a reducer-driven .y
// decides which rows a body-scroll view windows in). Everything else is
// render plumbing — writeback maxima and painted-bar rects the RENDERER
// refills every frame (hashing them would read last frame's paint into
// this frame's gate: a feedback loop, not state), step sizes, drag
// bookkeeping, the paint-generation counter. NON-AGGREGATE (unregistering
// destructor), so parts_cover_all cannot bind a count — this list is
// trusted prose; keep it in sync if ScrollState ever grows another
// reducer-driven field. Defined agentty-side: maya has no reason to know
// our frame gate exists.
inline auto visual_parts(const ScrollState& s) {
    return std::make_tuple(s.x, s.y);
}
static_assert(agentty::visual::parts_cover_all<ScrollState>);

} // namespace maya

namespace agentty::form {

// Form: every member is visible.
inline auto visual_parts(const Form& f) {
    return std::make_tuple(visual::ref(f.title), visual::ref(f.subtitle),
                           visual::ref(f.fields), f.cursor,
                           visual::ref(f.focus), f.dirty,
                           visual::ref(f.note));
}
static_assert(visual::parts_cover_all<Form>);

} // namespace agentty::form

// ── panel slot alternatives: bases + members ⇒ explicit lists ────────────
namespace agentty::ui::panel {

// The parent snapshot is NOT rendered while its child is open — walking it
// would also make every keystroke in a stacked panel re-walk its whole
// ancestry. Exempt, with this line as the reason.
inline auto visual_parts(const WithFrom&) {
    return std::make_tuple(visual::exempt);
}
static_assert(visual::parts_cover_all<WithFrom>);

inline auto visual_parts(const Models& p) {
    return std::make_tuple(visual::ref(static_cast<const pick::OpenAt&>(p)),
                           visual::ref(static_cast<const WithFrom&>(p)),
                           visual::ref(p.assign_slot));
}
static_assert(visual::parts_cover_all<Models>);

inline auto visual_parts(const Providers& p) {
    return std::make_tuple(visual::ref(static_cast<const pick::OpenAt&>(p)),
                           visual::ref(static_cast<const WithFrom&>(p)));
}
static_assert(visual::parts_cover_all<Providers>);

inline auto visual_parts(const ThreadList& p) {
    return std::make_tuple(visual::ref(static_cast<const pick::OpenAt&>(p)),
                           visual::ref(static_cast<const WithFrom&>(p)));
}
static_assert(visual::parts_cover_all<ThreadList>);

inline auto visual_parts(const SmartMode& p) {
    return std::make_tuple(visual::ref(static_cast<const WithFrom&>(p)),
                           visual::ref(p.form), p.advanced);
}
static_assert(visual::parts_cover_all<SmartMode>);

inline auto visual_parts(const Palette& p) {
    return std::make_tuple(
        visual::ref(static_cast<const agentty::palette::Open&>(p)),
        visual::ref(static_cast<const WithFrom&>(p)));
}
static_assert(visual::parts_cover_all<Palette>);

inline auto visual_parts(const Mention& p) {
    return std::make_tuple(
        visual::ref(static_cast<const agentty::mention::Open&>(p)),
        visual::ref(static_cast<const WithFrom&>(p)));
}
static_assert(visual::parts_cover_all<Mention>);

inline auto visual_parts(const Symbol& p) {
    return std::make_tuple(
        visual::ref(static_cast<const agentty::symbol::Open&>(p)),
        visual::ref(static_cast<const WithFrom&>(p)));
}
static_assert(visual::parts_cover_all<Symbol>);

inline auto visual_parts(const CodeBlocks& p) {
    return std::make_tuple(
        visual::ref(static_cast<const agentty::code_blocks::Open&>(p)),
        visual::ref(static_cast<const WithFrom&>(p)));
}
static_assert(visual::parts_cover_all<CodeBlocks>);

inline auto visual_parts(const CodeBlockResult& p) {
    return std::make_tuple(
        visual::ref(static_cast<const agentty::code_blocks::Result&>(p)),
        visual::ref(static_cast<const WithFrom&>(p)));
}
static_assert(visual::parts_cover_all<CodeBlockResult>);

inline auto visual_parts(const ToolOutput& p) {
    return std::make_tuple(
        visual::ref(static_cast<const agentty::tool_output::Open&>(p)),
        visual::ref(static_cast<const WithFrom&>(p)));
}
static_assert(visual::parts_cover_all<ToolOutput>);

inline auto visual_parts(const Checkpoints& p) {
    return std::make_tuple(
        visual::ref(static_cast<const agentty::checkpoints::Open&>(p)),
        visual::ref(static_cast<const WithFrom&>(p)));
}
static_assert(visual::parts_cover_all<Checkpoints>);

inline auto visual_parts(const Rag& p) {
    return std::make_tuple(
        visual::ref(static_cast<const agentty::rag_settings::Open&>(p)),
        visual::ref(static_cast<const WithFrom&>(p)));
}
static_assert(visual::parts_cover_all<Rag>);

inline auto visual_parts(const SettingsList& p) {
    return std::make_tuple(
        visual::ref(static_cast<const agentty::settings::ListOpen&>(p)),
        visual::ref(static_cast<const WithFrom&>(p)));
}
static_assert(visual::parts_cover_all<SettingsList>);

inline auto visual_parts(const Fork& p) {
    return std::make_tuple(
        visual::ref(static_cast<const agentty::fork_panel::Open&>(p)),
        visual::ref(static_cast<const WithFrom&>(p)));
}
static_assert(visual::parts_cover_all<Fork>);

inline auto visual_parts(const DiffReview& p) {
    return std::make_tuple(
        visual::ref(static_cast<const pick::OpenAtCell&>(p)),
        visual::ref(static_cast<const WithFrom&>(p)));
}
static_assert(visual::parts_cover_all<DiffReview>);

} // namespace agentty::ui::panel

namespace agentty::tool_output {

// Entry embeds a full ToolUse SNAPSHOT (time_points, arg streams — domain
// plumbing the walk must not decompose). The strings above it are the
// rendered row; the snapshot's body renders too, but it is IMMUTABLE once
// captured (that is the point of snapshotting) except for the live row,
// whose growing output is mirrored into `output` on every tool event — so
// output.size() + is_live cover its motion.
inline auto visual_parts(const Entry& e) {
    return std::make_tuple(visual::ref(e.name), visual::ref(e.title),
                           visual::ref(e.detail), visual::ref(e.trailing),
                           e.output.size(), e.failed, e.is_live,
                           visual::exempt);   // call: immutable snapshot
}
static_assert(visual::parts_cover_all<Entry>);

} // namespace agentty::tool_output

// ── login: its own variant outside the slot ───────────────────────
namespace agentty::ui::login {

// The API key IN FLIGHT is a secret: length + cursor only, same rule as
// field::Secret. (The OAuth callback code is a short-lived one-time token
// the user just copied from their own browser — still digested
// length-only for uniformity: the gate needs edit detection, not bytes.)
inline auto visual_parts(const OAuthCode& s) {
    return std::make_tuple(visual::exempt,          // verifier: not rendered
                           visual::exempt,          // state nonce: not rendered
                           visual::ref(s.authorize_url),
                           s.code_input.size(), s.cursor);
}
static_assert(visual::parts_cover_all<OAuthCode>);

inline auto visual_parts(const ApiKeyInput& s) {
    return std::make_tuple(visual::ref(s.origin),
                           visual::ref(s.provider),
                           visual::ref(s.provider_label),
                           s.key_input.size(),      // SECRET: length only
                           s.cursor);
}
static_assert(visual::parts_cover_all<ApiKeyInput>);

inline auto visual_parts(const ChatGptWaiting& s) {
    return std::make_tuple(s.attempt_id,
                           visual::exempt,          // cancel: worker plumbing
                           s.device_auth,
                           visual::ref(s.authorize_url),
                           visual::ref(s.user_code));
}
static_assert(visual::parts_cover_all<ChatGptWaiting>);

inline auto visual_parts(const DeviceWaiting& s) {
    return std::make_tuple(visual::ref(s.provider),
                           visual::ref(s.provider_label),
                           s.attempt_id,
                           visual::exempt,          // cancel: worker plumbing
                           visual::ref(s.authorize_url),
                           visual::ref(s.browser_url),
                           visual::ref(s.user_code));
}
static_assert(visual::parts_cover_all<DeviceWaiting>);

} // namespace agentty::ui::login
