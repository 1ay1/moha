// command_palette_test — locks the Ctrl+K palette's row catalog + filter UX.
//
// The palette is pure data (kCommands) + one pure function (filtered_commands),
// so it's cheaply unit-testable without any UI/runtime. This pins the two
// invariants that keep every row working and discoverable:
//   1. CATALOG COMPLETENESS — every Command enum value has exactly one row.
//      A missing row = a dead palette entry; the dispatcher switch would still
//      compile but the command could never be selected.
//   2. FILTER UX — matching spans label + description + shortcut (discovery by
//      intent), and label hits rank above description-only hits.

#include "agtest.hpp"

#include "agentty/runtime/panel/palette.hpp"

#include <array>
#include <string_view>

using namespace agentty;


// Every enum value, so we can assert the catalog covers each exactly once.
static constexpr std::array kAll = {
    Command::NewThread, Command::ReviewChanges, Command::ToggleChangesStrip,
    Command::AcceptAll,
    Command::RejectAll, Command::CycleProfile, Command::OpenModels,
    Command::SwapModel,
    Command::OpenProviders, Command::OpenThreads, Command::OpenPlan,
    Command::RunCodeBlock, Command::InspectToolOutputs, Command::CompactContext,
    Command::SmartMode,
    Command::RewindCheckpoint, Command::ForkThread,
    Command::OpenPlugins, Command::OpenCommands, Command::OpenAgents, Command::OpenHooks,
    Command::OpenRag, Command::OpenLogin,
    Command::SignOut, Command::UpdateAgentty, Command::Quit,
};

static bool has_id(const std::vector<const CommandDef*>& v, Command id) {
    for (const auto* c : v) if (c->id == id) return true;
    return false;
}

TEST_CASE("command palette") {
    // ── 1. catalog completeness (enum ⇄ kCommands bijection) ──────────────
    check(kCommands.size() == kAll.size(),
          "kCommands has exactly one row per Command enum value");
    for (Command id : kAll) {
        int n = 0;
        for (const auto& c : kCommands) if (c.id == id) ++n;
        check(n == 1, "each Command id appears exactly once in kCommands");
    }
    // Every row has a non-empty label + description (no blank palette rows).
    for (const auto& c : kCommands) {
        check(c.label && *c.label, "row has a label");
        check(c.description && *c.description, "row has a description");
        check(c.shortcut != nullptr, "row has a (possibly empty) shortcut");
    }

    // ── 2. empty query returns every row, in catalog order ────────────────
    {
        auto all = filtered_commands("");
        check(all.size() == kCommands.size(), "empty query returns all rows");
        bool ordered = true;
        for (std::size_t i = 0; i < all.size(); ++i)
            if (all[i]->id != kCommands[i].id) ordered = false;
        check(ordered, "empty query preserves catalog order");
    }

    // ── 3. label match ────────────────────────────────────────────────────
    {
        auto r = filtered_commands("thread");
        check(has_id(r, Command::NewThread) && has_id(r, Command::OpenThreads),
              "\"thread\" matches New thread + Open threads");
    }

    // ── 4. DESCRIPTION match (discovery by intent) ────────────────────────
    {
        // "diff" is only in Review changes' DESCRIPTION, not its label.
        auto r = filtered_commands("diff");
        check(has_id(r, Command::ReviewChanges),
              "\"diff\" finds Review changes via its description");
    }
    {
        // "summary" only in Compact context's description.
        auto r = filtered_commands("summary");
        check(has_id(r, Command::CompactContext),
              "\"summary\" finds Compact context via description");
    }

    // ── 5. SHORTCUT match ─────────────────────────────────────────────────
    {
        auto r = filtered_commands("ctrl+g");
        check(has_id(r, Command::RunCodeBlock),
              "\"ctrl+g\" finds Run code block via its shortcut");
    }

    // ── 6. case-insensitive ───────────────────────────────────────────────
    {
        auto r = filtered_commands("QUIT");
        check(has_id(r, Command::Quit), "filter is case-insensitive");
    }

    // ── 7. label hits rank ABOVE description-only hits ────────────────────
    {
        // "changes" is in the LABELS of AcceptAll/RejectAll/ReviewChanges and
        // also in the DESCRIPTION of RewindCheckpoint ("...conversation...").
        // Whatever matches by label must precede any description-only match.
        auto r = filtered_commands("change");
        // Find the first description-only match and the last label match;
        // assert no label match comes after a description-only one.
        bool seen_desc_only = false, ok = true;
        for (const auto* c : r) {
            std::string_view lab{c->label};
            bool label_hit = false;
            std::string low;
            for (char ch : lab) low.push_back(
                static_cast<char>(std::tolower((unsigned char)ch)));
            label_hit = low.find("change") != std::string_view::npos;
            if (!label_hit) seen_desc_only = true;
            else if (seen_desc_only) ok = false;   // label hit after a desc-only
        }
        check(ok, "label matches rank above description-only matches");
    }

    // ── 8. no match → empty ──────────────────────────────────────────────
    check(filtered_commands("zznotacommandzz").empty(),
          "a non-matching query returns no rows");
}

