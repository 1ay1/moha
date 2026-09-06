// complexity.cpp — the turn-complexity classifier (see complexity.hpp).
//
// An ADDITIVE FEATURE SCORE, not a keyword lookup. Three orthogonal signal
// families each contribute weight; the sum is thresholded into a tier. This is
// deliberately a tiny linear model with hand-set weights — no ML runtime, pure,
// allocation-light, deterministic, sub-microsecond — but it GENERALISES the way
// a lookup table can't:
//
//   • STRUCTURAL features are language-agnostic (enumerated asks, conjunction
//     and clause density, code-token density, question shape, glyph length).
//     A request's complexity lives mostly in its STRUCTURE, not its verbs, so
//     these carry most of the weight and work in any language.
//   • LEXICAL features are weighted keyword sets spanning the major languages
//     (en/es/fr/de/pt/it + a few CJK markers). Keywords ADD evidence; no single
//     hit is a hard override, so "design system button" no longer forces
//     Complex on the word "design" alone.
//   • MORPHOLOGICAL features capture token-shape variety (prose vs. identifiers
//     vs. paths) — a proxy for "this touches code structure".
//
// Conservative by construction: Standard is the score band around zero, ties
// break upward (under-thinking a hard turn costs more than over-thinking a
// cheap one).

#include "agentty/domain/complexity.hpp"
#include "agentty/domain/smart_tuning.hpp"

#include <array>
#include <cctype>
#include <cstdint>

namespace agentty::smart {

namespace {

// ── Lexing primitives (ASCII-lowercase; UTF-8 continuation bytes pass through
//    untouched, so multibyte scripts survive) ─────────────────────────────────
constexpr char lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}

bool has(std::string_view hay_lower, std::string_view needle) noexcept {
    if (needle.empty() || needle.size() > hay_lower.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= hay_lower.size(); ++i) {
        std::size_t j = 0;
        for (; j < needle.size(); ++j)
            if (hay_lower[i + j] != needle[j]) break;
        if (j == needle.size()) return true;
    }
    return false;
}

// Whitespace-run word count (ASCII-segmented languages).
std::size_t word_count(std::string_view s) noexcept {
    std::size_t n = 0;
    bool in = false;
    for (char c : s) {
        const bool ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
        if (!ws && !in) { ++n; in = true; }
        else if (ws) in = false;
    }
    return n;
}

// UTF-8 glyph count (lead bytes only) — a script-agnostic length that doesn't
// over-count multibyte characters the way byte length does.
std::size_t glyph_count(std::string_view s) noexcept {
    std::size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;   // not a continuation byte
    return n;
}

// ── Weighted lexical signal sets. Each entry is (needle, weight). Multilingual
//    where it's cheap to be; the structural features carry non-English turns
//    even when these miss. ───────────────────────────────────────────────────
struct Term { std::string_view s; int w; };

// "Hard" vocabulary — design / debugging / cross-cutting / multi-step intent.
constexpr std::array kHard = {
    // English
    Term{"architect", 3},  Term{"design", 2},      Term{"redesign", 3},
    Term{"refactor", 2},   Term{"debug", 2},       Term{"root cause", 3},
    Term{"why ", 2},       Term{" why", 2},        Term{"trade-off", 2},
    Term{"tradeoff", 2},   Term{"strategy", 2},    Term{"approach", 1},
    Term{"end to end", 3}, Term{"end-to-end", 3},  Term{"across the", 2},
    Term{"investigate", 2},Term{"diagnose", 2},    Term{"optimize", 2},
    Term{"optimise", 2},   Term{"race condition", 3}, Term{"deadlock", 3},
    Term{"compare", 2},    Term{"evaluate", 2},    Term{"review", 1},
    Term{"state of the art", 3}, Term{"deep", 1},  Term{"implement all", 2},
    Term{"rewrite", 2},    Term{"migrate", 2},     Term{"whole", 1},
    Term{"entire", 1},     Term{"analyze", 2},     Term{"analyse", 2},
    // Spanish / Portuguese
    Term{"por qu", 2},     Term{"porqu", 2},       Term{"disea", 2},
    Term{"refactor", 2},   Term{"arquitect", 3},   Term{"investiga", 2},
    Term{"depura", 2},     Term{"optimiza", 2},
    // French
    Term{"pourquoi", 2},   Term{"concevoir", 2},   Term{"architectur", 3},
    Term{"refactoris", 2}, Term{"dboguer", 2},     Term{"optimis", 2},
    // German
    Term{"warum", 2},      Term{"entwerf", 2},     Term{"architektur", 3},
    Term{"refaktor", 2},   Term{"debuggen", 2},    Term{"optimier", 2},
    // Italian
    Term{"perch", 2},      Term{"progetta", 2},    Term{"architett", 3},
};

// "Trivial" acknowledgements / one-word imperatives (matched as the WHOLE
// trimmed turn, so they can't false-fire inside a longer sentence).
constexpr std::array kTrivialExact = {
    // English
    std::string_view{"yes"}, std::string_view{"no"}, std::string_view{"ok"},
    std::string_view{"okay"}, std::string_view{"yep"}, std::string_view{"yeah"},
    std::string_view{"thanks"}, std::string_view{"thank you"}, std::string_view{"ty"},
    std::string_view{"go"}, std::string_view{"go ahead"}, std::string_view{"do it"},
    std::string_view{"run it"}, std::string_view{"continue"}, std::string_view{"proceed"},
    std::string_view{"commit"}, std::string_view{"commit it"}, std::string_view{"push"},
    std::string_view{"stop"}, std::string_view{"cancel"}, std::string_view{"sure"},
    std::string_view{"nice"}, std::string_view{"perfect"}, std::string_view{"great"},
    std::string_view{"lgtm"}, std::string_view{"undo"}, std::string_view{"retry"},
    std::string_view{"again"}, std::string_view{"next"},
    // Other languages (common acks)
    std::string_view{"si"}, std::string_view{"oui"}, std::string_view{"ja"},
    std::string_view{"gracias"}, std::string_view{"merci"}, std::string_view{"danke"},
    std::string_view{"vale"}, std::string_view{"claro"}, std::string_view{"bien"},
};

// ── Structural feature extractors (all language-agnostic) ────────────────────

// Enumerated asks: line-leading list markers ("1." "2)" "- " "* ") — a proxy
// for an explicit multi-part request.
std::size_t enumerated_asks(std::string_view s) noexcept {
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const bool bol = (i == 0) || s[i - 1] == '\n';
        if (bol) {
            char c = s[i];
            if (c == '-' || c == '*') ++n;
            else if (c >= '1' && c <= '9'
                     && i + 1 < s.size() && (s[i + 1] == '.' || s[i + 1] == ')'))
                ++n;
        }
    }
    return n;
}

