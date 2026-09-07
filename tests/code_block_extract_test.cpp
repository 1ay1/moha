// code_block_extract_test — closed-vs-open fence extraction for Ctrl+G.
//
// The mid-stream Run feature offers only code blocks whose ``` fence has
// already CLOSED (running a half-typed command is unsafe). This pins
// extract_closed_code_blocks: it must drop a trailing block whose fence is
// still open, keep every closed block, and report `had_open` so the reducer
// can say "still streaming" vs "no blocks".
#include "agtest.hpp"

#include "agentty/runtime/panel/code_blocks.hpp"

#include <string_view>

namespace cbp = agentty::code_blocks;


TEST_CASE("code block extract") {
    std::printf("[code_block_extract]\n");

    // 1. A closed block followed by an OPEN one (still streaming): only the
    //    closed block is offered, and had_open is set.
    {
        std::string_view s =
            "run this:\n```sh\necho one\n```\nnow watch:\n```sh\necho two";
        bool open = false;
        auto b = cbp::extract_closed_code_blocks(s, &open);
        CHECK(b.size() == 1, "one closed block offered (trailing open dropped)");
        CHECK(open, "had_open flags the still-streaming block");
        if (!b.empty())
            CHECK(b[0].body.find("echo one") != std::string::npos,
                  "the closed block is the first one");
    }

    // 2. All fences closed: every block offered, had_open false.
    {
        std::string_view s = "```sh\necho a\n```\n```sh\necho b\n```\n";
        bool open = true;
        auto b = cbp::extract_closed_code_blocks(s, &open);
        CHECK(b.size() == 2, "both closed blocks offered");
        CHECK(!open, "had_open false when nothing is mid-fence");
    }

    // 3. A single still-opening block (no close yet): nothing offered, but
    //    had_open true so the caller says "still streaming".
    {
        std::string_view s = "here you go:\n```bash\nsudo systemctl restart x";
        bool open = false;
        auto b = cbp::extract_closed_code_blocks(s, &open);
        CHECK(b.empty(), "no block offered while its fence is open");
        CHECK(open, "had_open true for the lone mid-fence block");
    }

    // 4. Prose with no fences at all: empty, had_open false.
    {
        std::string_view s = "just some prose, no code here at all.\n";
        bool open = true;
        auto b = cbp::extract_closed_code_blocks(s, &open);
        CHECK(b.empty(), "no blocks in plain prose");
        CHECK(!open, "had_open false with no fences");
    }

    // 5. Tilde fence closes correctly.
    {
        std::string_view s = "~~~sh\necho tilde\n~~~\n";
        bool open = true;
        auto b = cbp::extract_closed_code_blocks(s, &open);
        CHECK(b.size() == 1, "tilde-fenced closed block offered");
        CHECK(!open, "tilde close detected");
    }

    // 6. extract_closed never yields MORE than extract_code_blocks.
    {
        std::string_view s = "```sh\na\n```\n```sh\nb";   // one closed, one open
        auto all    = cbp::extract_code_blocks(s);
        auto closed = cbp::extract_closed_code_blocks(s);
        CHECK(all.size() == 2, "raw extract keeps the open trailing block");
        CHECK(closed.size() == 1, "closed extract drops the open one");
        CHECK(closed.size() < all.size(), "closed <= raw, strictly here");
    }
}
