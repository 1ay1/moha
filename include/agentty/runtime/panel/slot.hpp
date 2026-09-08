#pragma once
// agentty::ui::panel — the EXCLUSIVE panel slot, as a sum type.
//
// Model::UI used to carry 16 independent panel fields (pick::OneAxis
// here, an ad-hoc Closed|Open variant there). "Only one overlay open at
// a time" was a CONVENTION enforced by ~17 scattered
// `m.ui.X = pick::Closed{}` writes inside every Open* reducer — forget
// one and two overlays were open at once, with the router (overlay.hpp)
// papering over the ambiguity at dispatch time.
//
// Now every exclusive panel lives in ONE slot:
//
//     m.ui.panel = pn::Models{{.index = 3}};    // opens (closes rival)
//     m.ui.panel.is<pn::Models>()               // open?
//     m.ui.panel.get<pn::Models>()              // payload* or nullptr
//     m.ui.panel.close<pn::Models>()            // close IF topmost
//
// Opening is assignment — it structurally closes whatever was open,
// because a variant holds one alternative. "Two exclusive panels open"
// is not a bug we guard against anymore; it is UNREPRESENTABLE. All the
// rival-closing writes are deleted, not relocated.
//
// Deliberate NON-members (the exceptions that shape the design):
//   • login      — a 9-state auth machine with Origin-frame navigation;
//                  Closed is a first-class member of ITS variant.
//   • permission — m.d.pending_permission is DOMAIN state: the stream
//                  reducer raises it mid-turn, potentially while an
//                  overlay is open. Both must coexist; the router just
//                  hands permission the keyboard.
//   • todo       — an AMBIENT pane, not a modal: it stays open UNDER a
//                  picker and unclaimed keys fall through to global. Its
//                  open flag stays on TodoState.
// panel::top() (overlay.hpp) composes these four sources — login,
// permission, this slot, todo — into the one priority answer, which is
// now four checks instead of twenty.
//
// Each alternative INHERITS its overlay's existing Open payload
// (pick::OpenAt, palette::Open, …): same fields, same invariants, same
// manipulation code — plus a distinct TYPE so the slot can tell overlays
// apart and call sites name them directly.
//
// Pointer lifetime: get<K>() points INTO the slot. Assigning the slot
// destroys the previous alternative — capture what you need BEFORE
// opening a different overlay. (This was equally true with separate
// fields: assigning pick::Closed{} destroyed the payload too.)

#include <memory>
#include <utility>
#include <variant>

#include "agentty/runtime/panel/common.hpp"
#include "agentty/domain/smart_mode.hpp"   // smart::OverlayRow
#include "agentty/runtime/panel/palette.hpp"
#include "agentty/runtime/panel/mention.hpp"
#include "agentty/runtime/panel/symbol.hpp"
#include "agentty/runtime/panel/code_blocks.hpp"
#include "agentty/runtime/panel/tool_output.hpp"
#include "agentty/runtime/panel/checkpoints.hpp"
#include "agentty/runtime/panel/rag.hpp"
#include "agentty/runtime/panel/settings/list.hpp"
#include "agentty/runtime/panel/fork.hpp"
#include "agentty/runtime/panel/form.hpp"