// Clause/conjunction density: sentence/clause separators plus a few common
// coordinating conjunctions. More clauses ⇒ more sub-tasks. Counts ASCII and a
// couple of CJK separators.
std::size_t clause_markers(std::string_view low) noexcept {
    std::size_t n = 0;
    for (std::size_t i = 0; i < low.size(); ++i) {
        char c = low[i];
        if (c == ';' || c == ':') ++n;
        // CJK comma (、 = E3 80 81) / ideographic full stop (。 = E3 80 82)
        else if (static_cast<unsigned char>(c) == 0xE3 && i + 2 < low.size()
                 && static_cast<unsigned char>(low[i + 1]) == 0x80
                 && (static_cast<unsigned char>(low[i + 2]) == 0x81
                     || static_cast<unsigned char>(low[i + 2]) == 0x82)) { ++n; i += 2; }
    }
    // Coordinating conjunctions (word-ish, multilingual, cheap substring).
    for (auto w : {std::string_view{" and "}, std::string_view{" then "},
                   std::string_view{" also "}, std::string_view{" plus "},
                   std::string_view{" y "},    std::string_view{" et "},
                   std::string_view{" und "},  std::string_view{" e "}})
        for (std::size_t i = 0; i + w.size() <= low.size(); ++i)
            if (low.compare(i, w.size(), w) == 0) { ++n; i += w.size() - 1; }
    return n;
}

