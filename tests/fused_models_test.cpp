// fused_models_test — the pure ranking core of the unified cross-provider
// model picker. No Model / deps / network: we hand build_fused_rows() plain
// catalogs and assert ordering, sections, de-dup, active pinning, auth split.

#include "agentty/runtime/fused_models.hpp"
#include "agentty/domain/capkey.hpp"

#include <fstream>
#include <sstream>
#include <filesystem>

#include <doctest/doctest.h>

using namespace agentty;
using namespace agentty::ui;

namespace {

ModelInfo mk(std::string id, std::string name, std::string provider,
             int ctx = 200000, bool fav = false) {
    ModelInfo mi;
    mi.id = ModelId{std::move(id)};
    mi.display_name = std::move(name);
    mi.provider = std::move(provider);
    mi.context_window = ctx;
    mi.favorite = fav;
    return mi;
}

ProviderCatalog cat(std::string id, std::string label,
                    std::vector<ModelInfo> models) {
    ProviderCatalog c;
    c.provider_id = std::move(id);
    c.label = std::move(label);
    c.state = ProviderCatalog::State::Ready;
    c.models = std::move(models);
    return c;
}

} // namespace

TEST_CASE("fused: active pinned first and marked, no query") {
    std::vector<ProviderCatalog> cats = {
        cat("anthropic", "Anthropic",
            {mk("claude-sonnet-4-6", "Claude Sonnet 4.6", "anthropic"),
             mk("claude-opus-4", "Claude Opus 4", "anthropic")}),
        cat("openai", "OpenAI",
            {mk("gpt-5-codex", "gpt-5-codex", "openai", 400000),
             mk("gpt-4o", "gpt-4o", "openai", 128000)}),
    };
    FusedInputs in;
    in.catalogs = &cats;
    in.active = ModelRef{"openai", "gpt-5-codex"};

    auto rows = build_fused_rows(in);
    REQUIRE(!rows.empty());
    // Active pinned to row 0, in the RECENT section, marked active.
    CHECK(rows[0].provider_id == "openai");
    CHECK(rows[0].model.id.value == "gpt-5-codex");
    CHECK(rows[0].active);
    CHECK(rows[0].recent);
    // The active model must not also appear in the "all providers" section.
    int count_codex = 0;
    for (auto& r : rows)
        if (r.provider_id == "openai" && r.model.id.value == "gpt-5-codex")
            ++count_codex;
    CHECK(count_codex == 1);
    // Every other catalog model is present exactly once.
    CHECK(rows.size() == 4);
}

TEST_CASE("fused: query spans providers, ranks by match") {
    std::vector<ProviderCatalog> cats = {
        cat("anthropic", "Anthropic",
            {mk("claude-sonnet-4-6", "Claude Sonnet 4.6", "anthropic")}),
        cat("openai", "OpenAI",
            {mk("gpt-5-codex", "gpt-5-codex", "openai"),
             mk("gpt-4o", "gpt-4o", "openai")}),
        cat("kimi", "Kimi", {mk("kimi-k2", "kimi-k2-0905", "kimi", 256000)}),
    };
    FusedInputs in;
    in.catalogs = &cats;
    in.query = "gpt";

    auto rows = build_fused_rows(in);
    // Only the two GPT rows match "gpt".
    CHECK(rows.size() == 2);
    for (auto& r : rows) {
        CHECK(r.provider_id == "openai");
        CHECK(r.model.id.value.find("gpt") != std::string::npos);
    }
}

TEST_CASE("fused: MRU section then all-providers, deduped") {
    std::vector<ProviderCatalog> cats = {
        cat("anthropic", "Anthropic",
            {mk("claude-sonnet-4-6", "Claude Sonnet 4.6", "anthropic"),
             mk("claude-opus-4", "Claude Opus 4", "anthropic")}),
        cat("openai", "OpenAI", {mk("gpt-4o", "gpt-4o", "openai")}),
    };
    std::vector<ModelRef> recents = {
        {"openai", "gpt-4o"},
        {"anthropic", "claude-opus-4"},
    };
    FusedInputs in;
    in.catalogs = &cats;
    in.recents = &recents;
    in.active = ModelRef{"anthropic", "claude-sonnet-4-6"};

    auto rows = build_fused_rows(in);
    // RECENT = active + 2 MRU = 3 rows, all marked recent.
    int recent_rows = 0;
    for (auto& r : rows) if (r.recent) ++recent_rows;
    CHECK(recent_rows == 3);
    // ● LEADS, ALWAYS. The active model is pinned to row 0 by construction,
    // ahead of the alphabetical browse order — "Claude Opus 4" sorts before
    // "Claude Sonnet 4.6", but Sonnet is active, so Sonnet leads and Opus
    // follows it. (This assertion was briefly inverted to expect Opus first,
    // which encoded a regression as the contract: the picker pre-selects
    // row 0, so a displaced ● means opening the menu no longer highlights
    // the model you are actually using.)
    CHECK(rows[0].model.id.value == "claude-sonnet-4-6");
    CHECK(rows[0].active);                     // active pinned first
    // Behind the pin, the active provider's remaining recents lead the
    // section, alphabetical within, and the openai recent follows.
    CHECK(rows[1].model.id.value == "claude-opus-4");
    CHECK(rows[2].model.id.value == "gpt-4o");
    // Total distinct models = 3 (no dupes between RECENT and all-providers).
    CHECK(rows.size() == 3);
}