namespace agentty::ui::panel {

struct None {};

// ── From: the panel this one was opened OVER, as a full snapshot ──────
//
// Esc must unwind ONE level, and "one level up" depends on how you got
// here — which a panel cannot know unless it is told. The old answer
// (settings_origin::Origin) named the parent KIND plus hand-picked fields
// (a palette row, a settings category+row) and back_to() RECONSTRUCTED the
// parent from them — every field the reconstruction forgot (the palette's
// half-typed query) was user state silently thrown away, and every panel
// that wanted the behaviour needed its own stamp at every opener.
//
// A From carries the parent's ENTIRE slot value instead. Restoring is
// copying it back — nothing to reconstruct, nothing to forget — and the
// stashed parent contains ITS from, so palette → settings list → pane
// unwinds level by level without any stack to keep in sync with the slot:
// the chain lives inside the values, bounded by how deep a user actually
// descended.
//
// shared_ptr<const>: Model is copied by value in the reducer loop, so the
// snapshot must be cheap to copy and safe to share — immutable, restored
// BY COPY, never mutated in place. Snapshot is defined after Variant (the
// type is self-referential through this one indirection).
//
// Restored state can be STALE — the model may have changed while the child
// was open (a stream ended; a setting was applied). ascend()'s caller owns
// revalidation: clamp cursors, rebuild forms. See app::detail::ascend().
struct Snapshot;
class From {
public:
    From() = default;
    [[nodiscard]] bool empty() const noexcept { return !s_; }
    [[nodiscard]] const Snapshot* get() const noexcept { return s_.get(); }
    static From of(Snapshot s);
private:
    std::shared_ptr<const Snapshot> s_;
};

// Base every alternative inherits: where Esc goes. Default-empty = "opened
// over the thread", so Esc closes.
struct WithFrom { From from; };

// ── The exclusive panels: one distinct type each, payload inherited ───
struct Models     : pick::OpenAt, WithFrom {
    // Smart-Mode slot-assign mode: this picker was opened BY a SmartMode
    // Pick row to pin a model into `assign_slot`, and Enter writes the pin
    // instead of switching the model. Carried ON the panel — the mode is a
    // property of THIS picker instance, not of the app — so abandoning the
    // picker (close, hop to providers) structurally abandons the mode; no
    // parked flag to remember to reset. The SmartMode pane it must restore
    // rides in `from` like every other parent (the snapshot carries the
    // form, advanced flag and nested chain — nothing else to park).
    std::optional<smart::ModelRole> assign_slot;
};
struct Providers  : pick::OpenAt, WithFrom {};
struct ThreadList      : pick::OpenAt, WithFrom {};
// Smart Mode carries a typed ROW, not an index. The other pickers list a
// variable number of runtime-derived entries, so an int cursor is the honest
// representation there; Smart Mode's rows are a fixed, named set, and
// spelling them as an int is what let the cursor drift onto rows that do not
// exist (see smart::OverlayRow for the full post-mortem).
// Smart Mode carries its FORM, not an int cursor. Row identity, navigation,
// the env-pin lock and the picker hand-off all come from the shared form
// layer, so this pane behaves identically to Retrieval and to every future
// config surface — and an out-of-range cursor is not representable.
struct SmartMode : WithFrom {
    agentty::form::Form form;
    // Show the advanced routing-policy rows (^A). View state, not config — it
    // dies with the overlay and is not persisted. Held HERE because every
    // rebuild (a slot assignment reopens the pane) must preserve it, or the
    // rows would vanish the moment the user pinned a model.
    bool advanced = false;
};
struct Palette  : agentty::palette::Open, WithFrom {};
struct Mention         : agentty::mention::Open, WithFrom {};
struct Symbol          : agentty::symbol::Open, WithFrom {};
struct CodeBlocks      : agentty::code_blocks::Open, WithFrom {};
struct CodeBlockResult : agentty::code_blocks::Result, WithFrom {};
struct ToolOutput      : agentty::tool_output::Open, WithFrom {};
struct Checkpoints     : agentty::checkpoints::Open, WithFrom {};
struct Rag     : agentty::rag_settings::Open, WithFrom {};
struct SettingsList    : agentty::settings::ListOpen, WithFrom {};
// Plugin detail/add editor — one form pane for BOTH flows. `server` empty
// == ADD mode (the `kind` choice row rebuilds the field set as the user
// picks stdio/http/passthrough…); non-empty == editing that server's entry
// (identity rows locked, config rows editable, Save force-overwrites).
// `project` routes the write to ./.agentty/mcp.json vs the user file.
struct PluginEdit : WithFrom {
    agentty::form::Form form;
    std::string server;      // "" = add mode
    bool        project = false;
    // The kind the CURRENT field set was built for. When the `kind` choice
    // changes, the reducer rebuilds the form for the new kind while
    // preserving name/url text the user already typed — same rebuild-
    // preserve pattern as SmartMode's advanced toggle.
    std::string built_kind;
};
struct Fork            : agentty::fork_panel::Open, WithFrom {};
struct DiffReview      : pick::OpenAtCell, WithFrom {};

using Variant = std::variant<
    None,
    Models, Providers, ThreadList, SmartMode,
    Palette, Mention, Symbol,
    CodeBlocks, CodeBlockResult, ToolOutput, Checkpoints,
    Rag, SettingsList, PluginEdit, Fork,
    DiffReview>;

// The one indirection that lets the type refer to itself: a stashed parent
// is a whole slot value, from included.
struct Snapshot { Variant v; };
inline From From::of(Snapshot s) {
    From f;
    f.s_ = std::make_shared<const Snapshot>(std::move(s));
    return f;
}

template <class K>
concept Alternative = requires(Variant v) { std::holds_alternative<K>(v); };

// The slot itself: a thin wrapper so call sites read as intent
// (`m.ui.panel.is<pn::Models>()`) rather than as variant plumbing.
class State {
public:
    State() = default;

    // Open K (closing whatever was open) — plain assignment.
    template <Alternative K>
    State& operator=(K k) {
        v_ = std::move(k);
        return *this;
    }

    template <Alternative K>
    [[nodiscard]] bool is() const noexcept {
        return std::holds_alternative<K>(v_);
    }
    template <Alternative K>
    [[nodiscard]] K* get() noexcept { return std::get_if<K>(&v_); }
    template <Alternative K>
    [[nodiscard]] const K* get() const noexcept { return std::get_if<K>(&v_); }

