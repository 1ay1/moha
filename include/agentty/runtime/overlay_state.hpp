#pragma once
// agentty::ui::overlay — the EXCLUSIVE overlay slot, as a sum type.
//
// Model::UI used to carry 16 independent overlay fields (pick::OneAxis
// here, an ad-hoc Closed|Open variant there). "Only one overlay open at
// a time" was a CONVENTION enforced by ~17 scattered
// `m.ui.X = pick::Closed{}` writes inside every Open* reducer — forget
// one and two overlays were open at once, with the router (overlay.hpp)
// papering over the ambiguity at dispatch time.
//
// Now every exclusive overlay lives in ONE slot:
//
//     m.ui.overlay = ov::FusedPicker{{.index = 3}};    // opens (closes rival)
//     m.ui.overlay.is<ov::FusedPicker>()               // open?
//     m.ui.overlay.get<ov::FusedPicker>()              // payload* or nullptr
//     m.ui.overlay.close<ov::FusedPicker>()            // close IF topmost
//
// Opening is assignment — it structurally closes whatever was open,
// because a variant holds one alternative. "Two exclusive overlays open"
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
// overlay::top() (overlay.hpp) composes these four sources — login,
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

#include <utility>
#include <variant>

#include "agentty/runtime/picker.hpp"
#include "agentty/domain/smart_mode.hpp"   // smart::OverlayRow
#include "agentty/runtime/command_palette.hpp"
#include "agentty/runtime/mention_palette.hpp"
#include "agentty/runtime/symbol_palette.hpp"
#include "agentty/runtime/code_block_picker.hpp"
#include "agentty/runtime/tool_output_viewer.hpp"
#include "agentty/runtime/checkpoint_picker.hpp"
#include "agentty/runtime/rag_settings.hpp"
#include "agentty/runtime/settings_list.hpp"
#include "agentty/runtime/fork_picker.hpp"
#include "agentty/runtime/form.hpp"

namespace agentty::ui::overlay {

struct None {};

// ── The exclusive overlays: one distinct type each, payload inherited ───
struct FusedPicker     : pick::OpenAt {};
struct ProviderPicker  : pick::OpenAt {};
struct ThreadList      : pick::OpenAt {};
// Smart Mode carries a typed ROW, not an index. The other pickers list a
// variable number of runtime-derived entries, so an int cursor is the honest
// representation there; Smart Mode's rows are a fixed, named set, and
// spelling them as an int is what let the cursor drift onto rows that do not
// exist (see smart::OverlayRow for the full post-mortem).
// Smart Mode carries its FORM, not an int cursor. Row identity, navigation,
// the env-pin lock and the picker hand-off all come from the shared form
// layer, so this pane behaves identically to Retrieval and to every future
// config surface — and an out-of-range cursor is not representable.
struct SmartMode {
    agentty::form::Form form;
};
struct CommandPalette  : agentty::palette::Open {};
struct Mention         : agentty::mention::Open {};
struct Symbol          : agentty::symbol_palette::Open {};
struct CodeBlocks      : agentty::code_block_picker::Open {};
struct CodeBlockResult : agentty::code_block_picker::Result {};
struct ToolViewer      : agentty::tool_viewer::Open {};
struct Checkpoints     : agentty::checkpoint_picker::Open {};
struct RagSettings     : agentty::rag_settings::Open {};
struct SettingsList    : agentty::settings::ListOpen {};
struct Fork            : agentty::fork_picker::Open {};
struct DiffReview      : pick::OpenAtCell {};

using Variant = std::variant<
    None,
    FusedPicker, ProviderPicker, ThreadList, SmartMode,
    CommandPalette, Mention, Symbol,
    CodeBlocks, CodeBlockResult, ToolViewer, Checkpoints,
    RagSettings, SettingsList, Fork,
    DiffReview>;

template <class K>
concept Alternative = requires(Variant v) { std::holds_alternative<K>(v); };

// The slot itself: a thin wrapper so call sites read as intent
// (`m.ui.overlay.is<ov::FusedPicker>()`) rather than as variant plumbing.
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
    // Mirrors the old per-field close semantics: a stale CloseFusedPicker
    // arriving after the user hopped to the provider picker must not
    // close the provider picker.
    template <Alternative K>
    void close() noexcept {
        if (std::holds_alternative<K>(v_)) v_ = None{};
    }
    // Close unconditionally (whatever is open).
    void close_all() noexcept { v_ = None{}; }

    [[nodiscard]] const Variant& raw() const noexcept { return v_; }

private:
    Variant v_;
};

// ── Kind: the routing name shared by dispatcher and view ────────────────
enum class Kind {
    None,
    Login,
    Permission,
    CommandPalette,
    Mention,
    Symbol,
    CodeBlocks,
    CodeBlockResult,
    ToolViewer,
    Checkpoints,
    RagSettings,
    SettingsList,
    Fork,
    FusedPicker,
    ProviderPicker,
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
        Kind operator()(const FusedPicker&)     const { return Kind::FusedPicker; }
        Kind operator()(const ProviderPicker&)  const { return Kind::ProviderPicker; }
        Kind operator()(const ThreadList&)      const { return Kind::ThreadList; }
        Kind operator()(const SmartMode&)       const { return Kind::SmartMode; }
        Kind operator()(const CommandPalette&)  const { return Kind::CommandPalette; }
        Kind operator()(const Mention&)         const { return Kind::Mention; }
        Kind operator()(const Symbol&)          const { return Kind::Symbol; }
        Kind operator()(const CodeBlocks&)      const { return Kind::CodeBlocks; }
        Kind operator()(const CodeBlockResult&) const { return Kind::CodeBlockResult; }
        Kind operator()(const ToolViewer&)      const { return Kind::ToolViewer; }
        Kind operator()(const Checkpoints&)     const { return Kind::Checkpoints; }
        Kind operator()(const RagSettings&)     const { return Kind::RagSettings; }
        Kind operator()(const SettingsList&)    const { return Kind::SettingsList; }
        Kind operator()(const Fork&)            const { return Kind::Fork; }
        Kind operator()(const DiffReview&)      const { return Kind::DiffReview; }
    };
    return std::visit(V{}, s.raw());
}

} // namespace agentty::ui::overlay