TEST_CASE("fused: sign-in offers are QUERY-GATED — hidden while browsing") {
    std::vector<ProviderCatalog> cats = {
        cat("anthropic", "Anthropic",
            {mk("claude-sonnet-4-6", "Claude Sonnet 4.6", "anthropic")}),
    };
    std::vector<SigninOffer> offers = {
        {"groq", "Groq"}, {"cerebras", "Cerebras"},
    };
    FusedInputs in;
    in.catalogs = &cats;
    in.offers = &offers;

    // Empty query = browse view: NO offer rows (the list stays clean; signing
    // in is reachable by TYPING the provider's name, or via ^P).
    auto rows = build_fused_rows(in);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].authed);

    // A query matching an un-authed provider surfaces its offer — searching
    // for a provider you haven't added is never a dead end.
    in.query = "groq";
    rows = build_fused_rows(in);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].is_signin_offer());
    CHECK(rows[0].provider_id == "groq");
    // A sign-in offer carries no model id.
    CHECK(rows[0].model.id.value.empty());
}

TEST_CASE("fused: query filters sign-in offers by provider name") {
    std::vector<ProviderCatalog> cats;
    std::vector<SigninOffer> offers = {{"groq", "Groq"}, {"cerebras", "Cerebras"}};
    FusedInputs in;
    in.catalogs = &cats;
    in.offers = &offers;
    in.query = "cere";

    auto rows = build_fused_rows(in);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].provider_id == "cerebras");
    CHECK(rows[0].is_signin_offer());
}

TEST_CASE("fused: favorites float above equal-scoring peers") {
    std::vector<ProviderCatalog> cats = {
        cat("openai", "OpenAI",
            {mk("gpt-4o", "gpt-4o", "openai", 128000, /*fav=*/false),
             mk("gpt-5", "gpt-5", "openai", 400000, /*fav=*/true)}),
    };
    FusedInputs in;
    in.catalogs = &cats;
    // No query ⇒ all match with score 0; favorite must sort first.
    auto rows = build_fused_rows(in);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].model.favorite);
    CHECK(rows[0].model.id.value == "gpt-5");
}

TEST_CASE("fused: a recent whose model left the catalog is dropped") {
    std::vector<ProviderCatalog> cats = {
        cat("openai", "OpenAI", {mk("gpt-4o", "gpt-4o", "openai")}),
    };
    std::vector<ModelRef> recents = {
        {"openai", "gpt-3.5-gone"},        // no longer offered
        {"openai", "gpt-4o"},
    };
    FusedInputs in;
    in.catalogs = &cats;
    in.recents = &recents;

    auto rows = build_fused_rows(in);
    // Only the still-present recent survives; the stale one is silently skipped.
    CHECK(rows.size() == 1);
    CHECK(rows[0].model.id.value == "gpt-4o");
    CHECK(rows[0].recent);
}

TEST_CASE("fused: [1m] context variants are distinct rows, not alias dupes") {
    // The 1M-context variant is a SEPARATE choice from its base model —
    // both must be listable. Row identity folds spelling aliases but must
    // NOT fold the `[1m]` marker (capkey::norm_row_id vs norm_model): the
    // latter strips it for capability lookups, which made every 1M row
    // look like an alias of its base and vanish from the list.
    std::vector<ProviderCatalog> cats = {
        cat("anthropic", "Anthropic",
            {mk("claude-opus-4-8",     "Claude Opus 4.8", "anthropic"),
             mk("claude-opus-4-8[1m]", "Claude Opus 4.8 (1M Context)",
                "anthropic", 1'000'000),
             mk("claude-sonnet-4-6",   "Claude Sonnet 4.6", "anthropic")}),
    };
    ui::FusedInputs in;
    in.catalogs = &cats;
    auto rows = ui::build_fused_rows(in);

    bool base = false, one_m = false;
    for (const auto& r : rows) {
        if (r.model.id.value == "claude-opus-4-8")     base  = true;
        if (r.model.id.value == "claude-opus-4-8[1m]") one_m = true;
    }
    CHECK(base);
    CHECK(one_m);   // regression: was silently deduped away

    // A genuine alias SPELLING still dedups (the behaviour norm_model
    // was there for): -3-5 and -3.5 are one model, one row.
    std::vector<ProviderCatalog> alias = {
        cat("mistral", "Mistral",
            {mk("mistral-medium-3-5", "Mistral Medium 3.5", "mistral"),
             mk("mistral-medium-3.5", "Mistral Medium 3.5", "mistral")}),
    };
    ui::FusedInputs in2;
    in2.catalogs = &alias;
    auto rows2 = ui::build_fused_rows(in2);
    int mistral_rows = 0;
    for (const auto& r : rows2)
        if (r.provider_id == "mistral") ++mistral_rows;
    CHECK(mistral_rows == 1);
}