TEST_CASE("command palette — categories, gating, danger") {
    // ── every row has a category; danger rows are exactly the destructive set
    {
        // The commands that discard work or mutate the worktree.
        auto is_expected_danger = [](Command c) {
            return c == Command::RejectAll || c == Command::RewindCheckpoint
                || c == Command::SignOut;
        };
        for (const auto& c : kCommands) {
            check(c.danger == is_expected_danger(c.id),
                  "danger flag matches the destructive-action set");
            // category_label is empty only for General (Quit / Update).
            bool general = (c.category == Category::General);
            check(category_label(c.category).empty() == general,
                  "only General rows have no category badge");
        }
    }

    // ── category name is searchable (discovery by section) ────────────────
    {
        auto r = filtered_commands("changes");
        // The Changes cluster surfaces as a group.
        check(has_id(r, Command::ReviewChanges) && has_id(r, Command::AcceptAll)
              && has_id(r, Command::RejectAll),
              "\"changes\" surfaces the whole Changes category");
    }

    // ── visibility gating: dead BULK rows are hidden, entry point stays ──
    {
        // No pending diff → Accept-all / Reject-all disappear, but "Review
        // changes" (the entry point) stays discoverable.
        PaletteContext no_diff;
        no_diff.has_pending_changes = false;
        auto r = filtered_commands("", no_diff);
        check(!has_id(r, Command::AcceptAll) && !has_id(r, Command::RejectAll),
              "no pending changes hides the bulk Accept/Reject-all");
        check(has_id(r, Command::ReviewChanges),
              "Review changes stays visible (entry point, toasts when empty)");
        check(has_id(r, Command::NewThread), "unrelated commands stay visible");

        // With a diff the bulk actions come back.
        PaletteContext with_diff;
        with_diff.has_pending_changes = true;
        check(has_id(filtered_commands("", with_diff), Command::AcceptAll),
              "pending changes shows Accept-all");
    }
    {
        // No fenced reply → Run code block hidden.
        PaletteContext no_block;
        no_block.has_code_block = false;
        check(!has_id(filtered_commands("", no_block), Command::RunCodeBlock),
              "no code block hides Run code block");
    }
    {
        // No update available → Update agentty hidden (pre-existing contract).
        PaletteContext no_update;
        no_update.update_available = false;
        check(!has_id(filtered_commands("", no_update), Command::UpdateAgentty),
              "no update hides Update agentty");
    }

    // ── command_visible predicate agrees with the filter ─────────────────
    {
        PaletteContext ctx;
        ctx.has_pending_changes = false;
        for (const auto& c : kCommands) {
            bool in_filter = has_id(filtered_commands("", ctx), c.id);
            check(in_filter == command_visible(c, ctx),
                  "filter visibility == command_visible predicate");
        }
    }
}

TEST_CASE("command palette — scored fuzzy ranking") {
    auto rank_of = [&](const std::vector<CommandMatch>& v, Command id) -> int {
        for (int i = 0; i < (int)v.size(); ++i) if (v[(std::size_t)i].cmd->id == id) return i;
        return -1;
    };

    // ── "re": label PREFIX hits rank above a description-only match ──────
    {
        auto r = match_commands("re", PaletteContext{});
        int rev = rank_of(r, Command::ReviewChanges);
        int rew = rank_of(r, Command::RewindCheckpoint);
        int rej = rank_of(r, Command::RejectAll);
        int nt  = rank_of(r, Command::NewThread);   // matches "f-re-sh" in desc
        check(rev >= 0 && rew >= 0 && rej >= 0, "the Re* commands all match");
        check(nt < 0 || (rev < nt && rew < nt && rej < nt),
              "label 're' prefix hits outrank a description-only 're'");
    }

    // ── acronym typing: "rcb" → Run Code Block via word boundaries ───────
    {
        auto r = match_commands("rcb", PaletteContext{});
        check(!r.empty() && r.front().cmd->id == Command::RunCodeBlock,
              "'rcb' acronym-matches 'Run Code Block' first");
    }

    // ── positions point at the matched label characters ──────────────────
    {
        auto r = match_commands("new", PaletteContext{});
        check(!r.empty() && r.front().cmd->id == Command::NewThread,
              "'new' → New thread first");
        // "New thread": 'n','e','w' at offsets 0,1,2.
        const auto& pos = r.front().positions;
        check(pos.size() == 3 && pos[0] == 0 && pos[1] == 1 && pos[2] == 2,
              "positions mark the matched leading characters");
    }

    // ── exact label wins over a longer label that also matches ──────────
    {
        auto r = match_commands("quit", PaletteContext{});
        check(!r.empty() && r.front().cmd->id == Command::Quit,
              "'quit' → Quit ranks first (exact label)");
    }

    // ── empty query: catalog order, no positions ───────────────────────
    {
        auto r = match_commands("", PaletteContext{});
        check(r.front().cmd->id == kCommands.front().id,
              "empty query preserves catalog order");
        check(r.front().positions.empty(), "empty query has no highlight positions");
    }
}
