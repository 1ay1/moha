#include "moha/runtime/app/subscribe.hpp"

#include <chrono>
#include <optional>
#include <variant>

namespace moha::app {

using maya::Sub;
using maya::KeyEvent;
using maya::CharKey;
using maya::SpecialKey;

namespace {

// ── Per-modal key handlers — return std::nullopt to fall through ──────────

std::optional<Msg> on_permission(const KeyEvent& ev) {
    if (auto* ck = std::get_if<CharKey>(&ev.key)) {
        switch (ck->codepoint) {
            case 'y': case 'Y': return PermissionApprove{};
            case 'n': case 'N': return PermissionReject{};
            case 'a': case 'A': return PermissionApproveAlways{};
        }
    }
    if (std::holds_alternative<SpecialKey>(ev.key)
        && std::get<SpecialKey>(ev.key) == SpecialKey::Escape)
        return PermissionReject{};
    return std::nullopt;
}

std::optional<Msg> on_command_palette(const KeyEvent& ev) {
    if (std::holds_alternative<SpecialKey>(ev.key)) {
        auto sk = std::get<SpecialKey>(ev.key);
        switch (sk) {
            case SpecialKey::Escape:    return CloseCommandPalette{};
            case SpecialKey::Enter:     return CommandPaletteSelect{};
            case SpecialKey::Up:        return CommandPaletteMove{-1};
            case SpecialKey::Down:      return CommandPaletteMove{+1};
            case SpecialKey::Backspace: return CommandPaletteBackspace{};
            default: break;
        }
    }
    if (auto* ck = std::get_if<CharKey>(&ev.key))
        return CommandPaletteInput{ck->codepoint};
    return std::nullopt;
}

std::optional<Msg> on_model_picker(const KeyEvent& ev) {
    if (std::holds_alternative<SpecialKey>(ev.key)) {
        auto sk = std::get<SpecialKey>(ev.key);
        switch (sk) {
            case SpecialKey::Escape: return CloseModelPicker{};
            case SpecialKey::Enter:  return ModelPickerSelect{};
            case SpecialKey::Up:     return ModelPickerMove{-1};
            case SpecialKey::Down:   return ModelPickerMove{+1};
            default: break;
        }
    }
    if (auto* ck = std::get_if<CharKey>(&ev.key))
        if (ck->codepoint == 'f' || ck->codepoint == 'F')
            return ModelPickerToggleFavorite{};
    return std::nullopt;
}

std::optional<Msg> on_thread_list(const KeyEvent& ev) {
    if (std::holds_alternative<SpecialKey>(ev.key)) {
        auto sk = std::get<SpecialKey>(ev.key);
        switch (sk) {
            case SpecialKey::Escape: return CloseThreadList{};
            case SpecialKey::Enter:  return ThreadListSelect{};
            case SpecialKey::Up:     return ThreadListMove{-1};
            case SpecialKey::Down:   return ThreadListMove{+1};
            default: break;
        }
    }
    if (auto* ck = std::get_if<CharKey>(&ev.key))
        if (ck->codepoint == 'n' || ck->codepoint == 'N') return NewThread{};
    return std::nullopt;
}

std::optional<Msg> on_diff_review(const KeyEvent& ev) {
    if (std::holds_alternative<SpecialKey>(ev.key)) {
        auto sk = std::get<SpecialKey>(ev.key);
        switch (sk) {
            case SpecialKey::Escape: return CloseDiffReview{};
            case SpecialKey::Up:     return DiffReviewMove{-1};
            case SpecialKey::Down:   return DiffReviewMove{+1};
            case SpecialKey::Left:   return DiffReviewPrevFile{};
            case SpecialKey::Right:  return DiffReviewNextFile{};
            default: break;
        }
    }
    if (auto* ck = std::get_if<CharKey>(&ev.key)) {
        switch (ck->codepoint) {
            case 'y': case 'Y': return AcceptHunk{};
            case 'n': case 'N': return RejectHunk{};
            case 'a': case 'A': return AcceptAllChanges{};
            case 'x': case 'X': return RejectAllChanges{};
        }
    }
    return std::nullopt;
}

std::optional<Msg> on_todo_modal(const KeyEvent& ev) {
    if (std::holds_alternative<SpecialKey>(ev.key)
        && std::get<SpecialKey>(ev.key) == SpecialKey::Escape)
        return CloseTodoModal{};
    return std::nullopt;
}

std::optional<Msg> on_global(const KeyEvent& ev) {
    if (ev.mods.ctrl) {
        if (auto* ck = std::get_if<CharKey>(&ev.key)) {
            switch (static_cast<char>(ck->codepoint)) {
                case 'c': case 'C': return Quit{};
                case '/':           return OpenModelPicker{};
                case 'j': case 'J': return OpenThreadList{};
                case 'k': case 'K': return OpenCommandPalette{};
                case 'r': case 'R': return OpenDiffReview{};
                case 'n': case 'N': return NewThread{};
                case 't': case 'T': return OpenTodoModal{};
                case 'e': case 'E': return ComposerToggleExpand{};
            }
        }
    }
    if (ev.mods.shift && std::holds_alternative<SpecialKey>(ev.key)) {
        auto sk = std::get<SpecialKey>(ev.key);
        if (sk == SpecialKey::Tab || sk == SpecialKey::BackTab)
            return CycleProfile{};
    }
    return std::nullopt;
}

std::optional<Msg> on_composer(const KeyEvent& ev) {
    if (std::holds_alternative<SpecialKey>(ev.key)) {
        auto sk = std::get<SpecialKey>(ev.key);
        switch (sk) {
            case SpecialKey::Enter:
                // Newline on Shift+Enter (chat-app muscle memory) OR
                // Alt+Enter (universal fallback for terminals that don't
                // speak KKP / modifyOtherKeys). Maya enables both
                // protocols on entry, but if the user's terminal ignores
                // both, Shift+Enter arrives as plain Enter and the only
                // way to insert a newline is Alt+Enter — the legacy
                // binding that EVERY terminal delivers as `\x1b\r`.
                // Plain Enter still submits.
                return (ev.mods.shift || ev.mods.alt)
                       ? Msg{ComposerNewline{}}
                       : Msg{ComposerEnter{}};
            case SpecialKey::Backspace: return ComposerBackspace{};
            case SpecialKey::Left:      return ComposerCursorLeft{};
            case SpecialKey::Right:     return ComposerCursorRight{};
            case SpecialKey::Home:      return ComposerCursorHome{};
            case SpecialKey::End:       return ComposerCursorEnd{};
            case SpecialKey::Escape:    return Quit{};
            default: return std::nullopt;
        }
    }
    if (auto* ck = std::get_if<CharKey>(&ev.key))
        if (ck->codepoint >= 0x20) return ComposerCharInput{ck->codepoint};
    return std::nullopt;
}

} // namespace

Sub<Msg> subscribe(const Model& m) {
    const bool in_perm    = m.d.pending_permission.has_value();
    const bool in_cmd     = m.ui.command_palette.open;
    const bool in_models  = m.ui.model_picker.open;
    const bool in_threads = m.ui.thread_list.open;
    const bool in_diff    = m.ui.diff_review.open;
    const bool in_todo    = m.ui.todo.open;
    const bool streaming  = m.s.active
                         && !m.s.is_awaiting_permission();

    auto key_sub = Sub<Msg>::on_key(
        [=](const KeyEvent& ev) -> std::optional<Msg> {
            if (in_perm)    return on_permission(ev);
            if (in_cmd)     return on_command_palette(ev);
            if (in_models)  return on_model_picker(ev);
            if (in_threads) return on_thread_list(ev);
            if (in_diff)    return on_diff_review(ev);
            if (in_todo)    if (auto r = on_todo_modal(ev)) return r;
            // Esc during a live stream cancels the request rather than
            // quitting the app. Modals above swallow Esc themselves, so this
            // only fires from the bare composer view.
            if (streaming
                && std::holds_alternative<SpecialKey>(ev.key)
                && std::get<SpecialKey>(ev.key) == SpecialKey::Escape)
                return CancelStream{};
            if (auto m = on_global(ev))   return m;
            return on_composer(ev);
        });

    auto paste_sub = Sub<Msg>::on_paste([](std::string s) -> Msg {
        return ComposerPaste{std::move(s)};
    });

    // Only subscribe to Tick while the spinner is visible. With fps=0 the
    // maya loop is purely event-driven; an unconditional 16ms tick would
    // force a render 60× per second even when nothing is changing.
    if (m.s.active) {
        auto tick = Sub<Msg>::every(std::chrono::milliseconds(33), Tick{});
        return Sub<Msg>::batch(std::move(key_sub), std::move(paste_sub), std::move(tick));
    }
    return Sub<Msg>::batch(std::move(key_sub), std::move(paste_sub));
}

} // namespace moha::app
