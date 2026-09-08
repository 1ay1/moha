// md_robustness_test — the markdown renderer must never crash, never
// silently drop content, and never mangle a paragraph, no matter what the
// model emits or how narrow the surface is.
//
// Both halves were real, shipped bugs:
//   • Canvas(-1, …) cast a negative dimension to std::size_t and threw
//     bad_alloc from inside a render — a hard crash reachable from any
//     caller that computed a width by subtraction (a nested widget whose
//     chrome exceeded its parent, a terminal reporting 0 columns during a
//     resize race).
//   • The markdown widget's unconditional 2-column indent consumed the
//     WHOLE content area at width <= 2, so the document rendered as
//     nothing at all — silent data loss, the worst failure mode a
//     renderer has.
//
// Model output is adversarial by nature (pseudo-tags like <shell>,
// unbalanced angle brackets, half-streamed tags), so those shapes are
// swept here too: every one must survive at every width.

#include "agtest.hpp"

#include <maya/app/inline.hpp>
#include <maya/widget/markdown.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

[[nodiscard]] std::string render_md(const std::string& src, int width,
                                    bool live = false) {
    auto w = std::make_shared<maya::StreamingMarkdown>();
    w->set_live(live);
    w->set_reveal_fx(false);
    w->set_reveal_decorate(false);
    w->append(src);
    if (!live) w->finish();
    return maya::render_to_string(w->build(), width);
}

// Visible (non-space) characters — the signal for "did the content survive".
[[nodiscard]] std::size_t visible_chars(std::string_view s) {
    std::size_t n = 0;
    for (char c : s)
        if (c != ' ' && c != '\n' && c != '\r' && c != '\t') ++n;
    return n;
}

// The shapes a chat model actually emits that have broken this renderer.
const std::vector<std::string>& adversarial_docs() {
    static const std::vector<std::string> docs = {
        "plain prose with no markup at all, just words that wrap",
        "Yes - `^U` is the field clear chord (`Intent::ClearField`), but "
        "you're describing `^K` clearing the whole buffer.",
        // Pseudo-tags: not HTML, must render as text and must NOT open a
        // raw-HTML block that swallows the markdown beneath them.
        "<shell>\nUse POSIX tools; `uname -a` gives the kernel.",
        "The prompt has a <shell> stanza and a <thinking> block inline.",
        "<tool_call name=\"x\">\nprose after the pseudo-tag",
        "<system-reminder>\n- a list item\n- another item",
        // Unbalanced / bare brackets — never a tag, must stay literal.
        "Compare `a < b` and `c > d` in one paragraph.",
        "an unclosed <tag that never ends on this line",
        "a lone < and a lone > and a <> empty pair",
        // Real HTML mixed with prose.
        "<div>\nreal html block\n</div>\n\nprose after",
        "inline <b>bold</b> and <i>italic</i> tags",
        // Structure that must survive narrow widths.
        "# Heading\n\n- bullet one\n- bullet two\n\n```sh\ncode line\n```",
        "| a | b |\n|---|---|\n| 1 | 2 |",
        "> a block quote that is long enough to need wrapping somewhere",
    };
    return docs;
}

} // namespace

TEST_CASE("markdown: never crashes at any width, including degenerate ones") {
    // Widths a caller can genuinely produce: a real terminal, a squeezed
    // nested container, a resize race reporting 0, and a subtraction that
    // went negative. None may throw or abort.
    for (const auto& doc : adversarial_docs()) {
        for (int w : {200, 80, 40, 20, 10, 5, 3, 2, 1, 0, -1, -80}) {
            const std::string out = render_md(doc, w);
            // The contract at degenerate widths is "renders nothing",
            // never "throws". Reaching this line at all is the assertion.
            CHECK(out.size() < 1u << 22);   // and never explodes in size
        }
    }
}

TEST_CASE("markdown: content is never silently dropped on a narrow surface") {
    // The regression: a 2-column decorative indent ate the whole content
    // area, so width <= 2 rendered EMPTY. Any width that can hold at least
    // one character must show characters.
    for (const auto& doc : adversarial_docs()) {
        for (int w : {40, 10, 5, 3, 2, 1}) {
            const std::string out = render_md(doc, w);
            check(visible_chars(out) > 0,
                  "width " + std::to_string(w) + " must render SOME content");
        }
    }
}

TEST_CASE("markdown: a pseudo-tag does not swallow the markdown after it") {
    // <shell> is not HTML. Under strict CommonMark kind-7 it would open a
    // raw HTML block that eats everything to the next blank line, taking
    // the list with it. maya deliberately diverges: unknown tags are text.
    const std::string doc =
        "<shell>\n- first bullet\n- second bullet\n\nclosing prose";
    const std::string out = render_md(doc, 80);
    check(out.find("first bullet")  != std::string::npos,
          "list under a pseudo-tag survives");
    check(out.find("second bullet") != std::string::npos,
          "the whole list survives, not just its head");
    check(out.find("closing prose") != std::string::npos,
          "prose after the pseudo-tag survives");
    check(out.find("<shell>") != std::string::npos,
          "the pseudo-tag itself renders as literal text");
}