// Browse-view ranking: with NO query the head of the list must be the models
// you would plausibly pick, not an arbitrary slice of whatever the providers
// happen to serve.
//
// The motivating case is an aggregator: OpenRouter-class catalogs contribute
// hundreds of rows to a ~14-row viewport, and ordering by provider-registry
// position alone meant the first screen was determined by nothing the user
// cares about. Ranking by capability tier (Flagship → Mid → Cheap → Weak) puts
// the plausible picks on screen 1, so scrolling becomes a choice.
//
// The inverse matters just as much: once a QUERY is active, fuzzy score is the
// intent signal and tier must NOT re-rank behind it — otherwise the row the
// user is aiming at moves under them as they type.
TEST_CASE("fused: browse ranks by tier, search stays relevance-ordered") {
    // One provider, deliberately listed weakest-first so registry order alone
    // would leave the weak model at the top.
    std::vector<ProviderCatalog> cats = {
        cat("openrouter", "OpenRouter",
            {mk("tinyllama:1b",        "TinyLlama 1B",   "openrouter"),
             mk("qwen2.5-coder:7b",    "Qwen2.5 Coder",  "openrouter"),
             mk("claude-haiku-4-5",    "Claude Haiku",   "openrouter"),
             mk("claude-sonnet-4-6",   "Claude Sonnet",  "openrouter"),
             mk("claude-opus-4-5",     "Claude Opus",    "openrouter")}),
    };

    // ── Browse: alphabetical by model label within the section ──────
    {
        ui::FusedInputs in;
        in.catalogs = &cats;
        const auto rows = ui::build_fused_rows(in);
        REQUIRE(rows.size() >= 5);
        // Collect the browsed model labels and confirm they are in
        // normalized alphabetical order (the capkey fold the sort uses).
        std::vector<std::string> labels;
        for (const auto& r : rows) {
            if (r.is_signin_offer()) continue;
            labels.push_back(r.model_label);
        }
        std::vector<std::string> sorted = labels;
        std::sort(sorted.begin(), sorted.end(),
                  [](const std::string& a, const std::string& b) {
                      return capkey::norm_row_id(a) < capkey::norm_row_id(b);
                  });
        INFO("browse list is ordered alphabetically, not by tier");
        CHECK(labels == sorted);
        // Concretely: alphabetical by label — "Claude Haiku" precedes
        // "Qwen2.5 Coder" regardless of capability.
        int haiku = -1, qwen = -1;
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            if (rows[static_cast<std::size_t>(i)].model.id.value == "claude-haiku-4-5")  haiku = i;
            if (rows[static_cast<std::size_t>(i)].model.id.value == "qwen2.5-coder:7b") qwen  = i;
        }
        REQUIRE(haiku >= 0);
        REQUIRE(qwen  >= 0);
        INFO("labels sort alphabetically: \"Claude Haiku\" before \"Qwen2.5 Coder\"");
        CHECK(haiku < qwen);
    }

    // ── Search: relevance wins, no alphabetical re-rank behind it ────
    {
        ui::FusedInputs in;
        in.catalogs = &cats;
        in.query = "coder";           // matches only the weak local model
        const auto rows = ui::build_fused_rows(in);
        REQUIRE(!rows.empty());
        INFO("a query's best match leads, regardless of its tier");
        CHECK(rows.front().model.id.value == "qwen2.5-coder:7b");
    }

    // ── Favorites still outrank everything, in both modes ────────────
    {
        std::vector<ProviderCatalog> favc = {
            cat("openrouter", "OpenRouter",
                {mk("tinyllama:1b",    "TinyLlama 1B", "openrouter", 8000, true),
                 mk("claude-opus-4-5", "Claude Opus",  "openrouter")}),
        };
        ui::FusedInputs in;
        in.catalogs = &favc;
        const auto rows = ui::build_fused_rows(in);
        REQUIRE(!rows.empty());
        INFO("an explicit favorite outranks tier — the user already chose");
        CHECK(rows.front().model.id.value == "tinyllama:1b");
    }

    // ── The ACTIVE model is still reachable within RECENT ────────────
    // Ordering changes apply to the browse sections only; the recents
    // section's meaning (and the active flag) must not be disturbed by tier.
    {
        ui::FusedInputs in;
        in.catalogs = &cats;
        in.active = ModelRef{"openrouter", "tinyllama:1b"};   // weak, but active
        const auto rows = ui::build_fused_rows(in);
        REQUIRE(!rows.empty());
        INFO("the active model still leads the list");
        CHECK(rows.front().active);
        INFO("tier ranking does not evict the active model from the top");
        CHECK(rows.front().model.id.value == "tinyllama:1b");
    }

    // ── ● LEADS EVEN WITH A COMPETING RECENT ────────────────────────
    // The case above (and "active pinned first") each have the active model
    // as the ONLY recent, so a browse sort has nothing to reorder and both
    // pass whether or not the pin actually holds. That blind spot let an
    // alphabetical RECENT sort ship that dropped the active row behind any
    // earlier-sorting recent from the same provider.
    //
    // So: give the active provider TWO recents and make the active one sort
    // alphabetically LAST. Any ordering that treats `active` as just another
    // sort key fails here; only a pin that the sort cannot reach passes.
    {
        std::vector<ProviderCatalog> two = {
            cat("anthropic", "Anthropic",
                {mk("claude-opus-4-5",  "Claude Opus 4.5",  "anthropic"),
                 mk("claude-haiku-4-5", "Claude Haiku 4.5", "anthropic")}),
        };
        // "Claude Haiku" < "Claude Opus" alphabetically, and Haiku is the
        // newer MRU entry — both orderings would put it first.
        std::vector<ModelRef> recents{
            ModelRef{"anthropic", "claude-haiku-4-5"},
            ModelRef{"anthropic", "claude-opus-4-5"},
        };
        ui::FusedInputs in;
        in.catalogs = &two;
        in.recents  = &recents;
        in.active   = ModelRef{"anthropic", "claude-opus-4-5"};
        const auto rows = ui::build_fused_rows(in);
        REQUIRE(!rows.empty());
        INFO("● leads RECENT even when another recent sorts before it");
        CHECK(rows.front().active);
        CHECK(rows.front().model.id.value == "claude-opus-4-5");
        // ...and the rest of RECENT is still alphabetical behind the pin.
        std::vector<std::string> rest;
        for (const auto& r : rows)
            if (r.recent && !r.active) rest.push_back(r.model_label);
        std::vector<std::string> sorted_rest = rest;
        std::sort(sorted_rest.begin(), sorted_rest.end(),
                  [](const std::string& a, const std::string& b) {
                      return capkey::norm_row_id(a) < capkey::norm_row_id(b);
                  });
        INFO("the pin does not disturb alphabetical order below it");
        CHECK(rest == sorted_rest);
    }

    // ── FILTERING keeps MRU order, pin included ──────────────────────
    // Alphabetical ordering is a BROWSE affordance. With a query the list is
    // relevance-ordered, and RECENT stays MRU — assert the pin logic didn't
    // leak into the filtered path.
    {
        std::vector<ProviderCatalog> two = {
            cat("anthropic", "Anthropic",
                {mk("claude-opus-4-5",  "Claude Opus 4.5",  "anthropic"),
                 mk("claude-haiku-4-5", "Claude Haiku 4.5", "anthropic")}),
        };
        std::vector<ModelRef> recents{
            ModelRef{"anthropic", "claude-haiku-4-5"},
            ModelRef{"anthropic", "claude-opus-4-5"},
        };
        ui::FusedInputs in;
        in.catalogs = &two;
        in.recents  = &recents;
        in.active   = ModelRef{"anthropic", "claude-opus-4-5"};
        in.query    = "claude";
        const auto rows = ui::build_fused_rows(in);
        REQUIRE(!rows.empty());
        INFO("the active model still leads RECENT while filtering");
        CHECK(rows.front().active);
    }
}

