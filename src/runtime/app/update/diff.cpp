// diff_review_update — reducer for `msg::DiffReviewMsg`. Two-axis modal
// over (file_index, hunk_index); mutation = per-hunk Accepted/Rejected
// status flips; AcceptAll / RejectAll fan over every pending change at
// once. Emits status toasts via set_status_toast on the no-change paths
// so empty-state Enter doesn't feel silent.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"

#include <algorithm>
#include <utility>

#include <maya/core/overload.hpp>

#include "agentty/runtime/panel/common.hpp"
#include "agentty/runtime/app/deps.hpp"

namespace pn = agentty::ui::panel;

namespace agentty::app::detail {

namespace pick = agentty::ui::pick;
using maya::overload;

Step diff_review_update(Model m, msg::DiffReviewMsg dm) {
    // Persist one file's REVIEW DECISION to disk. The tool already wrote the
    // file when it ran, so `new_contents` is what's on disk now. Accept = keep;
    // Reject = revert. diff::apply_accepted() reconstructs the file with only
    // the accepted hunks applied on top of the original — so a file with any
    // rejected hunk gets rewritten, and an all-accepted file is left as-is.
    // Pending (undecided) hunks are treated as accepted on close (the change
    // is already live; not touching it keeps it).
    auto persist = [&](const FileChange& fc) {
        bool any_reject = false;
        for (const auto& hk : fc.hunks)
            if (hk.status == Hunk::Status::Rejected) { any_reject = true; break; }
        if (!any_reject) return;                 // nothing to revert; disk is correct
        if (deps().write_file)
            deps().write_file(fc.path, diff::apply_accepted(fc));
    };
    // Advance the cursor to the next still-PENDING hunk in the current file so
    // a decision flows the reviewer forward (like accepting a git add -p). If
    // none remain in this file, hop to the next file with pending hunks; wraps.
    auto advance = [&](pick::OpenAtCell* c) {
        const int nfiles = static_cast<int>(m.d.pending_changes.size());
        for (int fo = 0; fo < nfiles; ++fo) {
            int fi = (c->file_index + fo) % nfiles;
            const auto& hunks = m.d.pending_changes[static_cast<std::size_t>(fi)].hunks;
            int start = (fo == 0) ? c->hunk_index : 0;
            for (int ho = 0; ho < static_cast<int>(hunks.size()); ++ho) {
                int hi = (start + ho) % static_cast<int>(hunks.size());
                if (hunks[static_cast<std::size_t>(hi)].status == Hunk::Status::Pending) {
                    c->file_index = fi; c->hunk_index = hi;
                    c->body_scroll = 0;   // fresh hunk → top
                    return;
                }
            }
        }
        // Nothing pending anywhere — leave the cursor where it is.
    };

    // Clamp a possibly-stale cursor against the CURRENT changeset. Hunk
    // counts can shrink while the pane is open: a new tool edit to the same
    // path collapses into its existing review entry (*it = diff::compute(
    // original, latest)), which may yield fewer hunks than the cursor's
    // index. The view clamps defensively for render; the reducer must too or
    // fc.hunks[c->hunk_index] is out-of-bounds UB on the next y/n/Enter.
    auto clamp_cursor = [&](pick::OpenAtCell* c) -> FileChange* {
        if (!c || m.d.pending_changes.empty()) return nullptr;
        const int nfiles = static_cast<int>(m.d.pending_changes.size());
        if (c->file_index >= nfiles) c->file_index = nfiles - 1;
        if (c->file_index < 0)       c->file_index = 0;
        auto& fc = m.d.pending_changes[static_cast<std::size_t>(c->file_index)];
        const int nh = static_cast<int>(fc.hunks.size());
        if (c->hunk_index >= nh) c->hunk_index = nh > 0 ? nh - 1 : 0;
        if (c->hunk_index < 0)   c->hunk_index = 0;
        return &fc;
    };

    // Disarm the two-press ^X guard on ANY diff-review action other than the
    // confirming second ^X — a stray first press must not leave a live
    // "next ^X nukes everything" trap behind a j/k or scroll.
    if (!std::holds_alternative<RejectAllChanges>(dm))
        if (auto* c = m.ui.panel.get<pn::DiffReview>())
            c->confirm_reject_all = false;

    return std::visit(overload{
        [&](OpenDiffReview) -> Step {
            // Tell the user when there's nothing to review instead of
            // silently doing nothing — opening an empty pane would just
            // flicker the screen and leave them confused about whether
            // their keystroke registered.
            if (m.d.pending_changes.empty()) {
                auto cmd = set_status_toast(m, "no pending changes to review");
                return {std::move(m), std::move(cmd)};
            }
            m.ui.panel = pn::DiffReview{{0, 0}};
            return done(std::move(m));
        },
        [&](CloseDiffReview) -> Step {
            // Persist every file's decision on the way out, then clear the
            // queue — closing the pane commits the review. Say WHAT closing
            // meant: undecided hunks are kept (the change is already live on
            // disk), which is invisible unless we announce it.
            int reverted = 0, kept = 0;
            for (const auto& fc : m.d.pending_changes) {
                for (const auto& hk : fc.hunks) {
                    if (hk.status == Hunk::Status::Rejected) ++reverted;
                    else ++kept;   // accepted OR pending — both stay live
                }
                persist(fc);
            }
            m.d.pending_changes.clear();
            m.ui.panel.close<pn::DiffReview>();
            auto cmd = set_status_toast(m,
                reverted == 0
                    ? "review closed — all " + std::to_string(kept)
                        + (kept == 1 ? " change" : " changes") + " kept"
                    : "review closed — " + std::to_string(reverted)
                        + " reverted, " + std::to_string(kept) + " kept");
            return {std::move(m), std::move(cmd)};
        },
        [&](DiffReviewMove& e) -> Step {
            auto* c = m.ui.panel.get<pn::DiffReview>();
            auto* fc = clamp_cursor(c);
            if (!fc) return done(std::move(m));
            int sz = static_cast<int>(fc->hunks.size());
            if (sz == 0) return done(std::move(m));
            c->hunk_index = (c->hunk_index + e.delta + sz) % sz;
            c->body_scroll = 0;   // a newly-focused hunk starts at its top
            return done(std::move(m));
        },
        [&](DiffReviewScroll& e) -> Step {
            auto* c = m.ui.panel.get<pn::DiffReview>();
            auto* fc = clamp_cursor(c);
            if (!fc || fc->hunks.empty()) return done(std::move(m));
            const auto& hk =
                fc->hunks[static_cast<std::size_t>(c->hunk_index)];
            // Upper bound on the hunk's body rows: its patch line count.
            // The view clamps precisely against the parsed row count; this
            // just keeps the offset from running away unboundedly.
            const int max_rows = static_cast<int>(
                std::count(hk.patch.begin(), hk.patch.end(), '\n')) + 1;
            c->body_scroll = std::clamp(c->body_scroll + e.delta,
                                        0, std::max(0, max_rows - 1));
            return done(std::move(m));
        },
        [&](DiffReviewNextFile) -> Step {
            auto* c = m.ui.panel.get<pn::DiffReview>();
            if (!c || m.d.pending_changes.empty()) return done(std::move(m));
            int sz = static_cast<int>(m.d.pending_changes.size());
            c->file_index = (c->file_index + 1) % sz;
            c->hunk_index = 0;
            c->body_scroll = 0;
            return done(std::move(m));
        },
        [&](DiffReviewPrevFile) -> Step {
            auto* c = m.ui.panel.get<pn::DiffReview>();
            if (!c || m.d.pending_changes.empty()) return done(std::move(m));
            int sz = static_cast<int>(m.d.pending_changes.size());
            c->file_index = (c->file_index - 1 + sz) % sz;
            c->hunk_index = 0;
            c->body_scroll = 0;
            return done(std::move(m));
        },
        [&](AcceptHunk) -> Step {
            auto* c = m.ui.panel.get<pn::DiffReview>();
            if (auto* fc = clamp_cursor(c)) {
                if (!fc->hunks.empty())
                    fc->hunks[static_cast<std::size_t>(c->hunk_index)].status =
                        Hunk::Status::Accepted;
                advance(c);
            }
            return done(std::move(m));
        },
        [&](RejectHunk) -> Step {
            auto* c = m.ui.panel.get<pn::DiffReview>();
            if (auto* fc = clamp_cursor(c)) {
                if (!fc->hunks.empty())
                    fc->hunks[static_cast<std::size_t>(c->hunk_index)].status =
                        Hunk::Status::Rejected;
                advance(c);
            }
            return done(std::move(m));
        },
        [&](AcceptAllChanges) -> Step {
            if (m.d.pending_changes.empty()) {
                auto cmd = set_status_toast(m, "no pending changes to accept");
                return {std::move(m), std::move(cmd)};
            }
            // Accept = keep what the tools already wrote; nothing to persist.
            int hunks = 0;
            for (auto& fc : m.d.pending_changes)
                for (auto& h : fc.hunks) { h.status = Hunk::Status::Accepted; ++hunks; }
            m.d.pending_changes.clear();
            m.ui.panel.close<pn::DiffReview>();
            auto cmd = set_status_toast(m,
                "accepted " + std::to_string(hunks)
                + (hunks == 1 ? " hunk" : " hunks"));
            return {std::move(m), std::move(cmd)};
        },
        [&](RejectAllChanges) -> Step {
            if (m.d.pending_changes.empty()) {
                auto cmd = set_status_toast(m, "no pending changes to reject");
                return {std::move(m), std::move(cmd)};
            }
            // TWO-PRESS guard when driven from the open pane (^X): the first
            // press arms, the second executes. A palette "Reject all" (pane
            // closed) is already a deliberate multi-step action — execute
            // immediately. Mirrors the thread picker's two-press delete.
            if (auto* c = m.ui.panel.get<pn::DiffReview>();
                c && !c->confirm_reject_all) {
                c->confirm_reject_all = true;
                auto cmd = set_status_toast(m,
                    "press ^X again to revert ALL changes — any other key cancels");
                return {std::move(m), std::move(cmd)};
            }
            // Reject ALL = revert every touched file to its original contents
            // on disk (the tools already wrote the new version, so this undoes
            // them). apply_accepted() with every hunk Rejected yields exactly
            // original_contents.
            int hunks = 0, files = 0;
            for (auto& fc : m.d.pending_changes) {
                for (auto& h : fc.hunks) { h.status = Hunk::Status::Rejected; ++hunks; }
                if (deps().write_file) {
                    deps().write_file(fc.path, fc.original_contents);
                    ++files;
                }
            }
            m.d.pending_changes.clear();
            m.ui.panel.close<pn::DiffReview>();
            auto cmd = set_status_toast(m,
                "reverted " + std::to_string(hunks)
                + (hunks == 1 ? " hunk" : " hunks")
                + " across " + std::to_string(files)
                + (files == 1 ? " file" : " files"));
            return {std::move(m), std::move(cmd)};
        },
    }, dm);
}

} // namespace agentty::app::detail