// Code-token density: fraction (in tenths) of characters that look like code
// (path separators, dotted/underscored idents, brackets, camelCase humps).
int code_density_tenths(std::string_view s) noexcept {
    if (s.empty()) return 0;
    std::size_t codey = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '/' || c == '_' || c == '(' || c == ')' || c == '{' || c == '}'
            || c == '[' || c == ']' || c == '<' || c == '>' || c == '=')
            ++codey;
        else if (c == '.' && i + 1 < s.size() && std::isalnum((unsigned char)s[i + 1]))
            ++codey;   // dotted ident, not sentence period
        else if (c >= 'A' && c <= 'Z' && i > 0
                 && std::islower((unsigned char)s[i - 1]))
            ++codey;   // camelCase hump
    }
    return static_cast<int>(codey * 10 / s.size());
}

} // namespace

ComplexityScore classify_score(std::string_view text, int complex_min) noexcept {
    // Trim.
    std::size_t b = 0, e = text.size();
    while (b < e && std::isspace((unsigned char)text[b])) ++b;
    while (e > b && std::isspace((unsigned char)text[e - 1])) --e;
    std::string_view trimmed = text.substr(b, e - b);

    if (trimmed.empty())
        return {Complexity::Trivial, -100, 100};

    std::string low;
    low.reserve(trimmed.size());
    for (char c : trimmed) low.push_back(lower(c));

    const std::size_t words  = word_count(trimmed);
    const std::size_t glyphs = glyph_count(trimmed);
    const bool has_q         = trimmed.find('?') != std::string_view::npos
                            || has(low, "\xEF\xBC\x9F");   // fullwidth ？
    const std::size_t enums  = enumerated_asks(trimmed);
    const std::size_t clauses= clause_markers(low);
    const int code_tenths    = code_density_tenths(trimmed);

    // ── TRIVIAL fast path: the WHOLE turn is a known ack (any language), short,
    //    not a question. This is the one hard rule — an ack is an ack. ─────────
    if (words <= 3 && glyphs <= 24 && !has_q) {
        for (auto t : kTrivialExact)
            if (low == t)
                return {Complexity::Trivial, -100, 100};
    }

    // ── Additive score. Structure dominates; lexicon and morphology refine. ──
    int score = 0;

    // Size (glyph-based ⇒ script-agnostic). Saturating bands.
    if (glyphs >= 400)      score += 4;
    else if (glyphs >= 220) score += 3;
    else if (glyphs >= 120) score += 2;
    else if (glyphs >= 60)  score += 1;
    else if (glyphs <= 18)  score -= 2;   // very short ⇒ likely simple

    // Word count (English-ish) reinforces size where spaces exist.
    if (words >= 45)      score += 2;
    else if (words >= 24) score += 1;
    else if (words <= 5)  score -= 1;

    // Explicit multi-part structure is the strongest single signal.
    if (enums >= 3)      score += 4;
    else if (enums == 2) score += 2;
    else if (enums == 1) score += 1;

    // Clause density ⇒ multiple sub-tasks.
    if (clauses >= 4)      score += 3;
    else if (clauses >= 2) score += 2;
    else if (clauses == 1) score += 1;

    // Code-token density ⇒ touches structure (but a wall of code alone isn't
    // necessarily *hard* thinking, so cap its contribution).
    if (code_tenths >= 3)      score += 2;
    else if (code_tenths >= 1) score += 1;

    // Question shape: a question is usually explain/why work — mild bump.
    if (has_q) score += 1;

    // Lexical evidence (weighted, multilingual, additive — never an override).
    // Cap total lexical contribution so a keyword-stuffed short turn can't leap
    // straight to the top on vocabulary alone.
    int lex = 0;
    for (auto t : kHard)
        if (has(low, t.s)) { lex += t.w; if (lex >= 6) { lex = 6; break; } }
    score += lex;

    // ── Threshold into tiers, with margin to the nearest boundary. The Complex
    //    cut is user-tunable (settings row "smart.complex_threshold", or
    //    AGENTTY_SMART_COMPLEX_THRESHOLD): Standard is the band just below it,
    //    Simple everything at or under that. Ties break upward via the
    //    boundary placement. Passed in rather than read from the environment
    //    here — see the declaration. ──────────────────────────────────────
    const int kComplexMin  = complex_min;                  // >= this ⇒ Complex
    const int kSimpleMax   = kComplexMin - 3;               // <= this ⇒ Simple
                                                            // (Standard is the
                                                            //  2-wide band between)

    Complexity tier;
    int margin;
    if (score <= kSimpleMax) {
        tier   = Complexity::Simple;
        margin = kSimpleMax - score;                 // how far below the boundary
    } else if (score < kComplexMin) {
        tier   = Complexity::Standard;
        margin = std::min(score - kSimpleMax, kComplexMin - score);
    } else {
        tier   = Complexity::Complex;
        margin = score - kComplexMin;
    }
    return {tier, score, margin};
}