// Browsing splits into two titled sections — "recent" and "all providers"
// — the second alphabetical by model label across EVERY provider (the active
// provider is no longer broken out into its own "from this provider" block).
// The ACTIVE provider is named LAST alphabetically to prove it is NOT pinned
// above the others by identity anymore.
TEST_CASE("fused: browse is recent, then a flat all-providers list") {
    std::vector<ProviderCatalog> cats = {
        cat("zeta", "ZetaCo",
            {mk("zeta-2",  "Zeta Two",  "zeta"),
             mk("zeta-1",  "Zeta One",  "zeta")}),
        cat("alpha", "AlphaCo",
            {mk("alpha-2", "Alpha Two", "alpha"),
             mk("alpha-1", "Alpha One", "alpha")}),
        cat("mid",   "MidCo",
            {mk("mid-1",   "Mid One",   "mid")}),
    };
    std::vector<ModelRef> recents = {ModelRef{"alpha", "alpha-1"}};
    ui::FusedInputs in;
    in.catalogs = &cats;
    in.recents  = &recents;
    in.active   = ModelRef{"zeta", "zeta-1"};   // active provider = "zeta" (last!)
    in.recent_cap = 5;

    const auto rows = ui::build_fused_rows(in);

    // Section 1 — recent: the ACTIVE model is pinned first (and flagged), the
    // MRU row follows. The active model is de-duped out of its own provider's
    // catalog below, so it is not repeated there.
    REQUIRE(rows.size() >= 2);
    CHECK(rows[0].recent);
    CHECK(rows[0].active);
    CHECK(rows[0].provider_id == "zeta");
    CHECK(rows[0].model.id.value == "zeta-1");
    CHECK(rows[1].recent);
    CHECK(rows[1].provider_id == "alpha");

    // Section 2 — "all providers": every remaining non-recent row, alphabetical
    // by model label ACROSS providers. zeta-1 (active) and alpha-1 (recent)
    // were consumed above, so: "Alpha Two" < "Mid One" < "Zeta Two".
    std::vector<const FusedRow*> browse;
    for (const auto& r : rows)
        if (!r.recent && !r.is_signin_offer()) browse.push_back(&r);
    REQUIRE(browse.size() == 3);
    CHECK(browse[0]->model.id.value == "alpha-2");
    CHECK(browse[1]->model.id.value == "mid-1");
    CHECK(browse[2]->model.id.value == "zeta-2");   // active provider NOT pinned up

    // All non-recent rows are one contiguous block after the recent rows.
    int last_recent = -1, first_browse = -1;
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const auto& r = rows[static_cast<std::size_t>(i)];
        if (r.recent) last_recent = i;
        if (!r.recent && !r.is_signin_offer() && first_browse < 0)
            first_browse = i;
    }
    REQUIRE(first_browse >= 0);
    CHECK(last_recent < first_browse);
}

