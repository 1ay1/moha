// panel_sections_render_test — the fused model picker's SECTION HEADERS,
// asserted against real rendered cells.
//
// Browse is a two-section layout ("recent" / "all providers"); there is no
// per-provider split. The header is SPELLED in the view, so this renders the
// picker through the real view + maya and reads the header back off the
// canvas rather than grepping the source — a source-text assertion cannot
// tell you the header is reachable or that the rows it titles are on screen.

#include "agtest.hpp"

#include "agentty/runtime/model.hpp"
#include "agentty/runtime/fused_models.hpp"
#include "agentty/runtime/view/panels.hpp"

#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>

#include <string>
#include <vector>

using namespace agentty;
namespace pn = agentty::ui::panel;

namespace {

ModelInfo mk(std::string id, std::string name, std::string provider) {
    ModelInfo mi;
    mi.id = ModelId{std::move(id)};
    mi.display_name = std::move(name);
    mi.provider = std::move(provider);
    mi.context_window = 200000;
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

// Render ONLY the picker element (not the whole app layout) and flatten the
// canvas to text. Non-ASCII folds to '?' — the section titles are ASCII, and
// the badge/■ chrome around them is not what we're asserting on.
std::string render_panel(const Model& m, int width = 100, int height = 200) {
    auto root = ui::models_panel(m);
    maya::StylePool pool;
    maya::Canvas canvas(width, height, &pool);
    canvas.clear();
    maya::render_tree(root, canvas, pool, maya::theme::dark, true);

    std::string out;
    const int max_row = canvas.max_content_row();
    for (int y = 0; y <= max_row; ++y) {
        std::string line;
        for (int x = 0; x < width; ++x) {
            char32_t ch = canvas.get(x, y).character;
            if (ch == 0) ch = U' ';
            line.push_back(ch < 128 ? static_cast<char>(ch) : '?');
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out += line;
        out.push_back('\n');
    }
    return out;
}

bool has(const std::string& hay, std::string_view needle) {
    return hay.find(needle) != std::string::npos;
}

// Build a Model with the picker open over `cats`, as the reducer would:
// fused_rows is the reducer-maintained cache the view reads, never rebuilt
// per frame, so a render test must populate it the same way.
Model picker_model(const std::vector<ProviderCatalog>& cats,
                   ModelRef active = {}) {
    Model m;
    ui::FusedInputs in;
    in.catalogs = &cats;
    in.active   = active;
    m.d.fused_rows = ui::build_fused_rows(in);
    m.ui.panel = pn::Models{};
    return m;
}

}  // namespace

TEST_CASE("picker: browse lists every provider under one 'all providers' header") {
    // With an active provider set, its non-recent rows are NOT broken out
    // into a "from this provider" block — every non-recent model, regardless
    // of provider, lives under a single "all providers" header.
    const std::vector<ProviderCatalog> cats{
        cat("anthropic", "Anthropic",
            {mk("claude-opus-4-5",  "Claude Opus 4.5",  "anthropic"),
             mk("claude-haiku-4-5", "Claude Haiku 4.5", "anthropic")}),
        cat("openai", "OpenAI", {mk("gpt-5", "GPT-5", "openai")}),
    };
    const Model m = picker_model(cats, ModelRef{"anthropic", "claude-opus-4-5"});
    const std::string screen = render_panel(m);

    INFO(screen);
    // One flat section — no per-provider split.
    CHECK(has(screen, "ALL PROVIDERS"));
    CHECK_FALSE(has(screen, "from this provider"));
    CHECK_FALSE(has(screen, "from all other providers"));
    // Rows from BOTH providers render under it.
    CHECK(has(screen, "Claude Haiku 4.5"));   // active provider, non-recent
    CHECK(has(screen, "GPT-5"));              // another provider
}

TEST_CASE("picker: with NO active provider, the header is still 'all providers'") {
    // The fresh-install shape: nothing active. The header is unchanged —
    // there was never a contrast to drop.
    const std::vector<ProviderCatalog> cats{
        cat("anthropic", "Anthropic",
            {mk("claude-opus-4-5", "Claude Opus 4.5", "anthropic")}),
        cat("openai", "OpenAI", {mk("gpt-5", "GPT-5", "openai")}),
    };
    const Model m = picker_model(cats);           // active left EMPTY
    const std::string screen = render_panel(m);

    INFO(screen);
    CHECK_FALSE(has(screen, "from this provider"));
    CHECK_FALSE(has(screen, "from all other providers"));
    CHECK(has(screen, "ALL PROVIDERS"));
    // The models are still listed under it.
    CHECK(has(screen, "Claude Opus 4.5"));
    CHECK(has(screen, "GPT-5"));
}

TEST_CASE("picker: the active model leads, and its row is on screen") {
    // The ● pin is a builder invariant (fused_models_test proves the
    // ordering); this pins that the picker RENDERS it in the recent section
    // rather than ordering it correctly and then drawing something else.
    const std::vector<ProviderCatalog> cats{
        cat("anthropic", "Anthropic",
            {mk("claude-opus-4-5",  "Claude Opus 4.5",  "anthropic"),
             mk("claude-haiku-4-5", "Claude Haiku 4.5", "anthropic")}),
    };
    const std::vector<ModelRef> recents{
        ModelRef{"anthropic", "claude-haiku-4-5"},
        ModelRef{"anthropic", "claude-opus-4-5"},
    };
    Model m;
    ui::FusedInputs in;
    in.catalogs = &cats;
    in.recents  = &recents;
    in.active   = ModelRef{"anthropic", "claude-opus-4-5"};
    m.d.fused_rows = ui::build_fused_rows(in);
    m.ui.panel = pn::Models{};

    const std::string screen = render_panel(m);
    INFO(screen);
    CHECK(has(screen, "RECENT"));   // ┌─ RECENT divider (^K-style, uppercased)
    // Haiku sorts before Opus alphabetically; the active Opus must still be
    // the first RECENT row, because the picker pre-selects row 0.
    const auto recent_hdr = screen.find("RECENT");
    const auto opus       = screen.find("Claude Opus 4.5");
    const auto haiku      = screen.find("Claude Haiku 4.5");
    REQUIRE(recent_hdr != std::string::npos);
    REQUIRE(opus  != std::string::npos);
    REQUIRE(haiku != std::string::npos);
    CHECK(opus < haiku);
}