Complexity classify_complexity(std::string_view text) noexcept {
    return classify_score(text).tier;
}

bool is_routing_correction(std::string_view text) noexcept {
    // Lowercase only the opening — the sentiment lives at the start of a
    // follow-up, and a bounded scan keeps this cheap on the hot reduce path.
    std::string low;
    low.reserve(48);
    for (char c : text) {
        if (low.size() >= 48) break;
        low.push_back((c >= 'A' && c <= 'Z') ? char(c + 32) : c);
    }
    auto starts = [&](std::string_view p){ return low.rfind(p, 0) == 0; };
    auto has    = [&](std::string_view p){
        return low.find(p) != std::string_view::npos; };

    // Explicit praise vetoes a correction outright ("actually that's perfect").
    const bool positive =
           has("perfect") || has("great") || has("thanks") || has("thank you")
        || has("looks good") || has("lgtm") || has("nice") || has("works now")
        || has("that works") || has("awesome") || has("exactly");
    if (positive) return false;

    // Unambiguous dissatisfaction — matches wherever it appears in the opener.
    const bool hard_negative =
           starts("that's wrong") || starts("thats wrong")
        || starts("that's not")  || starts("thats not")
        || starts("that broke")  || starts("you broke")
        || starts("undo") || starts("revert") || starts("not quite")
        || has("doesn't work") || has("does not work") || has("didn't work")
        || has("still broken") || has("still fail") || has("still doesn't")
        || has("that's incorrect") || has("is wrong") || has("was wrong");
    if (hard_negative) return true;

    // Soft openers count ONLY with a corroborating negative cue — so a bare
    // "actually, also add tests" (additive) or "no worries" doesn't ratchet
    // the prior. "wrong" is intentionally NOT a soft opener ("wrong file" is a
    // redirection); genuine wrong-ness is in hard_negative above.
    const bool soft_opener =
           starts("no,") || starts("no ") || low == "no" || starts("nope")
        || starts("actually");
    const bool soft_negative_cue =
           has(" not ") || has("n't") || has("broke") || has("fail")
        || has("error") || has("still") || has("instead") || has("bug");
    return soft_opener && soft_negative_cue;
}

bool is_continuation_cue(std::string_view text) noexcept {
    // Trim + lowercase the (short) turn. Only ack-length turns qualify —
    // anything longer carries its own signal and classify_score handles it.
    std::size_t b = 0, e = text.size();
    while (b < e && std::isspace((unsigned char)text[b])) ++b;
    while (e > b && std::isspace((unsigned char)text[e - 1])) --e;
    std::string_view t = text.substr(b, e - b);
    if (t.empty() || t.size() > 24) return false;
    std::string low;
    low.reserve(t.size());
    for (char c : t) low.push_back(lower(c));
    // Commands that RESUME work (vs terminal acks like "thanks"/"lgtm").
    // "commit"/"push" are deliberately absent: they end a task with a cheap
    // mechanical action, which Trivial routes correctly.
    for (std::string_view cue : {
             std::string_view{"continue"}, std::string_view{"go on"},
             std::string_view{"keep going"}, std::string_view{"go ahead"},
             std::string_view{"do it"}, std::string_view{"proceed"},
             std::string_view{"retry"}, std::string_view{"try again"},
             std::string_view{"again"}, std::string_view{"next"},
             std::string_view{"run it"}, std::string_view{"finish it"},
             std::string_view{"keep at it"}})
        if (low == cue) return true;
    return false;
}

} // namespace agentty::smart