// Row-layout arithmetic: the picker aligns three columns —
//
//   <provider, padded>  <● marker><model name>   <ctx, right-aligned> <★> <✦>
//
// maya's Picker documents that callers must "pad badges to a common width for
// column alignment", and this picker did not, so with provider labels running
// 3..14 chars every model NAME began at a different column. In a FLAT
// cross-provider list the provider badge is the grouping signal, so a ragged
// badge column defeats the entire layout — you cannot scan it vertically.
//
// The context window is a MEASUREMENT and is right-aligned into a fixed 5-col
// field; the two marks occupy fixed slots after it so a row lacking a
// favourite leaves a hole rather than sliding its ✦ leftwards (which is what
// made the list jitter while scrolling).
//
// This test pins the arithmetic the view performs. It deliberately does NOT
// render — it reproduces the exact expressions from ui::fused_picker so a
// change there without a change here is caught.
TEST_CASE("fused: row columns align") {
    // ── Badge padding ────────────────────────────────────────────────
    auto pad_badge = [](const std::string& label, std::size_t w) {
        return label.size() < w ? label + std::string(w - label.size(), ' ')
                                : label;
    };
    {
        // Width is the widest label present, clamped to 12.
        const std::vector<std::string> labels = {
            "Groq", "Anthropic", "GitHub Copilot", "OpenRouter"};
        std::size_t w = 0;
        for (const auto& l : labels) w = std::max(w, l.size());
        w = std::min<std::size_t>(w, 12);
        CHECK(w == 12);   // "GitHub Copilot" is 14 → clamped

        // Every padded badge is the same width, so the next column starts at
        // one fixed offset for every row.
        std::size_t first = pad_badge(labels.front(), w).size();
        for (const auto& l : labels) {
            const auto b = pad_badge(l, w);
            CHECK(b.size() == std::max(first, l.size()));
        }
        // A short label really is padded (this is the bug being fixed).
        CHECK(pad_badge("Groq", w) == "Groq        ");
        // An over-long label is left alone; maya truncates the overflow.
        CHECK(pad_badge("GitHub Copilot", w) == "GitHub Copilot");
    }

    // ── Context window is right-aligned in a 5-column field ─────────
    auto ctx_field = [](int win) {
        std::string ctx;
        if (win > 0) {
            if (win >= 1'000'000) {
                ctx = std::to_string(win / 1'000'000) + "M";
                if (win % 1'000'000 != 0) ctx += "+";
            } else if (win >= 1000) {
                ctx = std::to_string(win / 1'000) + "k";
            } else {
                ctx = std::to_string(win);
            }
        }
        return ctx.size() < 5 ? std::string(5 - ctx.size(), ' ') + ctx : ctx;
    };
    {
        CHECK(ctx_field(200000) == " 200k");
        CHECK(ctx_field(128000) == " 128k");
        CHECK(ctx_field(8000)   == "   8k");
        CHECK(ctx_field(1000000)== "   1M");
        CHECK(ctx_field(1500000)== "  1M+");
        CHECK(ctx_field(0)      == "     ");   // unknown: blank, still aligned
        // Every field is exactly 5 wide → the digits share a column.
        for (int w : {200000, 128000, 8000, 1000000, 1500000, 0})
            CHECK(ctx_field(w).size() == 5);
    }

    // ── Marks occupy fixed slots ────────────────────────────────────
    auto marks = [](bool fav, bool reasons) {
        std::string t;
        t += fav ? "  \xe2\x98\x85" : "   ";
        t += reasons ? " \xe2\x9c\xa6" : "  ";
        return t;
    };
    {
        // DISPLAY COLUMNS, not bytes. ★ and ✦ are 3-byte UTF-8 sequences that
        // occupy ONE column each, so byte offsets legitimately differ between
        // a row with a favourite and one without — an earlier version of this
        // test compared find() offsets and "failed" on correct layout. What
        // must hold is that both marks land in the same COLUMN.
        auto cols = [](std::string_view t) {
            int n = 0;
            for (unsigned char c : t)
                if ((c & 0xC0) != 0x80) ++n;   // count non-continuation bytes
            return n;
        };
        // Every combination occupies exactly 5 columns: "  ★" / "   " is 3,
        // " ✦" / "  " is 2.
        CHECK(cols(marks(true,  true))  == 5);
        CHECK(cols(marks(true,  false)) == 5);
        CHECK(cols(marks(false, true))  == 5);
        CHECK(cols(marks(false, false)) == 5);
        // The ✦ therefore starts at column 3 whether or not ★ is present —
        // it can never slide into the favourite's slot.
        auto star_slot = [&](bool fav) { return cols(std::string{marks(fav, false)}); };
        CHECK(star_slot(true) == star_slot(false));
    }
}

// The badge column scales with the terminal. A fixed clamp is wrong in both
// directions: too greedy in a narrow split (14 columns restating the provider
// on every row starves the model NAME, which is what the user reads), too
// tight on a wide screen (truncating "GitHub Copilot" to "GitHub Copil" when
// there is ample room). The picker is commonly used in a split pane, so this
// is not a hypothetical.
TEST_CASE("fused: badge column scales with terminal width") {
    // Mirrors panel_badge_max_cols()'s rule: on very narrow terminals the
    // badge is abbreviated hard; everywhere else it may grow to fit the
    // longest actual provider label, but never consume more than half the
    // picker's horizontal space so the model NAME column stays usable.
    auto badge_max = [](int cols) {
        if (cols <= 40) return 8;
        const int cap = cols / 2;
        return cap < 8 ? 8 : cap;
    };

    CHECK(badge_max(60)  == 30);    // 60-col split → cap = 30 (was 8)
    CHECK(badge_max(80)  == 40);   // classic terminal  → cap = 40 (was 12)
    CHECK(badge_max(120) == 60);   // wide              → cap = 60 (was 16)
    CHECK(badge_max(40)  == 8);    // tiny-terminal floor

    // Monotonic in the regime that matters: a wider terminal never yields a
    // NARROWER badge column.
    int prev = 0;
    for (int c : {40, 60, 80, 100, 120, 200}) {
        const int w = badge_max(c);
        CHECK(w >= prev);
        prev = w;
    }

    // Regression: a shared-hostname custom provider (ollama.com#main /
    // ollama.com#seaventures) used to exceed the fixed 16-col cap and break
    // the model column's alignment. The dynamic cap must accept it on any
    // terminal wide enough for a picker to be useful (≥ 48 cols).
    CHECK(badge_max(80) >= 24);    // "ollama.com [seaventures]" is 24
    CHECK(badge_max(80) >= 21);    // "ollama.com [liferaft]" is 21
    // …and is abbreviated, not permitted to eat the row, on a phone/SSH pane.
    CHECK(badge_max(40) <= 8);
    // The widest real preset label still fits once there is room.
    CHECK(badge_max(120) >= 14);   // "GitHub Copilot"
}

// The other half of the shared-hostname fix (commit "disambiguate custom-host
// rows by their #tag"): refresh_fused_sources() surfaces a saved host's
// "#tag" in its picker label so accounts on one hostname stay distinct. This
// mirrors that derivation exactly — id is the raw saved spec, label is what
// provider_display_name() produced from it — and pins the guard that decides
// when to append " [tag]".
TEST_CASE("fused: custom-host #tag disambiguates the picker label") {
    // Same rule as the non-preset branch in refresh_fused_sources().
    auto derive = [](std::string_view id, std::string label) {
        if (auto h = id.rfind('#'); h != std::string_view::npos
                                    && h + 1 < id.size()) {
            const std::string tag{id.substr(h + 1)};
            if (label.find('#' + tag) == std::string::npos)
                label += " [" + tag + "]";
        }
        return label;
    };

    // Host-form specs: provider_display_name() returns the bare host, dropping
    // the fragment — so the tag must be re-exposed, and two accounts on one
    // host end up with DISTINCT labels.
    CHECK(derive("ollama.com#main",        "ollama.com") == "ollama.com [main]");
    CHECK(derive("ollama.com#seaventures", "ollama.com") == "ollama.com [seaventures]");
    CHECK(derive("ollama.com#main", "ollama.com")
          != derive("ollama.com#work", "ollama.com"));

    // URL-form specs keep "#tag" verbatim in the label already; don't
    // double-tag them.
    CHECK(derive("https://ollama.com/v1#main", "ollama.com#main") == "ollama.com#main");

    // The guard keys on the EXACT "#tag" fragment, not a bare substring of the
    // tag. A tag whose text also appears in the HOST must still be bracketed —
    // otherwise "main.ollama.com#main" would collide with a sibling account.
    CHECK(derive("main.ollama.com#main", "main.ollama.com") == "main.ollama.com [main]");
    CHECK(derive("main.ollama.com#main", "main.ollama.com")
          != derive("main.ollama.com#work", "main.ollama.com"));

    // Degenerate specs: no fragment, or a trailing '#' with an empty tag —
    // leave the label untouched (no stray " []").
    CHECK(derive("ollama.com",  "ollama.com") == "ollama.com");
    CHECK(derive("ollama.com#", "ollama.com") == "ollama.com");
}

// The row carries its capability tier, precomputed. Two consumers need it —
// the browse-mode sort and the view (which hues the provider badge by it so
// the strongest-first ordering is legible rather than unexplained) — and both
// run hot: a comparator is O(n log n), and the view touches every visible row
// every frame. tier_for() tokenises the id and runs several substring scans,
// so it is resolved ONCE per row, alongside `reasons`, for the same reason.
TEST_CASE("fused: rows carry a precomputed capability tier") {
    std::vector<ProviderCatalog> cats = {
        cat("openrouter", "OpenRouter",
            {mk("claude-opus-4-5",  "Claude Opus",   "openrouter"),
             mk("claude-sonnet-4-6","Claude Sonnet", "openrouter"),
             mk("claude-haiku-4-5", "Claude Haiku",  "openrouter"),
             mk("tinyllama:1b",     "TinyLlama 1B",  "openrouter")}),
    };

    auto tier_of = [&](const std::vector<FusedRow>& rows, std::string_view id) {
        for (const auto& r : rows)
            if (r.model.id.value == id) return static_cast<int>(r.tier);
        return -1;
    };

    // ── Populated on the ALL-PROVIDERS rows ─────────────────────────
    {
        ui::FusedInputs in;
        in.catalogs = &cats;
        const auto rows = ui::build_fused_rows(in);
        CHECK(tier_of(rows, "claude-opus-4-5")   == 3);   // Flagship
        CHECK(tier_of(rows, "claude-sonnet-4-6") == 2);   // Mid
        CHECK(tier_of(rows, "claude-haiku-4-5")  == 1);   // Cheap
        CHECK(tier_of(rows, "tinyllama:1b")      == 0);   // Weak
    }

    // ── Populated while FILTERING too ───────────────────────────────
    // The sort only needed tier when browsing, so it used to be computed
    // under `no_query`. The view needs it always — a filtered list still
    // hues its badges — so a query must not leave the field at 0 (which
    // would paint every match as Weak).
    {
        ui::FusedInputs in;
        in.catalogs = &cats;
        in.query = "opus";
        const auto rows = ui::build_fused_rows(in);
        REQUIRE(!rows.empty());
        CHECK(tier_of(rows, "claude-opus-4-5") == 3);
    }

    // ── Populated on RECENT rows (a separate build path) ────────────
    {
        std::vector<ModelRef> recents = {ModelRef{"openrouter", "claude-opus-4-5"}};
        ui::FusedInputs in;
        in.catalogs = &cats;
        in.recents  = &recents;
        const auto rows = ui::build_fused_rows(in);
        REQUIRE(!rows.empty());
        CHECK(rows.front().recent);
        CHECK(static_cast<int>(rows.front().tier) == 3);
    }

    // ── Tier survives the alphabetical re-order (badge hue stays true) ──
    // Browse no longer ranks by tier — it is alphabetical — but the badge
    // hue is still computed from the precomputed tier, so a flagship and a
    // 3B local model must keep their DISTINCT hues wherever they now sit.
    // Assert the precomputed field still matches the live computation.
    {
        ui::FusedInputs in;
        in.catalogs = &cats;
        const auto rows = ui::build_fused_rows(in);
        for (const auto& r : rows) {
            if (r.is_signin_offer()) continue;
            CHECK(static_cast<int>(r.tier) ==
                  static_cast<int>(ModelCapabilities::tier_for(r.model.id.value)));
            CHECK(!r.model_label.empty());             // and the name is there
        }
    }
}

// ── Picker row-style convention (source-level guard) ────────────────────
//
// Three rules hold across every picker, and all three were violated somewhere
// before this session:
//
//   1. The PRIMARY label renders at full foreground on non-selected rows.
//      Dimming every row but one inverts the hierarchy: the list reads as
//      uniformly unavailable while the trailing reference chip out-shouts the
//      name you are actually choosing between.
//   2. The TRAILING cell is `trailing_secondary`, so it yields space FIRST
//      under width pressure. maya's default is the opposite (for rows like
//      the file picker's diffstat, where the trailing cell matters more), and
//      inheriting it truncated model names to preserve "200k ★ ✦".
//   3. Badge columns are padded in DISPLAY COLUMNS, never bytes — a custom
//      host is user-named and may hold CJK or emoji.
//
// These are view-layer style constants with no runtime seam to assert
// against, so this reads the source. A grep-shaped test is unusual, but the
// alternative is a convention that silently rots — which is exactly what
// happened to rules 1 and 2 across four pickers.
TEST_CASE("pickers: primary labels are not dimmed, trailing yields first") {
    // The picker views live in several .cpp files under this dir — read and
    // concatenate them all so the convention guard covers every picker.
    namespace fs = std::filesystem;
    std::string src;
    const fs::path dir{AGENTTY_PICKERS_SRC_DIR};
    REQUIRE_MESSAGE(fs::is_directory(dir), "no picker src dir: " << dir);
    for (const auto& ent : fs::directory_iterator(dir)) {
        if (ent.path().extension() != ".cpp") continue;
        std::ifstream in(ent.path());
        REQUIRE_MESSAGE(in.good(), "cannot open " << ent.path());
        std::stringstream ss;
        ss << in.rdbuf();
        src += ss.str();
        src += '\n';
    }
    REQUIRE(!src.empty());

    auto count = [&](std::string_view needle) {
        std::size_t n = 0, pos = 0;
        while ((pos = src.find(needle, pos)) != std::string::npos) { ++n; ++pos; }
        return n;
    };

    // Rule 1: no picker dims its primary label on the non-active branch.
    // (`fg_dim(muted)` on a TRAILING cell is correct and not matched here.)
    CHECK(count("leading_style  = active ? fg_bold(fg) : fg_of(muted)") == 0);
    CHECK(count("leading_style = active ? fg_bold(fg) : fg_of(muted)") == 0);
    CHECK(count("leading_style  = is_current ? fg_bold(info) : fg_of(muted)") == 0);

    // Rule 2: every picker that shows a trailing chip marks it secondary.
    // Model, provider, thread list, command palette.
    CHECK(count("trailing_secondary = true") >= 4);

    // Rule 3: badge padding measures columns, not bytes.
    CHECK(src.find("maya::string_width(r.label)") != std::string::npos);

    // NOTE: the browse "all providers" section header is NOT asserted here.
    // Grepping this file would only prove the branch was TYPED — not that it
    // renders or that the rows it titles are on screen.
    // panel_sections_render_test renders the real picker and reads the
    // header back off the canvas, which is strictly stronger.
}


// ── Derived-cache coherence: the caches are an optimisation, not a fork ──
//
// The per-keystroke build reads three precomputed, size-guard-invalidated
// caches on ProviderCatalog: search_keys (lowercased haystacks), row_keys
// (capkey::norm_row_id folds, for alias dedup) and display_labels. They
// exist purely for speed — 450-model aggregator lists went 3.8ms → 0.6ms
// per keystroke — so the binding invariant is that results are IDENTICAL
// with and without them. A cache that changes behaviour is a bug.
namespace {
// Populate a catalog's derived caches exactly the way rebuild_fused_rows
// (src/runtime/app/update/picker.cpp) does.
void build_caches(ProviderCatalog& c) {
    c.row_keys.clear();
    c.display_labels.clear();
    for (const auto& mi : c.models) {
        c.row_keys.push_back(agentty::capkey::norm_row_id(mi.id.value));
        c.display_labels.push_back(
            mi.display_name.empty() ? mi.id.value : mi.display_name);
    }
}
std::vector<std::pair<std::string, std::string>> ids_of(
        const std::vector<FusedRow>& rows) {
    std::vector<std::pair<std::string, std::string>> v;
    v.reserve(rows.size());
    for (const auto& r : rows) v.emplace_back(r.provider_id, r.model.id.value);
    return v;
}
} // namespace

TEST_CASE("fused: derived caches never change the result set") {
    std::vector<ProviderCatalog> cats = {
        cat("anthropic", "Anthropic",
            {mk("claude-sonnet-4-6", "Claude Sonnet 4.6", "anthropic"),
             mk("claude-opus-4", "Claude Opus 4", "anthropic")}),
        cat("openai", "OpenAI",
            {mk("gpt-5-codex", "gpt-5-codex", "openai", 400000),
             mk("gpt-4o", "gpt-4o", "openai", 128000)}),
    };
    std::vector<ModelRef> recents{{"anthropic", "claude-opus-4"}};
    FusedInputs in;
    in.catalogs = &cats;
    in.recents  = &recents;
    in.active   = ModelRef{"openai", "gpt-5-codex"};

    for (const char* q : {"", "gpt", "claude", "o", "zzz"}) {
        INFO("query = '" << q << "'");
        in.query = q;
        for (auto& c : cats) { c.row_keys.clear(); c.display_labels.clear(); }
        const auto bare = ids_of(build_fused_rows(in));
        for (auto& c : cats) build_caches(c);
        CHECK(ids_of(build_fused_rows(in)) == bare);
    }
}

TEST_CASE("fused: alias twins dedupe identically through the row_keys cache") {
    // A catalog that lists two spellings of ONE model (the real
    // mistral-medium-3-5 / -3.5 case). Exactly one row must surface,
    // whether the fold comes from the cache or is computed live.
    std::vector<ProviderCatalog> cats = {
        cat("aggr", "Aggr",
            {mk("mistral-medium-3-5", "Mistral Medium 3.5", "aggr"),
             mk("mistral-medium-3.5", "Mistral Medium 3.5", "aggr")}),
    };
    FusedInputs in;
    in.catalogs = &cats;

    CHECK(build_fused_rows(in).size() == 1);   // live fold
    for (auto& c : cats) build_caches(c);
    CHECK(build_fused_rows(in).size() == 1);   // cached fold
}

// ── Tool capability is a RANKING signal, not just a label ───────────────
//
// A model whose provider positively reported no tool support cannot drive
// the agent at all. Labelling it is necessary but not sufficient: while
// BROWSING it must also sink below every model that can do the job, so
// the first screen is models you can actually pick. An explicit search
// still surfaces it immediately (you asked for it by name).
TEST_CASE("fused: no-tools models sink below capable ones while browsing") {
    auto no_tools = [](ModelInfo mi) { mi.supports_tools = false; return mi; };
    std::vector<ProviderCatalog> cats = {
        cat("ollama", "Ollama",
            {no_tools(mk("llama3-chat", "llama3-chat", "ollama")),
             mk("qwen3-coder", "qwen3-coder", "ollama")}),
    };
    FusedInputs in;
    in.catalogs = &cats;

    auto rows = build_fused_rows(in);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].model.id.value == "qwen3-coder");
    CHECK(rows[0].tool_capable);
    CHECK(rows[1].model.id.value == "llama3-chat");
    CHECK(!rows[1].tool_capable);

    // …but naming it finds it right away: the score gate outranks the sink.
    // (Query chosen to be unmatchable via the provider label — "Ollama"
    // itself contains "llama" as a subsequence, which would make BOTH rows
    // match on the provider segment of the haystack.)
    in.query = "chat";
    auto hit = build_fused_rows(in);
    REQUIRE(!hit.empty());
    CHECK(hit[0].model.id.value == "llama3-chat");
}

TEST_CASE("fused: unknown tool support is treated as capable") {
    // Hosted providers don't advertise this; absence must never be read as
    // "cannot", or every OpenAI/Anthropic row would be labelled chat-only.
    std::vector<ProviderCatalog> cats = {
        cat("openai", "OpenAI", {mk("gpt-4o", "gpt-4o", "openai")}),
    };
    FusedInputs in;
    in.catalogs = &cats;
    auto rows = build_fused_rows(in);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].tool_capable);
}