// ── In-body (inline) tags ──────────────────────────────────────────
// The BLOCK path gates raw-HTML blocks on html::is_known_tag, so a line
// starting with <shell> stays prose. Inline tags take a different route
// (cm_inline's scan_html_tag, which has no such gate), and there a tag
// that merely LOOKS like HTML used to be consumed as markup: a styling
// tag emits no literal text, so "Set <var> to 3" rendered "Set  to 3".
// Only names colliding with real HTML elements were affected, which is
// exactly the set a model hits when it talks about markup or echoes
// agentty's own prompt tags (<environment>, <output>, <summary>, <dir>).
// Contract: a tag is silent only when it is BALANCED.
TEST_CASE("markdown: an unbalanced in-body tag is never swallowed") {
    // Names that collide with real HTML elements — the regression set.
    for (const char* name : {"var", "cite", "code", "kbd", "q", "div", "p",
                             "span", "font", "summary", "dir", "output",
                             "data", "object", "b", "i", "em", "mark"}) {
        const std::string doc =
            std::string("Set <") + name + "> to three.";
        const std::string out = render_md(doc, 70);
        INFO("tag <", name, "> rendered: ", out);
        CHECK_MESSAGE(out.find(std::string("<") + name + ">") != std::string::npos,
                      "a lone opening tag must survive as literal text");
        CHECK_MESSAGE(out.find("three") != std::string::npos,
                      "surrounding prose must survive");

        // A stray CLOSE tag belongs to no scope and must not delete itself.
        const std::string cdoc =
            std::string("Set </") + name + "> to three.";
        const std::string cout_ = render_md(cdoc, 70);
        INFO("close </", name, "> rendered: ", cout_);
        CHECK(cout_.find(std::string("</") + name + ">") != std::string::npos);
    }

    // Pseudo-tags with no HTML twin were already fine; lock them in too.
    for (const char* name : {"shell", "thinking", "environment", "cwd",
                             "tool_call", "shell-notes", "memory-tools"}) {
        const std::string doc = std::string("Use the <") + name + "> tool.";
        const std::string out = render_md(doc, 70);
        INFO("pseudo <", name, "> rendered: ", out);
        CHECK(out.find(std::string("<") + name + ">") != std::string::npos);
    }
}

TEST_CASE("markdown: balanced in-body HTML still styles rather than printing") {
    // The other half of the contract — the fix must not turn real inline
    // HTML into literal tag soup. A matched pair is consumed as markup and
    // its CONTENT survives.
    struct Case { const char* doc; const char* keep; };
    for (auto c : {
            Case{"Set <b>bold</b> here.", "bold"},
            Case{"Set <em>emph</em> here.", "emph"},
            Case{"Set <var>x</var> here.", "x"},
            Case{"A <span style=\"color:red\">red</span> word.", "red"},
            Case{"A <cite>source</cite> here.", "source"},
         }) {
        const std::string out = render_md(c.doc, 70);
        INFO("doc: ", c.doc, " rendered: ", out);
        CHECK_MESSAGE(out.find(c.keep) != std::string::npos,
                      "content between a matched pair must render");
        CHECK_MESSAGE(out.find("</") == std::string::npos,
                      "a balanced pair must not print its own closing tag");
    }
}

TEST_CASE("markdown: an unclosed inline tag does not bleed across blocks") {
    // An unbalanced tag now stays literal, so it must also not leave a
    // style scope open that colours later paragraphs.
    const std::string doc =
        "Para one has an unclosed <b>bold tag.\n\n"
        "Para two is plain.\n\n"
        "## Heading\n";
    const std::string out = render_md(doc, 70);
    CHECK(out.find("Para two is plain.") != std::string::npos);
    CHECK(out.find("<b>") != std::string::npos);
    CHECK(out.find("Heading") != std::string::npos);
}

TEST_CASE("markdown: in-body tags stream to the same result as one-shot") {
    for (const char* doc : {"Use the <shell> tool now.",
                            "Mixed <shell> and <b>bold</b> here.",
                            "Set <var> to 3.",
                            "A <div> mid-sentence."}) {
        auto w = std::make_shared<maya::StreamingMarkdown>();
        w->set_live(true);
        w->set_reveal_fx(false);
        w->set_reveal_decorate(false);
        for (char ch : std::string(doc)) w->append(std::string(1, ch));
        w->finish();
        const std::string streamed = maya::render_to_string(w->build(), 70);
        const std::string oneshot  = render_md(doc, 70);
        INFO("doc: ", doc, "\n one-shot: ", oneshot, "\n streamed: ", streamed);
        CHECK(visible_chars(streamed) == visible_chars(oneshot));
    }
}

TEST_CASE("markdown: streaming a doc byte-by-byte never loses the settled text") {
    // Tags split ACROSS delta boundaries are the streaming hazard: the live
    // parser sees "<she" then "ll>" and must not latch a different block
    // type than the settled parse. Compare the byte-by-byte live build to
    // the one-shot settled build.
    for (const auto& doc : adversarial_docs()) {
        auto w = std::make_shared<maya::StreamingMarkdown>();
        w->set_live(true);
        w->set_reveal_fx(false);
        w->set_reveal_decorate(false);
        for (char c : doc) {
            w->append(std::string(1, c));
            (void)maya::render_to_string(w->build(), 60);   // must not throw
        }
        w->finish();
        const std::string streamed = maya::render_to_string(w->build(), 60);
        const std::string oneshot  = render_md(doc, 60);
        check(visible_chars(streamed) == visible_chars(oneshot),
              "byte-streamed and one-shot renders settle identically");
    }
}
