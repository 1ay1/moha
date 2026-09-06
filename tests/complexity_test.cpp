// complexity_test — the turn-complexity classifier (smart::classify_complexity).
// Pure heuristic, no I/O. Locks the conservative calibration: Standard is the
// fallback, only strong signals move a turn to Complex or Trivial.

#include "agentty/domain/complexity.hpp"

#include "agtest.hpp"

using namespace agentty::smart;

TEST_CASE("classify_complexity buckets prompts") {
    // Trivial: short acknowledgements / one-word imperatives, no question.
    CHECK(classify_complexity("yes") == Complexity::Trivial, "yes → trivial");
    CHECK(classify_complexity("go ahead") == Complexity::Trivial, "go ahead → trivial");
    CHECK(classify_complexity("commit it") == Complexity::Trivial, "commit it → trivial");
    CHECK(classify_complexity("  thanks  ") == Complexity::Trivial, "trimmed thanks → trivial");
    CHECK(classify_complexity("") == Complexity::Trivial, "empty → trivial");

    // Complex: design/debug/why vocabulary, long turns, or many enumerated asks.
    CHECK(classify_complexity("redesign the auth module") == Complexity::Complex,
          "redesign → complex");
    CHECK(classify_complexity("why does the retry loop deadlock?") == Complexity::Complex,
          "why+deadlock → complex");
    CHECK(classify_complexity("do a deep research on this and implement all state of the art way")
          == Complexity::Complex, "deep research + implement all → complex");
    CHECK(classify_complexity(
            "1. add a field\n2. persist it\n3. render it\n4. test it") == Complexity::Complex,
          "4 enumerated asks → complex");

    // Simple: a short single-clause request (no design vocab, one line).
    CHECK(classify_complexity("fix the typo in README") == Complexity::Simple,
          "short fix → simple");
    CHECK(classify_complexity("rename foo to bar") == Complexity::Simple,
          "short rename → simple");

    // Standard: the conservative default for medium, ambiguous turns.
    CHECK(classify_complexity(
            "update the parser to also accept trailing commas in list literals")
          == Complexity::Standard, "medium request → standard");

    // Conservative bias: an ambiguous 'why' still escalates (never under-thinks).
    CHECK(classify_complexity("explain why this test is flaky") == Complexity::Complex,
          "why → escalates (conservative upward)");

    // ── Context inheritance (classify_with_context): a short follow-up to a
    //    Complex turn keeps some weight instead of collapsing to Simple.
    CHECK(classify_with_context("now do the same for the other module",
                                Complexity::Complex) == Complexity::Standard,
          "short follow-up after Complex → lifted to Standard");
    // A fresh Trivial ack is always taken at face value regardless of history.
    CHECK(classify_with_context("thanks", Complexity::Complex) == Complexity::Trivial,
          "ack after Complex → still Trivial (an ack is an ack)");
    // A fresh Complex signal in the text wins outright.
    CHECK(classify_with_context("redesign the whole thing", Complexity::Simple)
          == Complexity::Complex, "fresh Complex text wins over Simple history");
    // Never DROPS below the text's own tier when history is weaker.
    CHECK(classify_with_context("fix the typo", Complexity::Trivial) == Complexity::Simple,
          "weaker history never lowers the text's own tier");
    // Standard history + Simple text stays Simple (only Complex lifts).
    CHECK(classify_with_context("rename foo to bar", Complexity::Standard)
          == Complexity::Simple, "Standard history does not lift a Simple follow-up");

    // ── Script-agnostic size floor: a LONG non-space-delimited (CJK) request
    //    must escalate on structure/length, not collapse to Simple just because
    //    word_count sees ~1 whitespace token. ~450 glyphs of CJK + clause
    //    markers clears the Complex band via the glyph-length + clause signals.
    {
        std::string cjk;
        for (int i = 0; i < 150; ++i) cjk += "\xE6\x8E\xA2\xE7\xB4\xA2\xE3\x80\x81";  // 2 glyphs + 、
        CHECK(classify_complexity(cjk) == Complexity::Complex,
              "long structured CJK turn escalates (glyph length + clause density)");
    }
    // Empty / whitespace-only input is Trivial, never a crash.
    CHECK(classify_complexity("") == Complexity::Trivial, "empty → trivial");
    CHECK(classify_complexity("   \n\t ") == Complexity::Trivial, "whitespace → trivial");

    // ── Additive model beats the old lookup table ──
    // A single "design" no longer HARD-forces Complex on a short, concrete UI
    // ask (the old `has(kComplexTerms)` override did exactly that).
    CHECK(classify_complexity("add a design token for button padding")
          != Complexity::Complex,
          "one keyword doesn't override structure on a short concrete ask");
    // But structure + vocabulary TOGETHER still escalate.
    CHECK(classify_complexity(
            "redesign the auth module: rework sessions, refresh tokens, and "
            "the login flow end to end") == Complexity::Complex,
          "vocabulary + multi-clause structure → complex");
    // Multi-clause conjunction density escalates WITHOUT any hard keyword.
    CHECK(classify_complexity(
            "update the parser and the lexer and the formatter and re-run the "
            "golden tests then wire it into the build")
          == Complexity::Complex, "pure conjunction/clause density → complex");
    // The scored API exposes a usable margin (confident on a clear ack).
    CHECK(classify_score("thanks").margin > 0, "trivial ack has positive margin");
    CHECK(classify_score("redesign everything end to end across the stack").tier
          == Complexity::Complex, "scored API agrees with tier API");

    // ── Tunable Complex threshold ──────────────────────────────
    // The cut is a PARAMETER now, not a getenv() inside the classifier. It has
    // to be: the value is user-configurable (settings row
    // "smart.complex_threshold"), and a read from the environment down here
    // could not see a threshold set in the UI. The runtime resolves
    // env-over-stored once at startup into RoleConfig and passes it in.
    //
    // So this asserts the CLASSIFIER's contract — that the threshold moves the
    // boundary — directly, without a process-global to set and unset.
    {
        const char* probe = "update the parser and re-run the formatter";
        const Complexity base = classify_complexity(probe);
        CHECK(base != Complexity::Complex,
              "the probe is below the Complex cut at the shipped default");

        // Lowering the cut escalates a borderline turn...
        CHECK(classify_score(probe, /*complex_min=*/1).tier == Complexity::Complex,
              "lowering the threshold escalates a borderline turn");
        // ...and the default parameter reproduces the shipped behaviour.
        CHECK(classify_score(probe).tier == base,
              "the default parameter is the shipped classification");
        // Raising it far past the probe's score cannot leave it Complex.
        CHECK(classify_score(probe, /*complex_min=*/8).tier != Complexity::Complex,
              "raising the threshold de-escalates");
    }

    // ── is_routing_correction: the outcome-learning regret signal ─────────
    // TRUE = a dissatisfaction follow-up that should bias the turn-class's
    // effort up. The false-positive cases are the whole point of the fix.
    {
        using agentty::smart::is_routing_correction;
        // Genuine regrets.
        CHECK(is_routing_correction("no, that doesn't work"), "'no, doesn't work' is a regret");
        CHECK(is_routing_correction("that's wrong"), "'that's wrong' is a regret");
        CHECK(is_routing_correction("undo that"), "'undo' is a regret");
        CHECK(is_routing_correction("revert it"), "'revert' is a regret");
        CHECK(is_routing_correction("still failing after your change"), "'still failing' is a regret");
        CHECK(is_routing_correction("actually that's not right"), "'actually not right' is a regret");
        CHECK(is_routing_correction("nope, still broken"), "'nope still broken' is a regret");
        CHECK(is_routing_correction("that broke the build"), "'that broke' is a regret");

        // False positives the naive prefix scan used to hit — must be FALSE.
        CHECK(!is_routing_correction("actually, let's also add tests"),
              "additive 'actually' is NOT a regret");
        CHECK(!is_routing_correction("actually that's perfect"),
              "praise 'actually that's perfect' is NOT a regret");
        CHECK(!is_routing_correction("wrong file, look at src/foo instead"),
              "redirection 'wrong file' is NOT a regret");
        CHECK(!is_routing_correction("no worries, carry on"),
              "'no worries' is NOT a regret");
        CHECK(!is_routing_correction("now add error handling"),
              "a plain new request is NOT a regret");
        CHECK(!is_routing_correction("thanks, that works"),
              "gratitude is NOT a regret");
        CHECK(!is_routing_correction("looks good, ship it"),
              "approval is NOT a regret");
    }
}