    [[nodiscard]] bool any_open() const noexcept {
        return !std::holds_alternative<None>(v_);
    }

    // Close K IF it is the open overlay; leave any other overlay alone.
    // Mirrors the old per-field close semantics: a stale CloseModels
    // arriving after the user hopped to the provider picker must not
    // close the provider picker.
    template <Alternative K>
    void close() noexcept {
        if (std::holds_alternative<K>(v_)) v_ = None{};
    }
    // Close unconditionally (whatever is open).
    void close_all() noexcept { v_ = None{}; }

    // Open K OVER the current overlay: K's `from` becomes a snapshot of
    // whatever is open now, so Esc can restore it verbatim — query, cursor,
    // nested from and all. Opening over None stashes nothing (from stays
    // empty) and Esc simply closes: "the thread" needs no snapshot.
    //
    // Use `descend` at OPEN sites and plain assignment at RESTORE sites —
    // a restore that descended would stash the child as its own parent's
    // parent and Esc would cycle instead of unwinding.
    template <Alternative K>
    void descend(K k) {
        if (!std::holds_alternative<None>(v_))
            k.from = From::of(Snapshot{std::move(v_)});
        v_ = std::move(k);
    }

    // Give the CURRENTLY-OPEN overlay a parent, if it has none yet. The
    // caller pattern: a dispatcher (palette select, settings-list action)
    // snapshots itself, closes, runs the command — and then adopts, so
    // WHATEVER the command opened inherits the dispatcher as its Esc
    // target. Generic: the dispatcher needs no per-command knowledge, and
    // a command that opened nothing (None) or re-opened something that
    // already has a parent is left alone.
    void adopt(From f) {
        std::visit(
            [&](auto& a) {
                if constexpr (requires { a.from; })
                    if (a.from.empty()) a.from = std::move(f);
            },
            v_);
    }

    // Restore the parent this overlay was opened over. False if there is
    // none (opened over the thread, or a pre-descend code path) — the
    // caller closes instead. The restored state may be STALE; the caller
    // owns revalidation (app::detail::ascend wraps both).
    [[nodiscard]] bool ascend() {
        const From* f = std::visit(
            [](const auto& a) -> const From* {
                if constexpr (requires { a.from; }) return &a.from;
                else return nullptr;
            },
            v_);
        if (!f || f->empty()) return false;
        // Copy out BEFORE overwriting: the snapshot lives inside v_.
        auto keep = f->get();
        Variant restored = keep->v;
        v_ = std::move(restored);
        return true;
    }

    [[nodiscard]] const Variant& raw() const noexcept { return v_; }

private:
    Variant v_;
};

// ── Kind: the routing name shared by dispatcher and view ────────────────
enum class Kind {
    None,
    Login,
    Permission,
    Palette,
    Mention,
    Symbol,
    CodeBlocks,
    CodeBlockResult,
    ToolOutput,
    Checkpoints,
    Rag,
    SettingsList,
    PluginEdit,
    Fork,
    Models,
    Providers,
    ThreadList,
    SmartMode,
    DiffReview,
    Todo,
};

// Slot alternative → Kind. An exhaustive visitor: adding an alternative
// without an arm here is a compile error, not a silent misroute.
[[nodiscard]] inline Kind kind_of(const State& s) noexcept {
    struct V {
        Kind operator()(const None&)            const { return Kind::None; }
        Kind operator()(const Models&)     const { return Kind::Models; }
        Kind operator()(const Providers&)  const { return Kind::Providers; }
        Kind operator()(const ThreadList&)      const { return Kind::ThreadList; }
        Kind operator()(const SmartMode&)       const { return Kind::SmartMode; }
        Kind operator()(const Palette&)  const { return Kind::Palette; }
        Kind operator()(const Mention&)         const { return Kind::Mention; }
        Kind operator()(const Symbol&)          const { return Kind::Symbol; }
        Kind operator()(const CodeBlocks&)      const { return Kind::CodeBlocks; }
        Kind operator()(const CodeBlockResult&) const { return Kind::CodeBlockResult; }
        Kind operator()(const ToolOutput&)      const { return Kind::ToolOutput; }
        Kind operator()(const Checkpoints&)     const { return Kind::Checkpoints; }
        Kind operator()(const Rag&)     const { return Kind::Rag; }
        Kind operator()(const SettingsList&)    const { return Kind::SettingsList; }
        Kind operator()(const PluginEdit&)      const { return Kind::PluginEdit; }
        Kind operator()(const Fork&)            const { return Kind::Fork; }
        Kind operator()(const DiffReview&)      const { return Kind::DiffReview; }
    };
    return std::visit(V{}, s.raw());
}

} // namespace agentty::ui::panel
